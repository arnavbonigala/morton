#pragma once
#include <string>
#include <vector>

#include "core/types.h"
#include "net/address.h"

namespace morton {

enum class RedisType : u8 { kNil, kStatus, kError, kString, kInteger, kArray };

struct RedisReply {
    RedisType type = RedisType::kNil;
    std::string str;
    i64 integer = 0;
    std::vector<RedisReply> elements;

    bool is_nil() const { return type == RedisType::kNil; }
    bool is_error() const { return type == RedisType::kError; }
    bool ok() const { return type == RedisType::kStatus && str == "OK"; }

    static RedisReply error(std::string message) {
        RedisReply reply;
        reply.type = RedisType::kError;
        reply.str = std::move(message);
        return reply;
    }
};

/// Blocking RESP2 client with no external dependencies.
///
/// Commands transparently reconnect once on a transport failure: presence and
/// registry traffic must survive a Redis restart without every caller writing
/// its own retry.
class RedisClient {
public:
    RedisClient() = default;
    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;
    ~RedisClient();

    bool connect(const Address& address, u32 timeout_ms = 2000);
    void disconnect();
    bool connected() const { return fd_ >= 0; }

    Address address() const { return address_; }

    RedisReply command(const std::vector<std::string>& args);

    /// Queues a command for pipelined execution; nothing is sent until flush().
    void queue(const std::vector<std::string>& args);
    std::size_t queued() const { return queued_count_; }

    /// Sends every queued command in one write and reads all replies back.
    bool flush(std::vector<RedisReply>* out);

    u64 commands_sent() const { return commands_sent_; }
    u64 reconnects() const { return reconnects_; }
    const std::string& last_error() const { return last_error_; }

private:
    bool open_socket();
    bool send_all(const char* data, std::size_t size);
    bool fill_buffer();
    bool read_reply(RedisReply* out);
    bool read_line(std::string* out);
    bool read_exact(std::size_t count, std::string* out);

    int fd_ = -1;
    Address address_;
    u32 timeout_ms_ = 2000;
    std::string pipeline_;
    std::size_t queued_count_ = 0;
    std::string buffer_;
    std::size_t cursor_ = 0;
    u64 commands_sent_ = 0;
    u64 reconnects_ = 0;
    std::string last_error_;
};

/// Serializes a command as a RESP array of bulk strings.
std::string encode_resp_command(const std::vector<std::string>& args);

}  // namespace morton
