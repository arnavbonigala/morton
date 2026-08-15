#pragma once
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "cluster/redis.h"
#include "core/time.h"

namespace morton_test {

/// Spawns a throwaway redis-server on a free port for the duration of a test
/// binary. Tests exercise the real protocol against the real server; a stub
/// would only prove the stub matches the client's assumptions.
class RedisFixture {
public:
    /// Brings the server back on the port it was already using, so clients that
    /// cached the address see an outage rather than a moved endpoint.
    bool restart() {
        stop();
        return spawn();
    }

    bool start() {
        int probe = ::socket(AF_INET, SOCK_STREAM, 0);
        if (probe < 0) return false;
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(0x7f000001u);
        sa.sin_port = 0;
        if (::bind(probe, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
            ::close(probe);
            return false;
        }
        socklen_t len = sizeof(sa);
        ::getsockname(probe, reinterpret_cast<sockaddr*>(&sa), &len);
        port_ = ntohs(sa.sin_port);
        ::close(probe);

        return spawn();
    }

    bool spawn() {
        std::string port_text = std::to_string(port_);
        pid_ = ::fork();
        if (pid_ < 0) return false;
        if (pid_ == 0) {
            ::freopen("/dev/null", "w", stdout);
            ::freopen("/dev/null", "w", stderr);
            ::execlp("redis-server", "redis-server", "--port", port_text.c_str(), "--bind",
                     "127.0.0.1", "--save", "", "--appendonly", "no", nullptr);
            ::_exit(127);
        }

        for (int attempt = 0; attempt < 200; ++attempt) {
            morton::RedisClient client;
            if (client.connect(address(), 500)) {
                morton::RedisReply pong = client.command({"PING"});
                if (pong.type == morton::RedisType::kStatus && pong.str == "PONG") {
                    running_ = true;
                    return true;
                }
            }
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return false;
            }
            morton::sleep_us(25000);
        }
        stop();
        return false;
    }

    void stop() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
            pid_ = -1;
        }
        running_ = false;
    }

    ~RedisFixture() { stop(); }

    bool running() const { return running_; }
    morton::u16 port() const { return port_; }
    morton::Address address() const { return morton::Address(0x7f000001u, port_); }

private:
    pid_t pid_ = -1;
    morton::u16 port_ = 0;
    bool running_ = false;
};

inline RedisFixture& redis_fixture() {
    static RedisFixture fixture;
    return fixture;
}

}  // namespace morton_test
