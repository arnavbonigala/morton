#pragma once
#include <netinet/in.h>

#include <string>
#include <vector>

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

    /// Copies the datagram into the pending batch, which leaves the socket
    /// untouched until flush_sends(). Ordering is preserved, so a peer sees
    /// batched datagrams in the order they were queued.
    bool send_batched(const Address& to, const u8* data, u32 size);

    /// Hands the pending batch to the kernel in as few syscalls as the
    /// platform allows.
    void flush_sends();

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

    /// Datagrams the kernel discarded because the receive queue was full.
    ///
    /// These never reach receive(), so without this counter an overflowing
    /// socket is indistinguishable from a quiet network: the peer's packets
    /// simply go unacknowledged and the loss is blamed on the wire.
    u64 receive_drops() const { return receive_drops_; }

private:
    static constexpr u32 kBatchSize = 64;
    static constexpr u32 kControlBytes = 64;

    struct Pending {
        sockaddr_in address;
        u32 size;
    };

    int refill();

    int fd_ = -1;
    std::vector<u8> send_bytes_;
    std::vector<Pending> send_pending_;
    u32 send_count_ = 0;
    std::vector<u8> recv_bytes_;
    std::vector<sockaddr_in> recv_addresses_;
    std::vector<u32> recv_sizes_;
    std::vector<u8> recv_control_;
    u32 recv_count_ = 0;
    u32 recv_next_ = 0;
    u32 last_overflow_ = 0;
    bool overflow_seen_ = false;
    Address local_;
    u64 bytes_sent_ = 0;
    u64 bytes_received_ = 0;
    u64 packets_sent_ = 0;
    u64 packets_received_ = 0;
    u64 send_failures_ = 0;
    u64 receive_drops_ = 0;
};

}  // namespace morton
