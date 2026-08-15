#include "net/udp_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <utility>

#include "core/log.h"

namespace morton {

UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept { *this = std::move(other); }

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        local_ = other.local_;
        bytes_sent_ = other.bytes_sent_;
        bytes_received_ = other.bytes_received_;
        packets_sent_ = other.packets_sent_;
        packets_received_ = other.packets_received_;
        send_failures_ = other.send_failures_;
        receive_drops_ = other.receive_drops_;
        last_overflow_ = other.last_overflow_;
        overflow_seen_ = other.overflow_seen_;
        recv_control_ = std::move(other.recv_control_);
        send_bytes_ = std::move(other.send_bytes_);
        send_pending_ = std::move(other.send_pending_);
        send_count_ = other.send_count_;
        recv_bytes_ = std::move(other.recv_bytes_);
        recv_addresses_ = std::move(other.recv_addresses_);
        recv_sizes_ = std::move(other.recv_sizes_);
        recv_count_ = other.recv_count_;
        recv_next_ = other.recv_next_;
        other.fd_ = -1;
        other.send_count_ = 0;
        other.recv_count_ = 0;
        other.recv_next_ = 0;
    }
    return *this;
}

bool UdpSocket::open(const Address& bind_address, u32 buffer_bytes) {
    close();

    fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) {
        MORTON_LOG_ERROR("socket() failed: %s", std::strerror(errno));
        return false;
    }

    int reuse = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int size = static_cast<int>(buffer_bytes);
    while (size > 65536) {
        if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0) break;
        size /= 2;
    }
    size = static_cast<int>(buffer_bytes);
    while (size > 65536) {
        if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0) break;
        size /= 2;
    }

    // Linux clamps the request to net.core.{w,r}mem_max without failing, so a
    // shard can be working out of a buffer a fraction of the size it asked for
    // and see the shortfall only as client side packet loss.
    int granted = 0;
    socklen_t granted_len = sizeof(granted);
    if (::getsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &granted, &granted_len) == 0 &&
        static_cast<u32>(granted) < buffer_bytes) {
        MORTON_LOG_WARN("send buffer is %d bytes, asked for %u", granted, buffer_bytes);
    }
    granted = 0;
    granted_len = sizeof(granted);
    if (::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &granted, &granted_len) == 0 &&
        static_cast<u32>(granted) < buffer_bytes) {
        MORTON_LOG_WARN("receive buffer is %d bytes, asked for %u", granted, buffer_bytes);
    }

#if defined(SO_RXQ_OVFL)
    int report_overflow = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_RXQ_OVFL, &report_overflow, sizeof(report_overflow));
#endif

    sockaddr_in sa = bind_address.to_sockaddr();
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        MORTON_LOG_ERROR("bind(%s) failed: %s", bind_address.to_string().c_str(),
                         std::strerror(errno));
        close();
        return false;
    }

    sockaddr_in actual;
    socklen_t actual_len = sizeof(actual);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&actual), &actual_len) == 0) {
        local_ = Address::from_sockaddr(actual);
    } else {
        local_ = bind_address;
    }

    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        MORTON_LOG_ERROR("fcntl(O_NONBLOCK) failed: %s", std::strerror(errno));
        close();
        return false;
    }

    return true;
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        flush_sends();
        ::close(fd_);
        fd_ = -1;
    }
    send_count_ = 0;
    recv_count_ = 0;
    recv_next_ = 0;
    overflow_seen_ = false;
}

