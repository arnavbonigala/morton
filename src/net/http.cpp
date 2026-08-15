#include "net/http.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "core/log.h"

namespace morton {
namespace {

constexpr std::size_t kMaxRequestBytes = 256 * 1024;
constexpr int kClientTimeoutMs = 5000;

const char* status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "OK";
    }
}

bool read_line(const std::string& buffer, std::size_t* cursor, std::string* out) {
    std::size_t end = buffer.find("\r\n", *cursor);
    if (end == std::string::npos) return false;
    *out = buffer.substr(*cursor, end - *cursor);
    *cursor = end + 2;
    return true;
}

}  // namespace

std::string url_decode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out.push_back(' ');
        } else if (text[i] == '%' && i + 2 < text.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int high = hex(text[i + 1]);
            int low = hex(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>(high * 16 + low));
                i += 2;
                continue;
            }
            out.push_back(text[i]);
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(const Address& bind) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        MORTON_LOG_ERROR("http socket() failed: %s", std::strerror(errno));
        return false;
    }

    int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in sa = bind.to_sockaddr();
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        MORTON_LOG_ERROR("http bind(%s) failed: %s", bind.to_string().c_str(),
                         std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, 512) < 0) {
        MORTON_LOG_ERROR("http listen failed: %s", std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    sockaddr_in actual;
    socklen_t actual_len = sizeof(actual);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&actual), &actual_len) == 0) {
        local_ = Address::from_sockaddr(actual);
    } else {
        local_ = bind;
    }

    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this] { serve(); });
    MORTON_LOG_INFO("http listening on %s", local_.to_string().c_str());
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false, std::memory_order_relaxed)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
}

void HttpServer::route(std::string method, std::string path, Handler handler) {
    routes_[method + " " + path] = std::move(handler);
}

void HttpServer::serve() {
    while (running_.load(std::memory_order_relaxed)) {
        pollfd pfd;
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ready = ::poll(&pfd, 1, 200);
        if (ready <= 0) continue;
        if (!running_.load(std::memory_order_relaxed)) break;

        int client_fd = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) continue;

        handle_client(client_fd);
        ::close(client_fd);
    }
}

void HttpServer::handle_client(int client_fd) {
    int nodelay = 1;
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    std::string buffer;
    char chunk[8192];
    std::size_t header_end = std::string::npos;

    while (buffer.size() < kMaxRequestBytes) {
        pollfd pfd;
        pfd.fd = client_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (::poll(&pfd, 1, kClientTimeoutMs) <= 0) return;

        ssize_t received = ::recv(client_fd, chunk, sizeof(chunk), 0);
        if (received <= 0) return;
        buffer.append(chunk, static_cast<std::size_t>(received));

        header_end = buffer.find("\r\n\r\n");
        if (header_end == std::string::npos) continue;

        std::size_t content_length = 0;
        std::size_t marker = buffer.find("Content-Length:");
        if (marker == std::string::npos) marker = buffer.find("content-length:");
        if (marker != std::string::npos && marker < header_end) {
            content_length = static_cast<std::size_t>(
                std::strtoul(buffer.c_str() + marker + 15, nullptr, 10));
        }
        if (buffer.size() >= header_end + 4 + content_length) break;
    }

    if (header_end == std::string::npos) return;

    HttpRequest request;
    std::size_t cursor = 0;
    std::string line;
    if (!read_line(buffer, &cursor, &line)) return;

    std::istringstream request_line(line);
    std::string target;
    request_line >> request.method >> target;

    std::size_t question = target.find('?');
    if (question == std::string::npos) {
        request.path = url_decode(target);
    } else {
        request.path = url_decode(target.substr(0, question));
        std::string query = target.substr(question + 1);
        std::size_t start = 0;
        while (start <= query.size()) {
            std::size_t amp = query.find('&', start);
            std::string pair = query.substr(start, amp == std::string::npos ? std::string::npos
                                                                            : amp - start);
            std::size_t equals = pair.find('=');
            if (!pair.empty()) {
                if (equals == std::string::npos) {
                    request.query[url_decode(pair)] = "";
                } else {
                    request.query[url_decode(pair.substr(0, equals))] =
                        url_decode(pair.substr(equals + 1));
                }
            }
            if (amp == std::string::npos) break;
            start = amp + 1;
        }
    }

    while (read_line(buffer, &cursor, &line) && !line.empty()) {
        std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::size_t value_start = line.find_first_not_of(" \t", colon + 1);
        std::string value = value_start == std::string::npos ? "" : line.substr(value_start);
        for (char& c : key) c = static_cast<char>(std::tolower(c));
        request.headers[key] = value;
    }

    if (header_end + 4 < buffer.size()) request.body = buffer.substr(header_end + 4);

    HttpResponse response = dispatch(request);
    requests_served_.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << " " << status_text(response.status) << "\r\n";
    out << "Content-Type: " << response.content_type << "\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Connection: close\r\n";
    for (const auto& [key, value] : response.headers) out << key << ": " << value << "\r\n";
    out << "\r\n" << response.body;

    std::string payload = out.str();
    std::size_t sent = 0;
    while (sent < payload.size()) {
        ssize_t wrote = ::send(client_fd, payload.data() + sent, payload.size() - sent, 0);
        if (wrote <= 0) break;
        sent += static_cast<std::size_t>(wrote);
    }
}

HttpResponse HttpServer::dispatch(const HttpRequest& request) {
    auto it = routes_.find(request.method + " " + request.path);
    if (it != routes_.end()) return it->second(request);
    if (fallback_) return fallback_(request);
    return HttpResponse::not_found();
}

}  // namespace morton
