#include "cluster/redis.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "core/log.h"

namespace morton {
namespace {

constexpr std::size_t kMaxBulkSize = 64 * 1024 * 1024;
constexpr std::size_t kCompactThreshold = 1 << 16;

}  // namespace

std::string encode_resp_command(const std::vector<std::string>& args) {
    std::string out;
    out += "*" + std::to_string(args.size()) + "\r\n";
    for (const std::string& arg : args) {
        out += "$" + std::to_string(arg.size()) + "\r\n";
        out += arg;
        out += "\r\n";
    }
    return out;
}

RedisClient::~RedisClient() { disconnect(); }

bool RedisClient::open_socket() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        last_error_ = std::strerror(errno);
        return false;
    }

    timeval tv;
    tv.tv_sec = static_cast<time_t>(timeout_ms_ / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout_ms_ % 1000) * 1000);
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int nodelay = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    sockaddr_in sa = address_.to_sockaddr();
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        last_error_ = std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    buffer_.clear();
    cursor_ = 0;
    return true;
}

bool RedisClient::connect(const Address& address, u32 timeout_ms) {
    disconnect();
    address_ = address;
    timeout_ms_ = timeout_ms;
    if (!open_socket()) {
        MORTON_LOG_DEBUG("redis connect %s failed: %s", address.to_string().c_str(),
                         last_error_.c_str());
        return false;
    }
    return true;
}

void RedisClient::disconnect() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    buffer_.clear();
    cursor_ = 0;
    pipeline_.clear();
    queued_count_ = 0;
}

bool RedisClient::send_all(const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        ssize_t wrote = ::send(fd_, data + sent, size - sent, 0);
        if (wrote <= 0) {
            if (wrote < 0 && errno == EINTR) continue;
            last_error_ = wrote == 0 ? "connection closed" : std::strerror(errno);
            return false;
        }
        sent += static_cast<std::size_t>(wrote);
    }
    return true;
}

bool RedisClient::fill_buffer() {
    char chunk[16384];
    ssize_t got = ::recv(fd_, chunk, sizeof(chunk), 0);
    if (got <= 0) {
        if (got < 0 && errno == EINTR) return fill_buffer();
        last_error_ = got == 0 ? "connection closed" : std::strerror(errno);
        return false;
    }
    if (cursor_ > kCompactThreshold) {
        buffer_.erase(0, cursor_);
        cursor_ = 0;
    }
    buffer_.append(chunk, static_cast<std::size_t>(got));
    return true;
}

bool RedisClient::read_line(std::string* out) {
    while (true) {
        std::size_t end = buffer_.find("\r\n", cursor_);
        if (end != std::string::npos) {
            *out = buffer_.substr(cursor_, end - cursor_);
            cursor_ = end + 2;
            return true;
        }
        if (!fill_buffer()) return false;
    }
}

bool RedisClient::read_exact(std::size_t count, std::string* out) {
    while (buffer_.size() - cursor_ < count) {
        if (!fill_buffer()) return false;
    }
    *out = buffer_.substr(cursor_, count);
    cursor_ += count;
    return true;
}

bool RedisClient::read_reply(RedisReply* out) {
    std::string line;
    if (!read_line(&line)) return false;
    if (line.empty()) {
        last_error_ = "empty reply line";
        return false;
    }

    char tag = line[0];
    std::string rest = line.substr(1);

    switch (tag) {
        case '+':
            out->type = RedisType::kStatus;
            out->str = rest;
            return true;
        case '-':
            out->type = RedisType::kError;
            out->str = rest;
            return true;
        case ':':
            out->type = RedisType::kInteger;
            out->integer = std::strtoll(rest.c_str(), nullptr, 10);
            return true;
        case '$': {
            i64 length = std::strtoll(rest.c_str(), nullptr, 10);
            if (length < 0) {
                out->type = RedisType::kNil;
                return true;
            }
            if (static_cast<std::size_t>(length) > kMaxBulkSize) {
                last_error_ = "bulk reply exceeds cap";
                return false;
            }
            std::string payload;
            if (!read_exact(static_cast<std::size_t>(length) + 2, &payload)) return false;
            out->type = RedisType::kString;
            out->str = payload.substr(0, static_cast<std::size_t>(length));
            return true;
        }
        case '*': {
            i64 count = std::strtoll(rest.c_str(), nullptr, 10);
            if (count < 0) {
                out->type = RedisType::kNil;
                return true;
            }
            out->type = RedisType::kArray;
            out->elements.resize(static_cast<std::size_t>(count));
            for (i64 i = 0; i < count; ++i) {
                if (!read_reply(&out->elements[static_cast<std::size_t>(i)])) return false;
            }
            return true;
        }
        default:
            last_error_ = std::string("unknown reply tag '") + tag + "'";
            return false;
    }
}

void RedisClient::queue(const std::vector<std::string>& args) {
    pipeline_ += encode_resp_command(args);
    ++queued_count_;
}

bool RedisClient::flush(std::vector<RedisReply>* out) {
    if (queued_count_ == 0) {
        if (out) out->clear();
        return true;
    }

    std::string payload;
    payload.swap(pipeline_);
    std::size_t expected = queued_count_;
    queued_count_ = 0;

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (fd_ < 0) {
            if (!open_socket()) break;
            ++reconnects_;
        }

        if (send_all(payload.data(), payload.size())) {
            std::vector<RedisReply> replies;
            replies.reserve(expected);
            bool complete = true;
            for (std::size_t i = 0; i < expected; ++i) {
                RedisReply reply;
                if (!read_reply(&reply)) {
                    complete = false;
                    break;
                }
                replies.push_back(std::move(reply));
            }
            if (complete) {
                commands_sent_ += expected;
                if (out) *out = std::move(replies);
                return true;
            }
        }

        ::close(fd_);
        fd_ = -1;
        buffer_.clear();
        cursor_ = 0;
    }

    if (out) {
        out->assign(expected, RedisReply::error("transport: " + last_error_));
    }
    return false;
}

RedisReply RedisClient::command(const std::vector<std::string>& args) {
    queue(args);
    std::vector<RedisReply> replies;
    if (!flush(&replies) || replies.empty()) {
        return RedisReply::error("transport: " + last_error_);
    }
    return std::move(replies.front());
}

}  // namespace morton
