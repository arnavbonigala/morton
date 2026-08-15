#pragma once
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "net/address.h"

namespace morton_test {

/// A browser-shaped WebSocket client: real handshake, real frame parsing.
class WsClient {
public:
    ~WsClient() { close(); }

    bool connect(const morton::Address& address, const std::string& key = "dGhlIHNhbXBsZSBub25jZQ==") {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        sockaddr_in sa = address.to_sockaddr();
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
            close();
            return false;
        }

        int nodelay = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        std::string request =
            "GET /view HTTP/1.1\r\nHost: " + address.to_string() +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
            "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (::send(fd_, request.data(), request.size(), 0) < 0) {
            close();
            return false;
        }

        std::string response;
        while (response.find("\r\n\r\n") == std::string::npos) {
            char buffer[1024];
            ssize_t received = read_with_timeout(buffer, sizeof(buffer), 2000);
            if (received <= 0) {
                close();
                return false;
            }
            response.append(buffer, static_cast<std::size_t>(received));
        }
        handshake_response_ = response;
        return response.find("101") != std::string::npos;
    }

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    void send_close() {
        if (fd_ < 0) return;
        const unsigned char frame[6] = {0x88, 0x80, 0, 0, 0, 0};
        ::send(fd_, frame, sizeof(frame), 0);
    }

    /// Reads one text frame, returning false on timeout.
    bool receive(std::string* out, int timeout_ms = 3000) {
        while (true) {
            std::string message;
            if (take_frame(&message)) {
                *out = message;
                return true;
            }
            char buffer[4096];
            ssize_t received = read_with_timeout(buffer, sizeof(buffer), timeout_ms);
            if (received <= 0) return false;
            incoming_.append(buffer, static_cast<std::size_t>(received));
        }
    }

    const std::string& handshake_response() const { return handshake_response_; }

private:
    ssize_t read_with_timeout(char* buffer, std::size_t size, int timeout_ms) {
        pollfd waiter{fd_, POLLIN, 0};
        if (::poll(&waiter, 1, timeout_ms) <= 0) return -1;
        return ::recv(fd_, buffer, size, 0);
    }

    bool take_frame(std::string* out) {
        if (incoming_.size() < 2) return false;
        auto byte = [this](std::size_t i) { return static_cast<unsigned char>(incoming_[i]); };

        std::size_t header = 2;
        std::size_t length = byte(1) & 0x7f;
        if (length == 126) {
            if (incoming_.size() < 4) return false;
            length = (static_cast<std::size_t>(byte(2)) << 8) | byte(3);
            header = 4;
        } else if (length == 127) {
            if (incoming_.size() < 10) return false;
            length = 0;
            for (std::size_t i = 0; i < 8; ++i) length = (length << 8) | byte(2 + i);
            header = 10;
        }
        if ((byte(1) & 0x80) != 0) header += 4;
        if (incoming_.size() < header + length) return false;

        *out = incoming_.substr(header, length);
        incoming_.erase(0, header + length);
        return true;
    }

    int fd_ = -1;
    std::string incoming_;
    std::string handshake_response_;
};

}  // namespace morton_test
