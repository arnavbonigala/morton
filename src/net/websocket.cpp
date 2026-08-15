#include "net/websocket.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstring>

#include "core/log.h"

namespace morton {
namespace {

constexpr const char* kMagicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t kMaxHandshakeBytes = 8192;
constexpr std::size_t kMaxIncomingBytes = 64 * 1024;
constexpr std::size_t kMaxOutgoingBytes = 4 * 1024 * 1024;

u32 rotate_left(u32 value, u32 bits) { return (value << bits) | (value >> (32 - bits)); }

void sha1(const u8* data, std::size_t size, u8 out[20]) {
    u32 h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};

    std::vector<u8> message(data, data + size);
    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0);
    u64 bits = static_cast<u64>(size) * 8;
    for (int i = 7; i >= 0; --i) message.push_back(static_cast<u8>((bits >> (i * 8)) & 0xff));

    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        u32 w[80];
        for (u32 i = 0; i < 16; ++i) {
            const u8* p = message.data() + offset + i * 4;
            w[i] = (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
                   (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
        }
        for (u32 i = 16; i < 80; ++i) {
            w[i] = rotate_left(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (u32 i = 0; i < 80; ++i) {
            u32 f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcu;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6u;
            }
            u32 temp = rotate_left(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotate_left(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    for (u32 i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<u8>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<u8>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<u8>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<u8>(h[i]);
    }
}

std::string header_value(const std::string& request, const std::string& name) {
    std::string lowered;
    lowered.reserve(request.size());
    for (char c : request) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    std::string needle = "\r\n" + name + ":";
    std::size_t start = lowered.find(needle);
    if (start == std::string::npos) return "";
    start += needle.size();
    std::size_t end = request.find("\r\n", start);
    if (end == std::string::npos) return "";

    std::string value = request.substr(start, end - start);
    std::size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    std::size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

std::string base64_encode(const u8* data, std::size_t size) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        u32 chunk = static_cast<u32>(data[i]) << 16;
        if (i + 1 < size) chunk |= static_cast<u32>(data[i + 1]) << 8;
        if (i + 2 < size) chunk |= static_cast<u32>(data[i + 2]);

        out.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
        out.push_back(i + 1 < size ? kAlphabet[(chunk >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < size ? kAlphabet[chunk & 0x3f] : '=');
    }
    return out;
}

std::string websocket_accept_key(const std::string& client_key) {
    std::string combined = client_key + kMagicGuid;
    u8 digest[20];
    sha1(reinterpret_cast<const u8*>(combined.data()), combined.size(), digest);
    return base64_encode(digest, sizeof(digest));
}

std::string encode_text_frame(const std::string& payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));

    std::size_t size = payload.size();
    if (size < 126) {
        frame.push_back(static_cast<char>(size));
    } else if (size <= 0xffff) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((size >> 8) & 0xff));
        frame.push_back(static_cast<char>(size & 0xff));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((static_cast<u64>(size) >> (i * 8)) & 0xff));
        }
    }
    frame += payload;
    return frame;
}

WebSocketServer::~WebSocketServer() { stop(); }

bool WebSocketServer::start(const WebSocketConfig& config) {
    config_ = config;

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        MORTON_LOG_ERROR("websocket socket() failed: %s", std::strerror(errno));
        return false;
    }

    int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in sa = config.bind.to_sockaddr();
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        MORTON_LOG_ERROR("websocket bind(%s) failed: %s", config.bind.to_string().c_str(),
                         std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, 32) < 0) {
        MORTON_LOG_ERROR("websocket listen failed: %s", std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    socklen_t length = sizeof(sa);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&sa), &length) == 0) {
        local_ = Address::from_sockaddr(sa);
    } else {
        local_ = config.bind;
    }

    set_nonblocking(listen_fd_);
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this] { serve(); });
    MORTON_LOG_INFO("websocket listening on %s", local_.to_string().c_str());
    return true;
}

void WebSocketServer::stop() {
    if (!running_.exchange(false, std::memory_order_relaxed)) return;
    if (thread_.joinable()) thread_.join();
    for (Peer& peer : peers_) {
        if (peer.fd >= 0) ::close(peer.fd);
    }
    peers_.clear();
    clients_.store(0, std::memory_order_relaxed);
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    std::lock_guard<std::mutex> guard(queue_mutex_);
    queue_.clear();
}

