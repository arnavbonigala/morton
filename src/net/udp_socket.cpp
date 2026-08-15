#include "net/udp_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

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
        other.fd_ = -1;
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
        ::close(fd_);
        fd_ = -1;
    }
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

int UdpSocket::receive(Address* from, u8* buffer, u32 capacity) {
    if (fd_ < 0) return -1;

    sockaddr_in sa;
    socklen_t sa_len = sizeof(sa);
    ssize_t received =
        ::recvfrom(fd_, buffer, capacity, 0, reinterpret_cast<sockaddr*>(&sa), &sa_len);

    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR || errno == ECONNREFUSED) return 0;
        return -1;
    }

    *from = Address::from_sockaddr(sa);
    ++packets_received_;
    bytes_received_ += static_cast<u64>(received);
    return static_cast<int>(received);
}

bool UdpSocket::wait_readable(u64 timeout_us) const {
    if (fd_ < 0) return false;

    pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int timeout_ms = static_cast<int>(timeout_us / 1000);
    int result = ::poll(&pfd, 1, timeout_ms);
    return result > 0 && (pfd.revents & POLLIN) != 0;
}

}  // namespace morton
