#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/types.h"
#include "net/address.h"

namespace morton {

struct WebSocketConfig {
    Address bind;
    u32 max_clients = 64;
    u32 max_queued_frames = 8;
};

/// Broadcast-only RFC 6455 endpoint: a browser connects, and from then on it
/// receives text frames the simulation publishes.
///
/// All sockets live on one background thread, so publish() never blocks the
/// simulation tick. A viewer that cannot keep up has its oldest queued frames
/// dropped rather than stalling the world; the world is authoritative and the
/// next frame is a complete state anyway.
class WebSocketServer {
public:
    ~WebSocketServer();

    bool start(const WebSocketConfig& config);
    void stop();

    void publish(const std::string& text);

    Address local_address() const { return local_; }
    bool running() const { return running_.load(std::memory_order_relaxed); }
    u32 client_count() const { return clients_.load(std::memory_order_relaxed); }
    u64 frames_sent() const { return frames_sent_.load(std::memory_order_relaxed); }
    u64 frames_dropped() const { return frames_dropped_.load(std::memory_order_relaxed); }

private:
    struct Peer {
        int fd = -1;
        std::string outgoing;
        std::string incoming;
        bool closing = false;
    };

    void serve();
    void accept_peer();
    bool handshake(int fd);
    void drain(Peer& peer);
    void pump(Peer& peer);

    WebSocketConfig config_;
    int listen_fd_ = -1;
    Address local_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    std::mutex queue_mutex_;
    std::deque<std::string> queue_;

    std::vector<Peer> peers_;
    std::atomic<u32> clients_{0};
    std::atomic<u64> frames_sent_{0};
    std::atomic<u64> frames_dropped_{0};
};

/// Wraps `payload` in an unmasked server text frame.
std::string encode_text_frame(const std::string& payload);

/// Computes the Sec-WebSocket-Accept value for a client key.
std::string websocket_accept_key(const std::string& client_key);

std::string base64_encode(const u8* data, std::size_t size);

}  // namespace morton
