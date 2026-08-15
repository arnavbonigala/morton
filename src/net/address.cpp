#include "net/address.h"

#include <arpa/inet.h>
#include <netdb.h>

#include <cstdio>
#include <cstdlib>

namespace morton {

bool Address::parse(const std::string& text, Address* out) {
    std::size_t colon = text.rfind(':');
    if (colon == std::string::npos) return false;

    std::string host = text.substr(0, colon);
    std::string port_text = text.substr(colon + 1);
    if (host.empty() || port_text.empty()) return false;

    char* end = nullptr;
    long port = std::strtol(port_text.c_str(), &end, 10);
    if (end == port_text.c_str() || *end != '\0' || port <= 0 || port > 65535) return false;

    if (host == "*" || host == "0.0.0.0") {
        *out = Address(INADDR_ANY, static_cast<u16>(port));
        return true;
    }

    in_addr numeric;
    if (inet_pton(AF_INET, host.c_str(), &numeric) == 1) {
        *out = Address(ntohl(numeric.s_addr), static_cast<u16>(port));
        return true;
    }

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 || results == nullptr) {
        return false;
    }

    const sockaddr_in* resolved = reinterpret_cast<const sockaddr_in*>(results->ai_addr);
    *out = Address(ntohl(resolved->sin_addr.s_addr), static_cast<u16>(port));
    freeaddrinfo(results);
    return true;
}

std::string Address::to_string() const {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u:%u", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                  (ip >> 8) & 0xff, ip & 0xff, port);
    return buffer;
}

}  // namespace morton
