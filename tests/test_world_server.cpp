#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "app/game_client.h"
#include "app/world_server.h"
#include "check.h"
#include "cluster/matchmaker.h"
#include "core/time.h"
#include "net/http_client.h"
#include "redis_fixture.h"
#include "ws_client.h"

using namespace morton;
using morton_test::redis_fixture;
using morton_test::WsClient;

namespace {

constexpr u64 kPumpStepUs = 2000;

RegionMap test_regions() {
    RegionMap map;
    map.world_size = 2048.f;
    map.regions_per_axis = 2;
    map.margin = 48.f;
    return map;
}

void wipe(const std::string& prefix) {
    RedisClient client;
    client.connect(redis_fixture().address());
    RedisReply keys = client.command({"KEYS", prefix + ":*"});
    if (keys.type != RedisType::kArray || keys.elements.empty()) return;
    std::vector<std::string> args{"DEL"};
    for (const RedisReply& key : keys.elements) args.push_back(key.str);
    client.command(args);
}

WorldServerConfig world_config(const std::string& id, const std::string& prefix) {
    WorldServerConfig config;
    config.shard_id = id;
    config.udp_bind = Address(0x7f000001u, 0);
    config.http_bind = Address(0x7f000001u, 0);
    config.redis = redis_fixture().address();
    config.key_prefix = prefix;
    config.regions = test_regions();
    config.capacity = 64;
    config.reconnect_grace_ms = 30000;
    config.redirect_grace_ticks = 20;
    return config;
}

MatchmakerConfig matchmaker_config(const std::string& prefix) {
    MatchmakerConfig config;
    config.redis = redis_fixture().address();
    config.http_bind = Address(0x7f000001u, 0);
    config.key_prefix = prefix;
    config.session_ttl_ms = 60000;
    config.regions = test_regions();
    return config;
}

GameClientConfig client_config(const std::string& player, const Address& matchmaker) {
    GameClientConfig config;
    config.player_id = player;
    config.matchmaker = matchmaker;
    return config;
}

struct Cluster {
    std::vector<std::unique_ptr<WorldServer>> servers;
    Matchmaker matchmaker;

    void sync() {
        for (auto& server : servers) {
            server->coordinator().refresh(wall_ms(), server->player_count(), 0.0, true);
        }
    }

    void pump(const std::vector<GameClient*>& clients, const Vec2& axis, u32 iterations) {
        for (u32 i = 0; i < iterations; ++i) {
            u64 now = now_us();
            for (auto& server : servers) server->tick(now);
            for (GameClient* client : clients) {
                client->update(now);
                client->send_input(axis, false, now);
            }
            sleep_us(kPumpStepUs);
        }
    }

    void shutdown() {
        matchmaker.stop();
        for (auto& server : servers) server->stop();
        servers.clear();
    }
};

Vec2 server_position(WorldServer& server, const std::string& player_id) {
    const WorldPlayer* player = server.player_by_id(player_id);
    if (player == nullptr) return Vec2{-1.f, -1.f};
    i64 index = server.world().entities().find(player->entity);
    if (index < 0) return Vec2{-1.f, -1.f};
    return server.world().entities().position[static_cast<std::size_t>(index)];
}

}  // namespace

TEST_CASE(a_matchmade_client_connects_and_predicts_the_authoritative_state) {
    wipe("morton:w1");

    Cluster cluster;
    cluster.servers.push_back(std::make_unique<WorldServer>());
    WorldServerConfig config = world_config("world-a", "morton:w1");
    config.drifters = 200;
    CHECK(cluster.servers[0]->start(config));
    CHECK(cluster.matchmaker.start(matchmaker_config("morton:w1")));
    cluster.sync();

    GameClient client;
    CHECK(client.start(client_config("ada", cluster.matchmaker.http_address())));
    CHECK(client.shard_id() == "world-a");

    cluster.pump({&client}, Vec2{0.f, 0.f}, 120);
    CHECK(client.connected());
    CHECK(client.local_entity() != kInvalidEntity);

    cluster.pump({&client}, Vec2{1.f, 0.3f}, 200);

    CHECK(client.stats().snapshots_applied > 50);
    CHECK(client.view().stats().snapshots_rejected == 0);
    CHECK(client.view().has_snapshot());

    Vec2 authoritative = server_position(*cluster.servers[0], "ada");
    Vec2 predicted = client.view().predicted_position();
    f32 error = (authoritative - predicted).length();
    CHECK(error < 12.f);
    CHECK(client.view().stats().mean_prediction_error < 6.f);
    // One snap is the very first snapshot, which lands the avatar at its spawn
    // point; anything beyond that would be visible rubber-banding.
    CHECK(client.view().stats().hard_snaps <= 1u);
    CHECK(client.view().stats().known_entities >= 2u);
    CHECK(!client.view().render_entities().empty());

    CHECK(cluster.servers[0]->stats().inputs_received > 100);
    CHECK(cluster.servers[0]->stats().snapshots_sent > 100);
    CHECK(cluster.servers[0]->tick_p99_ms() > 0.0);

    std::printf("       prediction error %.3f units, mean %.3f, %u snapshots\n",
                static_cast<double>(error),
                static_cast<double>(client.view().stats().mean_prediction_error),
                client.stats().snapshots_applied);

    client.stop();
    cluster.shutdown();
}

