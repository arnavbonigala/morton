#pragma once
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

#include "net/address.h"

namespace morton_test {

/// Minimal blocking HTTP client so tests exercise the server over a real socket.
inline std::string http_request(const morton::Address& server, const std::string& method,
                                const std::string& target, const std::string& body = "") {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";
    sockaddr_in sa = server.to_sockaddr();
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        ::close(fd);
        return "";
    }

    std::string request = method + " " + target + " HTTP/1.1\r\nHost: localhost\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    ::send(fd, request.data(), request.size(), 0);

    std::string response;
    char chunk[4096];
    while (true) {
        ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
        if (got <= 0) break;
        response.append(chunk, static_cast<std::size_t>(got));
    }
    ::close(fd);
    return response;
}

inline std::string http_body(const std::string& response) {
    std::size_t end = response.find("\r\n\r\n");
    return end == std::string::npos ? "" : response.substr(end + 4);
}

/// Extracts a flat JSON field value without pulling in a JSON parser.
inline std::string json_field(const std::string& json, const std::string& key) {
    std::size_t start = json.find("\"" + key + "\":");
    if (start == std::string::npos) return "";
    start += key.size() + 3;
    if (start >= json.size()) return "";
    if (json[start] == '"') {
        std::size_t end = json.find('"', start + 1);
        return json.substr(start + 1, end - start - 1);
    }
    std::size_t end = json.find_first_of(",}", start);
    return json.substr(start, end - start);
}

}  // namespace morton_test
