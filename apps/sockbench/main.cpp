#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/time.h"
#include "net/udp_socket.h"

using namespace morton;

namespace {

u32 number(int argc, char** argv, const char* name, u32 fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return static_cast<u32>(std::strtoul(argv[i + 1], nullptr, 10));
        }
    }
    return fallback;
}

struct Result {
    f64 send_ns;
    f64 receive_ns;
    u32 received;
};

/// Sends `count` datagrams from `from` to `to` and drains them, reporting the
/// per-datagram cost of each direction.
Result measure(UdpSocket& from, UdpSocket& to, const Address& destination, u32 count, u32 payload,
               bool batched) {
    std::vector<u8> data(payload, 0xa5);
    u8 scratch[kMaxDatagramSize];
    Address source;

    // The socket buffer holds far less than a whole round, so each chunk is
    // drained before the next is offered; sending the lot in one go would
    // measure the kernel dropping datagrams rather than the syscall.
    const u32 chunk = 64;
    u64 send_us = 0;
    u64 receive_us = 0;
    u32 received = 0;

    for (u32 offset = 0; offset < count; offset += chunk) {
        u32 size = count - offset < chunk ? count - offset : chunk;

        u64 mark = now_us();
        for (u32 i = 0; i < size; ++i) {
            if (batched) {
                from.send_batched(destination, data.data(), payload);
            } else {
                from.send(destination, data.data(), payload);
            }
        }
        if (batched) from.flush_sends();
        u64 sent_at = now_us();
        send_us += sent_at - mark;

        while (true) {
            int bytes = to.receive(&source, scratch, sizeof(scratch));
            if (bytes <= 0) break;
            ++received;
        }
        receive_us += now_us() - sent_at;
    }

    return {static_cast<f64>(send_us) * 1000.0 / count,
            static_cast<f64>(receive_us) * 1000.0 / (received > 0 ? received : 1), received};
}

}  // namespace

int main(int argc, char** argv) {
    u32 count = number(argc, argv, "--count", 4096);
    u32 payload = number(argc, argv, "--payload", 512);
    u32 rounds = number(argc, argv, "--rounds", 20);

    UdpSocket sender;
    UdpSocket receiver;
    if (!sender.open(Address(0x7f000001, 0)) || !receiver.open(Address(0x7f000001, 0))) {
        std::fprintf(stderr, "failed to open sockets\n");
        return 1;
    }
    const Address destination = receiver.local_address();

    for (int batched = 0; batched < 2; ++batched) {
        std::vector<f64> send_samples;
        std::vector<f64> receive_samples;
        u64 delivered = 0;
        u64 offered = 0;

        for (u32 round = 0; round < rounds; ++round) {
            Result result = measure(sender, receiver, destination, count, payload, batched != 0);
            if (round == 0) continue;
            send_samples.push_back(result.send_ns);
            receive_samples.push_back(result.receive_ns);
            delivered += result.received;
            offered += count;
        }

        auto median = [](std::vector<f64>& samples) {
            std::sort(samples.begin(), samples.end());
            return samples[samples.size() / 2];
        };
        std::printf("%-9s send %6.0f ns/datagram  receive %6.0f ns/datagram  delivered %.1f%%\n",
                    batched != 0 ? "batched" : "single", median(send_samples),
                    median(receive_samples), 100.0 * static_cast<f64>(delivered) / offered);
    }

    return 0;
}