TEST_CASE(crossing_a_seam_hands_the_player_to_the_owning_shard_without_losing_state) {
    wipe("morton:w2");

    Cluster cluster;
    cluster.servers.push_back(std::make_unique<WorldServer>());
    cluster.servers.push_back(std::make_unique<WorldServer>());
    CHECK(cluster.servers[0]->start(world_config("world-a", "morton:w2")));
    CHECK(cluster.servers[1]->start(world_config("world-b", "morton:w2")));
    CHECK(cluster.matchmaker.start(matchmaker_config("morton:w2")));
    cluster.sync();

    CHECK(cluster.servers[0]->coordinator().owns_region(0));
    CHECK(cluster.servers[1]->coordinator().owns_region(1));

    GameClient client;
    CHECK(client.start(client_config("mover", cluster.matchmaker.http_address())));
    CHECK(client.shard_id() == "world-a");

    cluster.pump({&client}, Vec2{0.f, 0.f}, 120);
    CHECK(client.connected());
    Vec2 before = client.view().predicted_position();
    CHECK(cluster.servers[0]->player_by_id("mover") != nullptr);

    cluster.pump({&client}, Vec2{1.f, 0.f}, 400);

    CHECK_EQ(client.stats().migrations, 1u);
    CHECK(client.shard_id() == "world-b");
    CHECK(client.connected());
    CHECK_EQ(cluster.servers[0]->stats().migrations_out, 1u);
    CHECK_EQ(cluster.servers[1]->stats().migrations_in, 1u);

    CHECK(cluster.servers[1]->player_by_id("mover") != nullptr);
    CHECK(cluster.servers[0]->player_by_id("mover") == nullptr);

    Vec2 after = client.view().predicted_position();
    CHECK(after.x > before.x + 400.f);
    CHECK(after.x > 1072.f);

    Vec2 authoritative = server_position(*cluster.servers[1], "mover");
    CHECK((authoritative - after).length() < 20.f);
    CHECK(client.view().stats().hard_snaps <= 1u);

    PresenceRecord record;
    CHECK(cluster.servers[1]->coordinator().registry().get_presence("mover", &record));
    CHECK(record.shard_id == "world-b");

    std::printf("       migrated at x=%.1f, post-handoff drift %.3f units\n",
                static_cast<double>(after.x),
                static_cast<double>((authoritative - after).length()));

    client.stop();
    cluster.shutdown();
}

TEST_CASE(a_reconnecting_player_resumes_in_place_instead_of_respawning) {
    wipe("morton:w3");

    Cluster cluster;
    cluster.servers.push_back(std::make_unique<WorldServer>());
    CHECK(cluster.servers[0]->start(world_config("world-a", "morton:w3")));
    CHECK(cluster.matchmaker.start(matchmaker_config("morton:w3")));
    cluster.sync();

    GameClient client;
    CHECK(client.start(client_config("flaky", cluster.matchmaker.http_address())));
    cluster.pump({&client}, Vec2{0.f, 1.f}, 220);
    CHECK(client.connected());

    Vec2 departed = server_position(*cluster.servers[0], "flaky");
    CHECK(departed.y > 0.f);

    client.connection().disconnect();
    cluster.pump({}, Vec2{}, 10);
    CHECK(cluster.servers[0]->player_by_id("flaky") == nullptr);
    CHECK_EQ(cluster.servers[0]->world().player_count(), 0u);

    GameClient returning;
    CHECK(returning.start(client_config("flaky", cluster.matchmaker.http_address())));
    CHECK(returning.stats().reconnects == 1u);

    cluster.pump({&returning}, Vec2{0.f, 0.f}, 60);
    CHECK(returning.connected());
    CHECK_EQ(cluster.servers[0]->stats().reconnects_resumed, 1u);

    Vec2 resumed = server_position(*cluster.servers[0], "flaky");
    CHECK((resumed - departed).length() < 12.f);

    std::printf("       resumed %.3f units from where the session dropped\n",
                static_cast<double>((resumed - departed).length()));

    returning.stop();
    cluster.shutdown();
}