bool UdpSocket::send(const Address& to, const u8* data, u32 size) {
    if (fd_ < 0 || size == 0) return false;

    sockaddr_in sa = to.to_sockaddr();
    ssize_t sent = ::sendto(fd_, data, size, 0, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    if (sent < 0) {
        ++send_failures_;
        return false;
    }

    ++packets_sent_;
    bytes_sent_ += static_cast<u64>(sent);
    return true;
}

bool UdpSocket::send_batched(const Address& to, const u8* data, u32 size) {
    if (fd_ < 0 || size == 0 || size > kMaxDatagramSize) return false;

    if (send_bytes_.empty()) {
        send_bytes_.resize(static_cast<std::size_t>(kBatchSize) * kMaxDatagramSize);
        send_pending_.resize(kBatchSize);
    }
    if (send_count_ == kBatchSize) flush_sends();

    std::memcpy(send_bytes_.data() + static_cast<std::size_t>(send_count_) * kMaxDatagramSize, data,
                size);
    send_pending_[send_count_].address = to.to_sockaddr();
    send_pending_[send_count_].size = size;
    ++send_count_;
    return true;
}

void UdpSocket::flush_sends() {
    if (send_count_ == 0) return;
    u32 count = send_count_;
    send_count_ = 0;
    if (fd_ < 0) return;

#if defined(__linux__)
    iovec vectors[kBatchSize];
    mmsghdr messages[kBatchSize];
    for (u32 i = 0; i < count; ++i) {
        vectors[i].iov_base = send_bytes_.data() + static_cast<std::size_t>(i) * kMaxDatagramSize;
        vectors[i].iov_len = send_pending_[i].size;
        std::memset(&messages[i], 0, sizeof(messages[i]));
        messages[i].msg_hdr.msg_name = &send_pending_[i].address;
        messages[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
        messages[i].msg_hdr.msg_iov = &vectors[i];
        messages[i].msg_hdr.msg_iovlen = 1;
    }

    u32 sent = 0;
    while (sent < count) {
        int result = ::sendmmsg(fd_, messages + sent, count - sent, 0);
        if (result <= 0) {
            send_failures_ += count - sent;
            return;
        }
        for (int i = 0; i < result; ++i) {
            ++packets_sent_;
            bytes_sent_ += messages[sent + i].msg_len;
        }
        sent += static_cast<u32>(result);
    }
#else
    for (u32 i = 0; i < count; ++i) {
        sockaddr_in address = send_pending_[i].address;
        const u8* data = send_bytes_.data() + static_cast<std::size_t>(i) * kMaxDatagramSize;
        ssize_t result = ::sendto(fd_, data, send_pending_[i].size, 0,
                                  reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (result < 0) {
            ++send_failures_;
            continue;
        }
        ++packets_sent_;
        bytes_sent_ += static_cast<u64>(result);
    }
#endif
}

int UdpSocket::receive(Address* from, u8* buffer, u32 capacity) {
    if (fd_ < 0) return -1;

    if (recv_next_ == recv_count_) {
        int result = refill();
        if (result <= 0) return result;
    }

    u32 size = recv_sizes_[recv_next_];
    if (size > capacity) size = capacity;
    std::memcpy(buffer, recv_bytes_.data() + static_cast<std::size_t>(recv_next_) * kMaxDatagramSize,
                size);
    *from = Address::from_sockaddr(recv_addresses_[recv_next_]);
    ++recv_next_;
    return static_cast<int>(size);
}

int UdpSocket::refill() {
    recv_next_ = 0;
    recv_count_ = 0;
    if (recv_bytes_.empty()) {
        recv_bytes_.resize(static_cast<std::size_t>(kBatchSize) * kMaxDatagramSize);
        recv_addresses_.resize(kBatchSize);
        recv_sizes_.resize(kBatchSize);
        recv_control_.resize(static_cast<std::size_t>(kBatchSize) * kControlBytes);
    }

    auto soft_error = [] {
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNREFUSED;
    };

#if defined(__linux__)
    iovec vectors[kBatchSize];
    mmsghdr messages[kBatchSize];
    for (u32 i = 0; i < kBatchSize; ++i) {
        vectors[i].iov_base = recv_bytes_.data() + static_cast<std::size_t>(i) * kMaxDatagramSize;
        vectors[i].iov_len = kMaxDatagramSize;
        std::memset(&messages[i], 0, sizeof(messages[i]));
        messages[i].msg_hdr.msg_name = &recv_addresses_[i];
        messages[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
        messages[i].msg_hdr.msg_iov = &vectors[i];
        messages[i].msg_hdr.msg_iovlen = 1;
        messages[i].msg_hdr.msg_control =
            recv_control_.data() + static_cast<std::size_t>(i) * kControlBytes;
        messages[i].msg_hdr.msg_controllen = kControlBytes;
    }

    int result = ::recvmmsg(fd_, messages, kBatchSize, MSG_DONTWAIT, nullptr);
    if (result < 0) return soft_error() ? 0 : -1;
    for (int i = 0; i < result; ++i) recv_sizes_[i] = messages[i].msg_len;
    recv_count_ = static_cast<u32>(result);

#if defined(SO_RXQ_OVFL)
    // The counter the kernel attaches is cumulative and only as fresh as the
    // datagram carrying it, so the last message of the batch is the one worth
    // reading. It is 32 bits and wraps, which unsigned subtraction absorbs.
    if (recv_count_ > 0) {
        msghdr& newest = messages[recv_count_ - 1].msg_hdr;
        for (cmsghdr* header = CMSG_FIRSTHDR(&newest); header != nullptr;
             header = CMSG_NXTHDR(&newest, header)) {
            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SO_RXQ_OVFL) continue;
            u32 total = 0;
            std::memcpy(&total, CMSG_DATA(header), sizeof(total));
            if (overflow_seen_) receive_drops_ += static_cast<u32>(total - last_overflow_);
            last_overflow_ = total;
            overflow_seen_ = true;
        }
    }
#endif
#else
    for (u32 i = 0; i < kBatchSize; ++i) {
        socklen_t address_len = sizeof(sockaddr_in);
        ssize_t result = ::recvfrom(
            fd_, recv_bytes_.data() + static_cast<std::size_t>(i) * kMaxDatagramSize,
            kMaxDatagramSize, 0, reinterpret_cast<sockaddr*>(&recv_addresses_[i]), &address_len);
        if (result < 0) {
            if (recv_count_ == 0 && !soft_error()) return -1;
            break;
        }
        recv_sizes_[i] = static_cast<u32>(result);
        ++recv_count_;
    }
#endif

    for (u32 i = 0; i < recv_count_; ++i) {
        ++packets_received_;
        bytes_received_ += recv_sizes_[i];
    }
    return recv_count_ > 0 ? 1 : 0;
}

bool UdpSocket::wait_readable(u64 timeout_us) const {
    if (fd_ < 0) return false;
    if (recv_next_ < recv_count_) return true;

    pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int timeout_ms = static_cast<int>(timeout_us / 1000);
    int result = ::poll(&pfd, 1, timeout_ms);
    return result > 0 && (pfd.revents & POLLIN) != 0;
}

}  // namespace morton
