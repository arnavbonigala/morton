#include "net/http_client.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace morton {

HttpFetch http_fetch(const Address& server, const std::string& method, const std::string& target,
                     const std::string& body, u32 timeout_ms) {
    HttpFetch result;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        result.error = std::strerror(errno);
        return result;
    }

    timeval tv;
    tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    sockaddr_in sa = server.to_sockaddr();
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        result.error = std::strerror(errno);
        ::close(fd);
        return result;
    }

    std::string request = method + " " + target + " HTTP/1.1\r\n";
    request += "Host: " + server.to_string() + "\r\n";
    request += "Connection: close\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    request += body;

    std::size_t sent = 0;
    while (sent < request.size()) {
        ssize_t wrote = ::send(fd, request.data() + sent, request.size() - sent, 0);
        if (wrote <= 0) {
            result.error = "send failed";
            ::close(fd);
            return result;
        }
        sent += static_cast<std::size_t>(wrote);
    }

    std::string response;
    char chunk[8192];
    while (response.size() < 1024 * 1024) {
        ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
        if (got <= 0) break;
        response.append(chunk, static_cast<std::size_t>(got));
    }
    ::close(fd);

    std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos || response.size() < 12) {
        result.error = "malformed response";
        return result;
    }

    result.status = static_cast<int>(std::strtol(response.c_str() + 9, nullptr, 10));
    result.body = response.substr(header_end + 4);
    result.ok = result.status >= 200 && result.status < 300;
    return result;
}

std::string json_lookup(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    std::size_t start = json.find(needle);
    if (start == std::string::npos) return "";
    start += needle.size();
    if (start >= json.size()) return "";
    if (json[start] == '"') {
        std::size_t end = json.find('"', start + 1);
        if (end == std::string::npos) return "";
        return json.substr(start + 1, end - start - 1);
    }
    std::size_t end = json.find_first_of(",}", start);
    if (end == std::string::npos) end = json.size();
    return json.substr(start, end - start);
}

}  // namespace morton
