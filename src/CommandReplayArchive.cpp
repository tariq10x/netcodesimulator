#include "replay/ReplayArchive.hpp"

#include <fstream>
#include <iterator>
#include <utility>

#include "net/Codec.hpp"

namespace replay {
namespace {

constexpr std::uint32_t kCommandReplayMagic = 0x444D434Eu;  // "NCMD" in little-endian storage.
constexpr std::uint32_t kMaxReplayCommands = 1'000'000u;
constexpr std::uint32_t kMaxReplayEvents = 1'000'000u;
constexpr std::uint32_t kMaxReplayKeyframes = 120'000u;
constexpr std::uint32_t kMaxReplayClients = 64u;
constexpr std::uint32_t kMaxReplayClientEvents = 1'000'000u;
constexpr std::uint32_t kMaxWorldPlayers = 256u;
constexpr std::uint32_t kMaxWorldEnemies = 4'096u;
constexpr std::uint32_t kMaxWorldRoster = 512u;
constexpr std::uint32_t kMaxWorldSpawns = 4'096u;
constexpr std::uint32_t kMaxCollisionBoxes = 4'096u;

using net::ByteBuffer;
using net::codec::ByteReader;
using net::codec::ByteWriter;

void setError(std::string* errorOut, const std::string& message) {
    if (errorOut != nullptr) {
        *errorOut = message;
    }
}

bool readFileBytes(const std::filesystem::path& path, ByteBuffer* bytesOut) {
    if (bytesOut == nullptr) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    bytesOut->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool writeFileBytes(const std::filesystem::path& path, const ByteBuffer& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

bool readCount(ByteReader& reader, std::uint32_t maxCount, std::uint32_t* countOut) {
    std::uint32_t count = 0u;
    if (countOut == nullptr || !reader.readU32(&count) || count > maxCount) {
        return false;
    }
    *countOut = count;
    return true;
}

bool readInt(ByteReader& reader, int* valueOut) {
    std::int32_t value = 0;
    if (valueOut == nullptr || !reader.readI32(&value)) {
        return false;
    }
    *valueOut = static_cast<int>(value);
    return true;
}

void writeVec3(ByteWriter& writer, const sim::Vec3& value) {
    writer.writeF32(value.x);
    writer.writeF32(value.y);
    writer.writeF32(value.z);
}

bool readVec3(ByteReader& reader, sim::Vec3* valueOut) {
    return valueOut != nullptr &&
           reader.readF32(&valueOut->x) &&
           reader.readF32(&valueOut->y) &&
           reader.readF32(&valueOut->z);
}

void writeTeam(ByteWriter& writer, sim::TeamId team) {
    writer.writeU8(static_cast<std::uint8_t>(team));
}

bool readTeam(ByteReader& reader, sim::TeamId* teamOut) {
    std::uint8_t raw = 0u;
    if (teamOut == nullptr || !reader.readU8(&raw) ||
        raw > static_cast<std::uint8_t>(sim::TeamId::Spectator)) {
        return false;
    }
    *teamOut = static_cast<sim::TeamId>(raw);
    return true;
}

void writePlayerState(ByteWriter& writer, const sim::PlayerState& player) {
    writer.writeI32(player.playerId);
    writeVec3(writer, player.position);
    writeVec3(writer, player.velocity);
    writer.writeF32(player.yaw);
    writer.writeF32(player.pitch);
    writer.writeF32(player.health);
    writer.writeF32(player.maxHealth);
    writer.writeF32(player.weaponCooldownRemaining);
    writer.writeI32(player.jumpsUsed);
    writer.writeBool(player.grounded);
}

bool readPlayerState(ByteReader& reader, sim::PlayerState* playerOut) {
    return playerOut != nullptr &&
           readInt(reader, &playerOut->playerId) &&
           readVec3(reader, &playerOut->position) &&
           readVec3(reader, &playerOut->velocity) &&
           reader.readF32(&playerOut->yaw) &&
           reader.readF32(&playerOut->pitch) &&
           reader.readF32(&playerOut->health) &&
           reader.readF32(&playerOut->maxHealth) &&
           reader.readF32(&playerOut->weaponCooldownRemaining) &&
           readInt(reader, &playerOut->jumpsUsed) &&
           reader.readBool(&playerOut->grounded);
}

void writeRemoteActorState(ByteWriter& writer, const sim::RemoteActorState& actor) {
    writer.writeI32(actor.entityId);
    writeVec3(writer, actor.position);
    writeVec3(writer, actor.velocity);
    writer.writeF32(actor.yaw);
    writer.writeF32(actor.pitch);
    writer.writeF32(actor.health);
    writer.writeF32(actor.radius);
    writer.writeBool(actor.alive);
}

bool readRemoteActorState(ByteReader& reader, sim::RemoteActorState* actorOut) {
    return actorOut != nullptr &&
           readInt(reader, &actorOut->entityId) &&
           readVec3(reader, &actorOut->position) &&
           readVec3(reader, &actorOut->velocity) &&
           reader.readF32(&actorOut->yaw) &&
           reader.readF32(&actorOut->pitch) &&
           reader.readF32(&actorOut->health) &&
           reader.readF32(&actorOut->radius) &&
           reader.readBool(&actorOut->alive);
}

void writeControlBinding(ByteWriter& writer, const sim::ControlBinding& binding) {
    writer.writeU8(static_cast<std::uint8_t>(binding.kind));
    writer.writeI32(binding.actorId);
}

bool readControlBinding(ByteReader& reader, sim::ControlBinding* bindingOut) {
    std::uint8_t raw = 0u;
    if (bindingOut == nullptr || !reader.readU8(&raw) ||
        raw > static_cast<std::uint8_t>(sim::ControlBindingKind::Actor) ||
        !readInt(reader, &bindingOut->actorId)) {
        return false;
    }
    bindingOut->kind = static_cast<sim::ControlBindingKind>(raw);
    return true;
}

void writeRosterEntry(ByteWriter& writer, const sim::RosterEntry& entry) {
    writer.writeI32(entry.actorId);
    writeTeam(writer, entry.team);
    writer.writeU8(static_cast<std::uint8_t>(entry.sessionPresence));
    writer.writeU8(static_cast<std::uint8_t>(entry.participation));
    writeControlBinding(writer, entry.control);
    writer.writeBool(entry.isBot);
    writer.writeU16(entry.kills);
    writer.writeU16(entry.deaths);
    writer.writeU16(entry.assists);
    writer.writeBool(entry.alive);
    writer.writeBool(entry.interpolationEnabled);
    writer.writeBool(entry.predictionEnabled);
    writer.writeU8(static_cast<std::uint8_t>(entry.reconciliationStrategy));
    writer.writeU32(entry.smoothCorrectionWindowMs);
    writer.writeU16(entry.latencyMs);
    writer.writeU8(entry.lossPct);
    writer.writeString(entry.displayName);
}

bool readRosterEntry(ByteReader& reader, sim::RosterEntry* entryOut) {
    std::uint8_t presence = 0u;
    std::uint8_t participation = 0u;
    std::uint8_t strategy = 0u;
    if (entryOut == nullptr ||
        !readInt(reader, &entryOut->actorId) ||
        !readTeam(reader, &entryOut->team) ||
        !reader.readU8(&presence) ||
        presence > static_cast<std::uint8_t>(sim::SessionPresence::Disconnected) ||
        !reader.readU8(&participation) ||
        participation > static_cast<std::uint8_t>(sim::ParticipationState::Spectating) ||
        !readControlBinding(reader, &entryOut->control) ||
        !reader.readBool(&entryOut->isBot) ||
        !reader.readU16(&entryOut->kills) ||
        !reader.readU16(&entryOut->deaths) ||
        !reader.readU16(&entryOut->assists) ||
        !reader.readBool(&entryOut->alive) ||
        !reader.readBool(&entryOut->interpolationEnabled) ||
        !reader.readBool(&entryOut->predictionEnabled) ||
        !reader.readU8(&strategy) ||
        strategy > static_cast<std::uint8_t>(sim::RuntimeReconciliationStrategy::Smooth) ||
        !reader.readU32(&entryOut->smoothCorrectionWindowMs) ||
        !reader.readU16(&entryOut->latencyMs) ||
        !reader.readU8(&entryOut->lossPct) ||
        !reader.readString(&entryOut->displayName)) {
        return false;
    }
    entryOut->sessionPresence = static_cast<sim::SessionPresence>(presence);
    entryOut->participation = static_cast<sim::ParticipationState>(participation);
    entryOut->reconciliationStrategy = static_cast<sim::RuntimeReconciliationStrategy>(strategy);
    return true;
}

void writeEnvironment(ByteWriter& writer, const sim::MovementEnvironment& environment) {
    writer.writeF32(environment.arenaHalfSize);
    writer.writeU32(static_cast<std::uint32_t>(environment.collisionBoxes.size()));
    for (const sim::CollisionBox& box : environment.collisionBoxes) {
        writeVec3(writer, box.center);
        writeVec3(writer, box.halfSize);
    }
}

bool readEnvironment(ByteReader& reader, sim::MovementEnvironment* environmentOut) {
    std::uint32_t count = 0u;
    if (environmentOut == nullptr ||
        !reader.readF32(&environmentOut->arenaHalfSize) ||
        !readCount(reader, kMaxCollisionBoxes, &count)) {
        return false;
    }
    environmentOut->collisionBoxes.resize(count);
    for (sim::CollisionBox& box : environmentOut->collisionBoxes) {
        if (!readVec3(reader, &box.center) || !readVec3(reader, &box.halfSize)) {
            return false;
        }
    }
    return true;
}

void writeSimConfig(ByteWriter& writer, const sim::SimConfig& config) {
    writer.writeF32(config.playerEyeHeight);
    writer.writeF32(config.playerMoveSpeed);
    writer.writeF32(config.playerJumpVelocity);
    writer.writeF32(config.playerGravity);
    writer.writeI32(config.playerMaxJumps);
    writer.writeF32(config.playerRadius);
    writer.writeF32(config.playerCollisionHeight);
    writer.writeF32(config.minPitch);
    writer.writeF32(config.maxPitch);
    writer.writeF32(config.playerMaxHealth);
    writer.writeF32(config.enemyRadius);
    writer.writeF32(config.enemyMaxHealth);
    writer.writeF32(config.weaponCooldownSeconds);
    writer.writeF32(config.weaponDamage);
    writer.writeF32(config.weaponRange);
    writer.writeF32(config.arenaHalfSize);
}

bool readSimConfig(ByteReader& reader, sim::SimConfig* configOut) {
    return configOut != nullptr &&
           reader.readF32(&configOut->playerEyeHeight) &&
           reader.readF32(&configOut->playerMoveSpeed) &&
           reader.readF32(&configOut->playerJumpVelocity) &&
           reader.readF32(&configOut->playerGravity) &&
           readInt(reader, &configOut->playerMaxJumps) &&
           reader.readF32(&configOut->playerRadius) &&
           reader.readF32(&configOut->playerCollisionHeight) &&
           reader.readF32(&configOut->minPitch) &&
           reader.readF32(&configOut->maxPitch) &&
           reader.readF32(&configOut->playerMaxHealth) &&
           reader.readF32(&configOut->enemyRadius) &&
           reader.readF32(&configOut->enemyMaxHealth) &&
           reader.readF32(&configOut->weaponCooldownSeconds) &&
           reader.readF32(&configOut->weaponDamage) &&
           reader.readF32(&configOut->weaponRange) &&
           reader.readF32(&configOut->arenaHalfSize);
}

void writeWorldState(ByteWriter& writer, const sim::WorldState& world) {
    writeEnvironment(writer, world.environment);
    writer.writeU16(world.cadence.authoritativeTickHz);
    writer.writeU16(world.cadence.snapshotCadenceHz);
    writer.writeU16(world.cadence.commandCadenceHz);
    writer.writeU32(world.authoritativeTime.serverTick);
    writer.writeU64(world.authoritativeTime.serverTimeUs);
    writer.writeU64(world.authoritativeTime.viewedServerTimeUs);

    writer.writeU32(static_cast<std::uint32_t>(world.players.size()));
    for (const sim::PlayerState& player : world.players) {
        writePlayerState(writer, player);
    }
    writer.writeU32(static_cast<std::uint32_t>(world.enemies.size()));
    for (const sim::RemoteActorState& enemy : world.enemies) {
        writeRemoteActorState(writer, enemy);
    }
    writer.writeU32(static_cast<std::uint32_t>(world.roster.size()));
    for (const sim::RosterEntry& entry : world.roster) {
        writeRosterEntry(writer, entry);
    }

    writer.writeU16(world.teamScores.attackers);
    writer.writeU16(world.teamScores.defenders);
    writer.writeU32(world.sessionMetadata.sessionId);
    writer.writeI32(world.sessionMetadata.levelSlot);
    writer.writeU32(world.sessionMetadata.levelHash);
    writer.writeU16(world.sessionMetadata.maxHumanPlayers);
    writer.writeU16(world.sessionMetadata.connectedHumanPlayers);
    writer.writeU16(world.sessionMetadata.connectedBotPlayers);

    writer.writeU32(static_cast<std::uint32_t>(world.playerSpawns.size()));
    for (const sim::Vec3& spawn : world.playerSpawns) {
        writeVec3(writer, spawn);
    }
    writer.writeU32(static_cast<std::uint32_t>(world.enemySpawns.size()));
    for (const sim::Vec3& spawn : world.enemySpawns) {
        writeVec3(writer, spawn);
    }
    writer.writeU32(static_cast<std::uint32_t>(world.enemyWaypointIndices.size()));
    for (std::size_t index : world.enemyWaypointIndices) {
        writer.writeU32(static_cast<std::uint32_t>(index));
    }
    writer.writeU32(static_cast<std::uint32_t>(world.enemyRespawnTimers.size()));
    for (float timer : world.enemyRespawnTimers) {
        writer.writeF32(timer);
    }
}

bool readWorldState(ByteReader& reader, sim::WorldState* worldOut) {
    std::uint32_t count = 0u;
    if (worldOut == nullptr ||
        !readEnvironment(reader, &worldOut->environment) ||
        !reader.readU16(&worldOut->cadence.authoritativeTickHz) ||
        !reader.readU16(&worldOut->cadence.snapshotCadenceHz) ||
        !reader.readU16(&worldOut->cadence.commandCadenceHz) ||
        !reader.readU32(&worldOut->authoritativeTime.serverTick) ||
        !reader.readU64(&worldOut->authoritativeTime.serverTimeUs) ||
        !reader.readU64(&worldOut->authoritativeTime.viewedServerTimeUs)) {
        return false;
    }

    if (!readCount(reader, kMaxWorldPlayers, &count)) {
        return false;
    }
    worldOut->players.resize(count);
    for (sim::PlayerState& player : worldOut->players) {
        if (!readPlayerState(reader, &player)) {
            return false;
        }
    }
    if (!readCount(reader, kMaxWorldEnemies, &count)) {
        return false;
    }
    worldOut->enemies.resize(count);
    for (sim::RemoteActorState& enemy : worldOut->enemies) {
        if (!readRemoteActorState(reader, &enemy)) {
            return false;
        }
    }
    if (!readCount(reader, kMaxWorldRoster, &count)) {
        return false;
    }
    worldOut->roster.resize(count);
    for (sim::RosterEntry& entry : worldOut->roster) {
        if (!readRosterEntry(reader, &entry)) {
            return false;
        }
    }

    if (!reader.readU16(&worldOut->teamScores.attackers) ||
        !reader.readU16(&worldOut->teamScores.defenders) ||
        !reader.readU32(&worldOut->sessionMetadata.sessionId) ||
        !reader.readI32(&worldOut->sessionMetadata.levelSlot) ||
        !reader.readU32(&worldOut->sessionMetadata.levelHash) ||
        !reader.readU16(&worldOut->sessionMetadata.maxHumanPlayers) ||
        !reader.readU16(&worldOut->sessionMetadata.connectedHumanPlayers) ||
        !reader.readU16(&worldOut->sessionMetadata.connectedBotPlayers)) {
        return false;
    }

    if (!readCount(reader, kMaxWorldSpawns, &count)) {
        return false;
    }
    worldOut->playerSpawns.resize(count);
    for (sim::Vec3& spawn : worldOut->playerSpawns) {
        if (!readVec3(reader, &spawn)) {
            return false;
        }
    }
    if (!readCount(reader, kMaxWorldSpawns, &count)) {
        return false;
    }
    worldOut->enemySpawns.resize(count);
    for (sim::Vec3& spawn : worldOut->enemySpawns) {
        if (!readVec3(reader, &spawn)) {
            return false;
        }
    }
    if (!readCount(reader, kMaxWorldSpawns, &count)) {
        return false;
    }
    worldOut->enemyWaypointIndices.resize(count);
    for (std::size_t& index : worldOut->enemyWaypointIndices) {
        std::uint32_t raw = 0u;
        if (!reader.readU32(&raw)) {
            return false;
        }
        index = raw;
    }
    if (!readCount(reader, kMaxWorldSpawns, &count)) {
        return false;
    }
    worldOut->enemyRespawnTimers.resize(count);
    for (float& timer : worldOut->enemyRespawnTimers) {
        if (!reader.readF32(&timer)) {
            return false;
        }
    }
    return true;
}

void writeCommand(ByteWriter& writer, const sim::PlayerCommand& command) {
    writer.writeU32(command.seq);
    writer.writeF32(command.dtSeconds);
    writer.writeF32(command.moveX);
    writer.writeF32(command.moveY);
    writer.writeF32(command.yaw);
    writer.writeF32(command.pitch);
    writer.writeU32(command.buttons);
    writer.writeU64(command.viewedServerTimeUs);
    writer.writeU32(command.interpDelayMs);
    writer.writeU64(command.controlViewedServerTimeUs);
    writer.writeU32(command.controlInterpDelayMs);
    writeTeam(writer, command.requestedTeam);
    writer.writeU16(command.reportedLatencyMs);
    writer.writeU8(command.reportedLossPct);
}

bool readCommand(ByteReader& reader, sim::PlayerCommand* commandOut) {
    return commandOut != nullptr &&
           reader.readU32(&commandOut->seq) &&
           reader.readF32(&commandOut->dtSeconds) &&
           reader.readF32(&commandOut->moveX) &&
           reader.readF32(&commandOut->moveY) &&
           reader.readF32(&commandOut->yaw) &&
           reader.readF32(&commandOut->pitch) &&
           reader.readU32(&commandOut->buttons) &&
           reader.readU64(&commandOut->viewedServerTimeUs) &&
           reader.readU32(&commandOut->interpDelayMs) &&
           reader.readU64(&commandOut->controlViewedServerTimeUs) &&
           reader.readU32(&commandOut->controlInterpDelayMs) &&
           readTeam(reader, &commandOut->requestedTeam) &&
           reader.readU16(&commandOut->reportedLatencyMs) &&
           reader.readU8(&commandOut->reportedLossPct);
}

bool readTrack(ByteReader& reader, ReplayTrack* trackOut) {
    std::uint8_t raw = 0u;
    if (trackOut == nullptr || !reader.readU8(&raw) ||
        raw > static_cast<std::uint8_t>(ReplayTrack::Control)) {
        return false;
    }
    *trackOut = static_cast<ReplayTrack>(raw);
    return true;
}

bool readStage(ByteReader& reader, ReplayCommandStage* stageOut) {
    std::uint8_t raw = 0u;
    if (stageOut == nullptr || !reader.readU8(&raw) ||
        raw > static_cast<std::uint8_t>(ReplayCommandStage::Applied)) {
        return false;
    }
    *stageOut = static_cast<ReplayCommandStage>(raw);
    return true;
}

void writeCommandEvent(ByteWriter& writer, const ServerCommandEvent& event) {
    writer.writeU8(static_cast<std::uint8_t>(event.track));
    writer.writeU8(static_cast<std::uint8_t>(event.stage));
    writer.writeU16(event.peerId);
    writer.writeI32(event.actorId);
    writer.writeU32(event.serverTick);
    writer.writeU64(event.serverTimeUs);
    writeCommand(writer, event.command);
}

bool readCommandEvent(ByteReader& reader, ServerCommandEvent* eventOut) {
    return eventOut != nullptr &&
           readTrack(reader, &eventOut->track) &&
           readStage(reader, &eventOut->stage) &&
           reader.readU16(&eventOut->peerId) &&
           readInt(reader, &eventOut->actorId) &&
           reader.readU32(&eventOut->serverTick) &&
           reader.readU64(&eventOut->serverTimeUs) &&
           readCommand(reader, &eventOut->command);
}

void writeSnapshotEvent(ByteWriter& writer, const net::SnapshotEvent& event) {
    writer.writeU8(static_cast<std::uint8_t>(event.kind));
    writer.writeI32(event.sourcePlayerId);
    writer.writeI32(event.targetEntityId);
    writeVec3(writer, event.origin);
    writeVec3(writer, event.direction);
    writer.writeBool(event.hit);
}

bool readSnapshotEvent(ByteReader& reader, net::SnapshotEvent* eventOut) {
    std::uint8_t rawKind = 0u;
    if (eventOut == nullptr ||
        !reader.readU8(&rawKind) ||
        rawKind > static_cast<std::uint8_t>(net::SnapshotEventKind::PlayerKilled) ||
        !readInt(reader, &eventOut->sourcePlayerId) ||
        !readInt(reader, &eventOut->targetEntityId) ||
        !readVec3(reader, &eventOut->origin) ||
        !readVec3(reader, &eventOut->direction) ||
        !reader.readBool(&eventOut->hit)) {
        return false;
    }
    eventOut->kind = static_cast<net::SnapshotEventKind>(rawKind);
    return true;
}

void writeHeader(ByteWriter& writer, const ReplayHeader& header) {
    writer.writeU32(header.formatVersion);
    writer.writeU16(header.protocolVersion);
    writer.writeI32(header.levelSlot);
    writer.writeU32(header.levelHash);
    writer.writeU16(header.tickRateHz);
    writer.writeU16(header.snapshotRateHz);
    writer.writeU32(header.simConfigVersion);
    writer.writeU64(header.recordedAtUnixSeconds);
    writer.writeU64(header.startedServerTimeUs);
    writer.writeU64(header.durationUs);
    writer.writeU32(header.maxRewindMs);
    writer.writeF32(header.respawnDelaySeconds);
    writer.writeF32(header.spawnProtectionSeconds);
    writer.writeU8(static_cast<std::uint8_t>(header.shotEvaluationMode));
    writer.writeBool(header.hasControlLane);
    writer.writeBool(header.hasClientPerceptionTracks);
    writer.writeString(header.title);
    writer.writeString(header.sourceLabel);
}

bool readHeader(ByteReader& reader, ReplayHeader* headerOut) {
    if (headerOut == nullptr ||
        !reader.readU32(&headerOut->formatVersion) ||
        (headerOut->formatVersion != kCommandReplayFormatVersion &&
         headerOut->formatVersion != kLegacyCommandReplayFormatVersion) ||
        !reader.readU16(&headerOut->protocolVersion) ||
        !readInt(reader, &headerOut->levelSlot) ||
        !reader.readU32(&headerOut->levelHash) ||
        !reader.readU16(&headerOut->tickRateHz) ||
        !reader.readU16(&headerOut->snapshotRateHz) ||
        !reader.readU32(&headerOut->simConfigVersion) ||
        !reader.readU64(&headerOut->recordedAtUnixSeconds) ||
        !reader.readU64(&headerOut->startedServerTimeUs) ||
        !reader.readU64(&headerOut->durationUs)) {
        return false;
    }

    if (headerOut->formatVersion >= kCommandReplayFormatVersion) {
        std::uint8_t rawShotMode = 0u;
        if (!reader.readU32(&headerOut->maxRewindMs) ||
            !reader.readF32(&headerOut->respawnDelaySeconds) ||
            !reader.readF32(&headerOut->spawnProtectionSeconds) ||
            !reader.readU8(&rawShotMode) ||
            !net::tryParseShotEvaluationMode(rawShotMode, &headerOut->shotEvaluationMode)) {
            return false;
        }
    }

    return reader.readBool(&headerOut->hasControlLane) &&
           reader.readBool(&headerOut->hasClientPerceptionTracks) &&
           reader.readString(&headerOut->title) &&
           reader.readString(&headerOut->sourceLabel);
}

}  // namespace

bool ReplayArchive::save(const ReplayDemo& demo,
                         const std::filesystem::path& path,
                         std::string* errorOut) const {
    ByteBuffer bytes;
    if (!writeBytes(demo, &bytes, errorOut)) {
        return false;
    }
    if (!writeFileBytes(path, bytes)) {
        setError(errorOut, "failed to write command replay file");
        return false;
    }
    return true;
}

bool ReplayArchive::load(const std::filesystem::path& path,
                         ReplayDemo* demoOut,
                         std::string* errorOut) const {
    ByteBuffer bytes;
    if (!readFileBytes(path, &bytes)) {
        setError(errorOut, "failed to read command replay file");
        return false;
    }
    return readBytes(bytes, demoOut, errorOut);
}

bool ReplayArchive::writeBytes(const ReplayDemo& demo,
                               ByteBuffer* bytesOut,
                               std::string* errorOut) {
    if (bytesOut == nullptr) {
        setError(errorOut, "missing output buffer");
        return false;
    }
    if (demo.header.formatVersion != kCommandReplayFormatVersion) {
        setError(errorOut, "unsupported command replay format version");
        return false;
    }

    ByteWriter writer;
    writer.writeU32(kCommandReplayMagic);
    writeHeader(writer, demo.header);
    writeSimConfig(writer, demo.initialState.simConfig);
    writeWorldState(writer, demo.initialState.worldState);

    writer.writeU32(static_cast<std::uint32_t>(demo.commandEvents.size()));
    for (const ServerCommandEvent& event : demo.commandEvents) {
        writeCommandEvent(writer, event);
    }

    writer.writeU32(static_cast<std::uint32_t>(demo.runtimeEvents.size()));
    for (const ReplayRuntimeEvent& event : demo.runtimeEvents) {
        writer.writeU8(static_cast<std::uint8_t>(event.kind));
        writer.writeU32(event.serverTick);
        writer.writeU64(event.serverTimeUs);
        writer.writeU16(event.peerId);
        writer.writeString(event.key);
        writer.writeF32(event.value);
        writer.writeBool(event.applied);
    }

    writer.writeU32(static_cast<std::uint32_t>(demo.combatEvents.size()));
    for (const ReplayCombatEvent& event : demo.combatEvents) {
        writer.writeU32(event.serverTick);
        writer.writeU64(event.serverTimeUs);
        writeSnapshotEvent(writer, event.event);
    }

    writer.writeU32(static_cast<std::uint32_t>(demo.keyframes.size()));
    for (const WorldKeyframe& keyframe : demo.keyframes) {
        writer.writeU8(static_cast<std::uint8_t>(keyframe.track));
        writer.writeU32(keyframe.serverTick);
        writer.writeU64(keyframe.serverTimeUs);
        writeWorldState(writer, keyframe.worldState);
        writer.writeU32(static_cast<std::uint32_t>(keyframe.controlPlayers.size()));
        for (const sim::PlayerState& player : keyframe.controlPlayers) {
            writePlayerState(writer, player);
        }
    }

    writer.writeU32(static_cast<std::uint32_t>(demo.clientTracks.size()));
    for (const ClientPerceptionTrack& track : demo.clientTracks) {
        writer.writeU16(track.peerId);
        writer.writeString(track.playerName);
        writer.writeU32(static_cast<std::uint32_t>(track.events.size()));
        for (const ClientPerceptionEvent& event : track.events) {
            writer.writeU8(static_cast<std::uint8_t>(event.kind));
            writer.writeU32(event.clientFrame);
            writer.writeU64(event.clientTimeUs);
            writer.writeU32(event.serverTick);
            writer.writeU64(event.serverTimeUs);
            writer.writeU32(event.commandSeq);
            writer.writeF32(event.correctionMagnitude);
            writer.writeF32(event.yaw);
            writer.writeF32(event.pitch);
            writer.writeBool(event.predictionEnabled);
            writer.writeBool(event.interpolationEnabled);
        }
    }

    *bytesOut = std::move(writer).take();
    return true;
}

bool ReplayArchive::readBytes(const ByteBuffer& bytes,
                              ReplayDemo* demoOut,
                              std::string* errorOut) {
    if (demoOut == nullptr) {
        setError(errorOut, "missing replay output");
        return false;
    }

    ByteReader reader(bytes);
    std::uint32_t magic = 0u;
    ReplayDemo demo;
    if (!reader.readU32(&magic) || magic != kCommandReplayMagic ||
        !readHeader(reader, &demo.header) ||
        !readSimConfig(reader, &demo.initialState.simConfig) ||
        !readWorldState(reader, &demo.initialState.worldState)) {
        setError(errorOut, "invalid command replay header");
        return false;
    }

    std::uint32_t count = 0u;
    if (!readCount(reader, kMaxReplayCommands, &count)) {
        setError(errorOut, "invalid command replay command count");
        return false;
    }
    demo.commandEvents.resize(count);
    for (ServerCommandEvent& event : demo.commandEvents) {
        if (!readCommandEvent(reader, &event)) {
            setError(errorOut, "invalid command replay command event");
            return false;
        }
    }

    if (!readCount(reader, kMaxReplayEvents, &count)) {
        setError(errorOut, "invalid command replay runtime event count");
        return false;
    }
    demo.runtimeEvents.resize(count);
    for (ReplayRuntimeEvent& event : demo.runtimeEvents) {
        std::uint8_t rawKind = 0u;
        if (!reader.readU8(&rawKind) ||
            rawKind > static_cast<std::uint8_t>(ReplayRuntimeEventKind::SessionAction) ||
            !reader.readU32(&event.serverTick) ||
            !reader.readU64(&event.serverTimeUs) ||
            !reader.readU16(&event.peerId) ||
            !reader.readString(&event.key) ||
            !reader.readF32(&event.value) ||
            !reader.readBool(&event.applied)) {
            setError(errorOut, "invalid command replay runtime event");
            return false;
        }
        event.kind = static_cast<ReplayRuntimeEventKind>(rawKind);
    }

    if (!readCount(reader, kMaxReplayEvents, &count)) {
        setError(errorOut, "invalid command replay combat event count");
        return false;
    }
    demo.combatEvents.resize(count);
    for (ReplayCombatEvent& event : demo.combatEvents) {
        if (!reader.readU32(&event.serverTick) ||
            !reader.readU64(&event.serverTimeUs) ||
            !readSnapshotEvent(reader, &event.event)) {
            setError(errorOut, "invalid command replay combat event");
            return false;
        }
    }

    if (!readCount(reader, kMaxReplayKeyframes, &count)) {
        setError(errorOut, "invalid command replay keyframe count");
        return false;
    }
    demo.keyframes.resize(count);
    for (WorldKeyframe& keyframe : demo.keyframes) {
        std::uint32_t playerCount = 0u;
        if (!readTrack(reader, &keyframe.track) ||
            !reader.readU32(&keyframe.serverTick) ||
            !reader.readU64(&keyframe.serverTimeUs) ||
            !readWorldState(reader, &keyframe.worldState) ||
            !readCount(reader, kMaxWorldPlayers, &playerCount)) {
            setError(errorOut, "invalid command replay keyframe");
            return false;
        }
        keyframe.controlPlayers.resize(playerCount);
        for (sim::PlayerState& player : keyframe.controlPlayers) {
            if (!readPlayerState(reader, &player)) {
                setError(errorOut, "invalid command replay keyframe player");
                return false;
            }
        }
    }

    if (!readCount(reader, kMaxReplayClients, &count)) {
        setError(errorOut, "invalid command replay client track count");
        return false;
    }
    demo.clientTracks.resize(count);
    for (ClientPerceptionTrack& track : demo.clientTracks) {
        std::uint32_t eventCount = 0u;
        if (!reader.readU16(&track.peerId) ||
            !reader.readString(&track.playerName) ||
            !readCount(reader, kMaxReplayClientEvents, &eventCount)) {
            setError(errorOut, "invalid command replay client track");
            return false;
        }
        track.events.resize(eventCount);
        for (ClientPerceptionEvent& event : track.events) {
            std::uint8_t rawKind = 0u;
            if (!reader.readU8(&rawKind) ||
                rawKind > static_cast<std::uint8_t>(ClientPerceptionEventKind::LocalPendingShot) ||
                !reader.readU32(&event.clientFrame) ||
                !reader.readU64(&event.clientTimeUs) ||
                !reader.readU32(&event.serverTick) ||
                !reader.readU64(&event.serverTimeUs) ||
                !reader.readU32(&event.commandSeq) ||
                !reader.readF32(&event.correctionMagnitude) ||
                !reader.readF32(&event.yaw) ||
                !reader.readF32(&event.pitch) ||
                !reader.readBool(&event.predictionEnabled) ||
                !reader.readBool(&event.interpolationEnabled)) {
                setError(errorOut, "invalid command replay client event");
                return false;
            }
            event.kind = static_cast<ClientPerceptionEventKind>(rawKind);
        }
    }

    if (!reader.atEnd()) {
        setError(errorOut, "trailing bytes in command replay file");
        return false;
    }

    *demoOut = std::move(demo);
    return true;
}

}  // namespace replay