void WebSocketServer::publish(const std::string& text) {
    if (!running_.load(std::memory_order_relaxed)) return;
    if (clients_.load(std::memory_order_relaxed) == 0) return;

    std::lock_guard<std::mutex> guard(queue_mutex_);
    queue_.push_back(text);
    while (queue_.size() > config_.max_queued_frames) {
        queue_.pop_front();
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool WebSocketServer::handshake(int fd) {
    std::string request;
    pollfd waiter{fd, POLLIN, 0};

    while (request.find("\r\n\r\n") == std::string::npos) {
        if (request.size() > kMaxHandshakeBytes) return false;
        int ready = ::poll(&waiter, 1, 500);
        if (ready <= 0) return false;

        char buffer[2048];
        ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0) return false;
        request.append(buffer, static_cast<std::size_t>(received));
    }

    if (request.compare(0, 4, "GET ") != 0) return false;
    std::string key = header_value(request, "sec-websocket-key");
    if (key.empty()) return false;

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        websocket_accept_key(key) + "\r\n\r\n";

    return ::send(fd, response.data(), response.size(), 0) ==
           static_cast<ssize_t>(response.size());
}

void WebSocketServer::accept_peer() {
    sockaddr_in from{};
    socklen_t length = sizeof(from);
    int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&from), &length);
    if (fd < 0) return;

    if (peers_.size() >= config_.max_clients || !handshake(fd)) {
        ::close(fd);
        return;
    }

    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    set_nonblocking(fd);

    Peer peer;
    peer.fd = fd;
    peers_.push_back(std::move(peer));
    clients_.store(static_cast<u32>(peers_.size()), std::memory_order_relaxed);
}

void WebSocketServer::drain(Peer& peer) {
    char buffer[2048];
    while (true) {
        ssize_t received = ::recv(peer.fd, buffer, sizeof(buffer), 0);
        if (received == 0) {
            peer.closing = true;
            return;
        }
        if (received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) peer.closing = true;
            return;
        }
        peer.incoming.append(buffer, static_cast<std::size_t>(received));
        if (peer.incoming.size() > kMaxIncomingBytes) {
            peer.closing = true;
            return;
        }
        if (peer.incoming.size() >= 1 &&
            (static_cast<u8>(peer.incoming[0]) & 0x0f) == 0x8) {
            peer.closing = true;
            return;
        }
        peer.incoming.clear();
    }
}

void WebSocketServer::pump(Peer& peer) {
    while (!peer.outgoing.empty()) {
        ssize_t sent = ::send(peer.fd, peer.outgoing.data(), peer.outgoing.size(), 0);
        if (sent > 0) {
            peer.outgoing.erase(0, static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        peer.closing = true;
        return;
    }
}

void WebSocketServer::serve() {
    while (running_.load(std::memory_order_relaxed)) {
        std::vector<pollfd> waiters;
        waiters.push_back(pollfd{listen_fd_, POLLIN, 0});
        for (const Peer& peer : peers_) {
            short events = POLLIN;
            if (!peer.outgoing.empty()) events |= POLLOUT;
            waiters.push_back(pollfd{peer.fd, events, 0});
        }

        ::poll(waiters.data(), waiters.size(), 20);

        if ((waiters[0].revents & POLLIN) != 0) accept_peer();

        std::vector<std::string> frames;
        {
            std::lock_guard<std::mutex> guard(queue_mutex_);
            while (!queue_.empty()) {
                frames.push_back(encode_text_frame(queue_.front()));
                queue_.pop_front();
            }
        }

        for (std::size_t i = 0; i < peers_.size(); ++i) {
            Peer& peer = peers_[i];
            if (i + 1 < waiters.size() && (waiters[i + 1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                drain(peer);
            }
            if (peer.closing) continue;

            for (const std::string& frame : frames) {
                if (peer.outgoing.size() + frame.size() > kMaxOutgoingBytes) {
                    frames_dropped_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                peer.outgoing += frame;
                frames_sent_.fetch_add(1, std::memory_order_relaxed);
            }
            pump(peer);
        }

        for (std::size_t i = peers_.size(); i-- > 0;) {
            if (!peers_[i].closing) continue;
            ::close(peers_[i].fd);
            peers_.erase(peers_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        clients_.store(static_cast<u32>(peers_.size()), std::memory_order_relaxed);
    }
}

}  // namespace morton
