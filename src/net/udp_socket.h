#pragma once
#include <string>

#include "core/types.h"
#include "net/address.h"

namespace morton {

constexpr u32 kMaxDatagramSize = 1200;

/// Non-blocking IPv4 UDP socket.
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    /// Binds to the address. Port 0 requests an ephemeral port, readable via local_address().
    bool open(const Address& bind_address, u32 buffer_bytes = 8 * 1024 * 1024);
    void close();
    bool is_open() const { return fd_ >= 0; }

    /// Returns false when the datagram was dropped by the kernel buffer being full.
    bool send(const Address& to, const u8* data, u32 size);

    /// Returns bytes received, 0 when no datagram is pending, -1 on a real error.
    int receive(Address* from, u8* buffer, u32 capacity);

    /// Blocks up to timeout_us waiting for readability. Returns true if readable.
    bool wait_readable(u64 timeout_us) const;

    Address local_address() const { return local_; }
    int fd() const { return fd_; }

    u64 bytes_sent() const { return bytes_sent_; }
    u64 bytes_received() const { return bytes_received_; }
    u64 packets_sent() const { return packets_sent_; }
    u64 packets_received() const { return packets_received_; }
    u64 send_failures() const { return send_failures_; }

private:
    int fd_ = -1;
    Address local_;
    u64 bytes_sent_ = 0;
    u64 bytes_received_ = 0;
    u64 packets_sent_ = 0;
    u64 packets_received_ = 0;
    u64 send_failures_ = 0;
};

}  // namespace morton
