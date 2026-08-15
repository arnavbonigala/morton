#pragma once
#include <netinet/in.h>

#include <cstring>
#include <string>

#include "core/types.h"

namespace morton {

/// IPv4 endpoint with value semantics so it can key a hash map of connections.
struct Address {
    u32 ip = 0;
    u16 port = 0;

    Address() = default;
    Address(u32 ip_host_order, u16 port_host_order) : ip(ip_host_order), port(port_host_order) {}

    static Address from_sockaddr(const sockaddr_in& sa) {
        Address a;
        a.ip = ntohl(sa.sin_addr.s_addr);
        a.port = ntohs(sa.sin_port);
        return a;
    }

    sockaddr_in to_sockaddr() const {
        sockaddr_in sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(ip);
        sa.sin_port = htons(port);
        return sa;
    }

    /// Parses "host:port"; resolves hostnames through getaddrinfo.
    static bool parse(const std::string& text, Address* out);

    std::string to_string() const;

    bool valid() const { return port != 0; }

    bool operator==(const Address& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const Address& o) const { return !(*this == o); }
};

struct AddressHash {
    std::size_t operator()(const Address& a) const {
        return (static_cast<std::size_t>(a.ip) << 16) ^ a.port;
    }
};

}  // namespace morton
