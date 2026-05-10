#include "client/ClientSyncRuntime.hpp"
#include "net/InterpolationBuffer.hpp"
#include "net/ClientRuntime.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

sim::RemoteActorState makeEntity(int entityId, float x, float z) {
    sim::RemoteActorState entity;
    entity.entityId = entityId;
    entity.position = sim::Vec3{x, 0.0f, z};
    entity.velocity = sim::Vec3{};
    entity.yaw = 0.0f;
    entity.pitch = 0.0f;
    entity.health = 100.0f;
    entity.radius = 0.8f;
    entity.alive = true;
    return entity;
}

void testHalfwayInterpolationUsesTwoSamples() {
    net::InterpolationBuffer buffer;
    buffer.pushSnapshot(1'000'000u, std::vector<sim::RemoteActorState>{makeEntity(7, 0.0f, 0.0f)});
    buffer.pushSnapshot(1'100'000u, std::vector<sim::RemoteActorState>{makeEntity(7, 10.0f, 0.0f)});

    const auto entities = buffer.sample(1'050'000u);
    expect(entities.size() == 1u, "buffer should return the tracked entity");
    expect(entities.front().position.x == 5.0f,
           "sampling halfway between two snapshots should interpolate halfway");
}

void testTeleportDiscontinuitySnapsToNewestSample() {
    net::InterpolationBuffer buffer(net::InterpolationConfig{32u, 4.0f});
    buffer.pushSnapshot(1'000'000u, std::vector<sim::RemoteActorState>{makeEntity(11, 0.0f, 0.0f)});
    buffer.pushSnapshot(1'100'000u, std::vector<sim::RemoteActorState>{makeEntity(11, 100.0f, 0.0f)});

    const auto entities = buffer.sample(1'050'000u);
    expect(entities.size() == 1u, "teleport sample should still return one entity");
    expect(entities.front().position.x == 100.0f,
           "teleport-sized discontinuities should clear history and snap to the newest sample");
}

void testMissingStraddlingSamplesFallBackToNewestValidSample() {
    net::InterpolationBuffer buffer;
    buffer.pushSnapshot(2'000'000u, std::vector<sim::RemoteActorState>{makeEntity(19, 1.0f, 0.0f)});
    buffer.pushSnapshot(2'100'000u, std::vector<sim::RemoteActorState>{makeEntity(19, 3.0f, 0.0f)});

    const auto entities = buffer.sample(2'300'000u);
    expect(entities.size() == 1u, "buffer should still return the latest tracked entity");
    expect(entities.front().position.x == 3.0f,
           "when no straddling samples exist, interpolation should fall back to the newest valid sample");
}

void testClientRuntimeCanSwitchBetweenBufferedInterpolationAndNewestSnapshotFallback() {
    net::InterpolationBuffer buffer;
    buffer.pushSnapshot(1'000'000u, std::vector<sim::RemoteActorState>{makeEntity(7, 0.0f, 0.0f)});
    buffer.pushSnapshot(1'100'000u, std::vector<sim::RemoteActorState>{makeEntity(7, 10.0f, 0.0f)});

    const std::vector<sim::RemoteActorState> newestSnapshot{makeEntity(7, 10.0f, 0.0f)};
    const auto interpolated = client::ClientSyncRuntime::sampleRemoteActorsForPresentation(
        buffer,
        newestSnapshot,
        1'050'000u,
        true);
    expect(interpolated.size() == 1u && interpolated.front().position.x == 5.0f,
           "enabled multiplayer interpolation should sample between buffered snapshots");

    const auto newestFallback = client::ClientSyncRuntime::sampleRemoteActorsForPresentation(
        buffer,
        newestSnapshot,
        1'050'000u,
        false);
    expect(newestFallback.size() == 1u && newestFallback.front().position.x == 10.0f,
           "disabled multiplayer interpolation should fall back to the newest authoritative snapshot");
}

}  // namespace

int main() {
    try {
        testHalfwayInterpolationUsesTwoSamples();
        testTeleportDiscontinuitySnapsToNewestSample();
        testMissingStraddlingSamplesFallBackToNewestValidSample();
        testClientRuntimeCanSwitchBetweenBufferedInterpolationAndNewestSnapshotFallback();
        std::cout << "InterpolationTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "InterpolationTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