TEST_CASE(forged_and_replayed_credentials_are_refused) {
    wipe("morton:w4");

    Cluster cluster;
    cluster.servers.push_back(std::make_unique<WorldServer>());
    CHECK(cluster.servers[0]->start(world_config("world-a", "morton:w4")));
    CHECK(cluster.matchmaker.start(matchmaker_config("morton:w4")));
    cluster.sync();

    GameClientConfig forged;
    forged.player_id = "intruder";
    forged.direct_shard = cluster.servers[0]->udp_address();
    forged.session_token = 0xabadcafeull;
    forged.timeout_us = 1500000;

    GameClient attacker;
    CHECK(attacker.start(forged));
    cluster.pump({&attacker}, Vec2{}, 120);
    CHECK(!attacker.connected());
    CHECK(attacker.state() == ClientState::kDenied);
    CHECK(cluster.servers[0]->stats().denied_connects > 0);
    CHECK_EQ(cluster.servers[0]->player_count(), 0u);
    attacker.stop();

    GameClient legitimate;
    CHECK(legitimate.start(client_config("honest", cluster.matchmaker.http_address())));
    cluster.pump({&legitimate}, Vec2{}, 60);
    CHECK(legitimate.connected());

    GameClientConfig wrong_player;
    wrong_player.player_id = "impostor";
    wrong_player.direct_shard = cluster.servers[0]->udp_address();
    wrong_player.session_token = 0;
    wrong_player.timeout_us = 1500000;

    GameClient impostor;
    CHECK(impostor.start(wrong_player));
    cluster.pump({&legitimate, &impostor}, Vec2{}, 120);
    CHECK(!impostor.connected());
    CHECK(legitimate.connected());
    CHECK_EQ(cluster.servers[0]->player_count(), 1u);

    impostor.stop();
    legitimate.stop();
    cluster.shutdown();
}

TEST_CASE(the_viewer_bridge_streams_live_world_state_to_a_browser_client) {
    wipe("morton:w5");

    Cluster cluster;
    cluster.servers.push_back(std::make_unique<WorldServer>());
    WorldServerConfig config = world_config("world-a", "morton:w5");
    config.ws_bind = Address(0x7f000001u, 0);
    config.viewer_hz = 30;
    config.drifters = 20;
    CHECK(cluster.servers[0]->start(config));
    CHECK(cluster.matchmaker.start(matchmaker_config("morton:w5")));
    cluster.sync();

    HttpFetch page = http_fetch(cluster.servers[0]->http_address(), "GET", "/");
    CHECK(page.ok);
    CHECK(page.body.find("new WebSocket") != std::string::npos);
    CHECK(page.body.find(cluster.servers[0]->viewer_address().to_string()) != std::string::npos);

    WsClient viewer;
    CHECK(viewer.connect(cluster.servers[0]->viewer_address()));

    GameClient client;
    CHECK(client.start(client_config("watched", cluster.matchmaker.http_address())));
    cluster.pump({&client}, Vec2{1.f, 0.f}, 160);
    CHECK(client.connected());
    CHECK_EQ(cluster.servers[0]->viewer_count(), 1u);

    std::string frame;
    bool got_player = false;
    for (u32 attempt = 0; attempt < 40 && !got_player; ++attempt) {
        cluster.pump({&client}, Vec2{1.f, 0.f}, 5);
        if (!viewer.receive(&frame, 500)) continue;
        got_player = frame.find("\"players\":1") != std::string::npos;
    }

    CHECK(got_player);
    CHECK(frame.find("\"shard\":\"world-a\"") != std::string::npos);
    CHECK(frame.find("\"owned_regions\":[0") != std::string::npos);
    CHECK(frame.find("\"kind\":0") != std::string::npos);
    CHECK(frame.find("\"viewers\":1") != std::string::npos);

    Vec2 authoritative = server_position(*cluster.servers[0], "watched");
    CHECK(authoritative.x > 0.f);

    std::printf("       viewer frame %zu bytes for %u entities\n", frame.size(),
                cluster.servers[0]->world().entities().size());

    client.stop();
    cluster.shutdown();
}

int main() {
    if (!redis_fixture().start()) {
        std::printf("redis-server unavailable; skipping world server integration tests\n");
        return 0;
    }
    int result = ::morton_test::run_all();
    redis_fixture().stop();
    return result;
}
