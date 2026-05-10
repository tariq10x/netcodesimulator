#include "net/Codec.hpp"
#include "net/Protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace net {
namespace {

using codec::ByteReader;
using codec::ByteWriter;

template <typename>
struct DependentFalse : std::false_type {};

void writeControlBinding(ByteWriter& writer, const sim::ControlBinding& binding);
ParseError readControlBinding(ByteReader& reader, sim::ControlBinding* binding);
void writeParticipantState(ByteWriter& writer, const sim::ParticipantState& state);
ParseError readParticipantState(ByteReader& reader, sim::ParticipantState* state);
void writePaneViewState(ByteWriter& writer, const sim::PaneViewState& state);
ParseError readPaneViewState(ByteReader& reader, sim::PaneViewState* state);
void writeAuthoritativeTime(ByteWriter& writer, const sim::AuthoritativeTime& time);
bool readAuthoritativeTime(ByteReader& reader, sim::AuthoritativeTime* time);
void writeVec3(ByteWriter& writer, const sim::Vec3& value);
bool readVec3(ByteReader& reader, sim::Vec3* value);
void writePlayerState(ByteWriter& writer, const sim::PlayerState& state);
bool readPlayerState(ByteReader& reader, sim::PlayerState* state);
void writeRemoteActorState(ByteWriter& writer, const sim::RemoteActorState& state);
bool readRemoteActorState(ByteReader& reader, sim::RemoteActorState* state);
void writeRosterEntry(ByteWriter& writer, const sim::RosterEntry& entry);
ParseError readRosterEntry(ByteReader& reader, sim::RosterEntry* entry);
void writeTeamScores(ByteWriter& writer, const sim::TeamScores& scores);
bool readTeamScores(ByteReader& reader, sim::TeamScores* scores);
void writeTimingCadence(ByteWriter& writer, const sim::TimingCadence& cadence);
bool readTimingCadence(ByteReader& reader, sim::TimingCadence* cadence);
void writeHostedSessionMetadata(ByteWriter& writer, const HostedSessionMetadata& metadata);
ParseError readHostedSessionMetadata(ByteReader& reader, HostedSessionMetadata* metadata);
void writeWorldSnapshotMessage(ByteWriter& writer, const WorldSnapshot& message);
ParseError readWorldSnapshotMessage(ByteReader& reader, WorldSnapshot* message);
bool tryReadSessionActionKind(std::uint8_t rawKind, SessionActionKind* kindOut);

bool simVecEqual(const sim::Vec3& lhs, const sim::Vec3& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

bool playerStateEqual(const sim::PlayerState& lhs, const sim::PlayerState& rhs) {
    return lhs.playerId == rhs.playerId &&
           simVecEqual(lhs.position, rhs.position) &&
           simVecEqual(lhs.velocity, rhs.velocity) &&
           lhs.yaw == rhs.yaw &&
           lhs.pitch == rhs.pitch &&
           lhs.health == rhs.health &&
           lhs.maxHealth == rhs.maxHealth &&
           lhs.weaponCooldownRemaining == rhs.weaponCooldownRemaining &&
           lhs.jumpsUsed == rhs.jumpsUsed &&
           lhs.grounded == rhs.grounded;
}

bool remoteActorEqual(const sim::RemoteActorState& lhs, const sim::RemoteActorState& rhs) {
    return lhs.entityId == rhs.entityId &&
           simVecEqual(lhs.position, rhs.position) &&
           simVecEqual(lhs.velocity, rhs.velocity) &&
           lhs.yaw == rhs.yaw &&
           lhs.pitch == rhs.pitch &&
           lhs.health == rhs.health &&
           lhs.radius == rhs.radius &&
           lhs.alive == rhs.alive;
}

bool rosterEntryEqual(const sim::RosterEntry& lhs, const sim::RosterEntry& rhs) {
    return lhs.actorId == rhs.actorId &&
           lhs.team == rhs.team &&
           lhs.sessionPresence == rhs.sessionPresence &&
           lhs.participation == rhs.participation &&
           lhs.control.kind == rhs.control.kind &&
           lhs.control.actorId == rhs.control.actorId &&
           lhs.isBot == rhs.isBot &&
           lhs.kills == rhs.kills &&
           lhs.deaths == rhs.deaths &&
           lhs.assists == rhs.assists &&
           lhs.alive == rhs.alive &&
           lhs.interpolationEnabled == rhs.interpolationEnabled &&
           lhs.predictionEnabled == rhs.predictionEnabled &&
           lhs.reconciliationStrategy == rhs.reconciliationStrategy &&
           lhs.smoothCorrectionWindowMs == rhs.smoothCorrectionWindowMs &&
           lhs.latencyMs == rhs.latencyMs &&
           lhs.lossPct == rhs.lossPct &&
           lhs.displayName == rhs.displayName;
}

bool teamScoresEqual(const sim::TeamScores& lhs, const sim::TeamScores& rhs) {
    return lhs.attackers == rhs.attackers &&
           lhs.defenders == rhs.defenders;
}

bool timingCadenceEqual(const sim::TimingCadence& lhs, const sim::TimingCadence& rhs) {
    return lhs.authoritativeTickHz == rhs.authoritativeTickHz &&
           lhs.snapshotCadenceHz == rhs.snapshotCadenceHz &&
           lhs.commandCadenceHz == rhs.commandCadenceHz;
}

bool authoritativeTimeEqual(const sim::AuthoritativeTime& lhs,
                            const sim::AuthoritativeTime& rhs) {
    return lhs.serverTick == rhs.serverTick &&
           lhs.serverTimeUs == rhs.serverTimeUs &&
           lhs.viewedServerTimeUs == rhs.viewedServerTimeUs;
}

bool controlBindingEqual(const sim::ControlBinding& lhs,
                         const sim::ControlBinding& rhs) {
    return lhs.kind == rhs.kind &&
           lhs.actorId == rhs.actorId;
}

bool participantStateEqual(const sim::ParticipantState& lhs,
                           const sim::ParticipantState& rhs) {
    return lhs.presence == rhs.presence &&
           lhs.team == rhs.team &&
           lhs.participation == rhs.participation &&
           controlBindingEqual(lhs.control, rhs.control);
}

bool paneViewStateEqual(const sim::PaneViewState& lhs,
                        const sim::PaneViewState& rhs) {
    return lhs.slot == rhs.slot &&
           lhs.mode == rhs.mode &&
           lhs.focused == rhs.focused &&
           lhs.followTargetActorId == rhs.followTargetActorId;
}

bool userCmdEqual(const sim::UserCmd& lhs, const sim::UserCmd& rhs) {
    return lhs.dtSeconds == rhs.dtSeconds &&
           lhs.moveX == rhs.moveX &&
           lhs.moveY == rhs.moveY &&
           lhs.yaw == rhs.yaw &&
           lhs.pitch == rhs.pitch &&
           lhs.buttons == rhs.buttons;
}

bool gameplayEventEqual(const GameplayEvent& lhs, const GameplayEvent& rhs) {
    return lhs.kind == rhs.kind &&
           lhs.sourcePlayerId == rhs.sourcePlayerId &&
           lhs.targetEntityId == rhs.targetEntityId &&
           simVecEqual(lhs.origin, rhs.origin) &&
           simVecEqual(lhs.direction, rhs.direction) &&
           lhs.hit == rhs.hit;
}

void writeWorldSnapshotMessage(ByteWriter& writer, const WorldSnapshot& message) {
    const ReplicationSnapshot& replication = message.replication();
    const SessionSummary& summary = message.summary();
    const GameplayEventBatch& gameplayEvents = message.gameplayEvents();

    writer.writeU32(replication.serverTick);
    writer.writeU64(replication.serverTimeUs);
    writer.writeU32(replication.ackedInputSeq);
    writeTimingCadence(writer, replication.cadence);
    writeAuthoritativeTime(writer, replication.authoritativeTime);
    writeParticipantState(writer, replication.localParticipantState);
    writePaneViewState(writer, replication.localPaneView);
    writeHostedSessionMetadata(writer, summary.sessionMetadata);
    writePlayerState(writer, replication.localPlayerState);
    writer.writeU16(static_cast<std::uint16_t>(replication.remotePlayers.size()));
    for (const sim::PlayerState& player : replication.remotePlayers) {
        writePlayerState(writer, player);
    }
    writer.writeU16(static_cast<std::uint16_t>(replication.controlRemotePlayers.size()));
    for (const sim::PlayerState& player : replication.controlRemotePlayers) {
        writePlayerState(writer, player);
    }
    writer.writeU16(static_cast<std::uint16_t>(replication.remoteEnemies.size()));
    for (const sim::RemoteActorState& entity : replication.remoteEnemies) {
        writeRemoteActorState(writer, entity);
    }
    writer.writeU16(static_cast<std::uint16_t>(summary.roster.size()));
    for (const sim::RosterEntry& entry : summary.roster) {
        writeRosterEntry(writer, entry);
    }
    writeTeamScores(writer, summary.teamScores);
    writer.writeU16(static_cast<std::uint16_t>(gameplayEvents.events.size()));
    for (const GameplayEvent& event : gameplayEvents.events) {
        writer.writeU8(static_cast<std::uint8_t>(event.kind));
        writer.writeI32(event.sourcePlayerId);
        writer.writeI32(event.targetEntityId);
        writeVec3(writer, event.origin);
        writeVec3(writer, event.direction);
        writer.writeBool(event.hit);
    }
}

ParseError readWorldSnapshotMessage(ByteReader& reader, WorldSnapshot* message) {
    if (message == nullptr) {
        return ParseError::BufferUnderflow;
    }

    ReplicationSnapshot& replication = message->replication();
    SessionSummary& summary = message->summary();
    GameplayEventBatch& gameplayEvents = message->gameplayEvents();
    std::uint16_t remotePlayerCount = 0;
    std::uint16_t controlRemotePlayerCount = 0;
    std::uint16_t remoteEnemyCount = 0;
    std::uint16_t rosterCount = 0;
    std::uint16_t eventCount = 0;
    if (!reader.readU32(&replication.serverTick) ||
        !reader.readU64(&replication.serverTimeUs) ||
        !reader.readU32(&replication.ackedInputSeq) ||
        !readTimingCadence(reader, &replication.cadence)) {
        return ParseError::BufferUnderflow;
    }
    if (!readAuthoritativeTime(reader, &replication.authoritativeTime)) {
        return ParseError::BufferUnderflow;
    }
    {
        const ParseError participantError =
            readParticipantState(reader, &replication.localParticipantState);
        if (participantError != ParseError::None) {
            return participantError;
        }
    }
    {
        const ParseError paneError = readPaneViewState(reader, &replication.localPaneView);
        if (paneError != ParseError::None) {
            return paneError;
        }
    }
    const ParseError metadataError = readHostedSessionMetadata(reader, &summary.sessionMetadata);
    if (metadataError != ParseError::None) {
        return metadataError;
    }
    if (!readPlayerState(reader, &replication.localPlayerState) ||
        !reader.readU16(&remotePlayerCount)) {
        return ParseError::BufferUnderflow;
    }
    replication.remotePlayers.resize(remotePlayerCount);
    for (auto& player : replication.remotePlayers) {
        if (!readPlayerState(reader, &player)) {
            return ParseError::BufferUnderflow;
        }
    }
    if (!reader.readU16(&controlRemotePlayerCount)) {
        return ParseError::BufferUnderflow;
    }
    replication.controlRemotePlayers.resize(controlRemotePlayerCount);
    for (auto& player : replication.controlRemotePlayers) {
        if (!readPlayerState(reader, &player)) {
            return ParseError::BufferUnderflow;
        }
    }
    if (!reader.readU16(&remoteEnemyCount)) {
        return ParseError::BufferUnderflow;
    }
    replication.remoteEnemies.resize(remoteEnemyCount);
    for (auto& entity : replication.remoteEnemies) {
        if (!readRemoteActorState(reader, &entity)) {
            return ParseError::BufferUnderflow;
        }
    }
    if (!reader.readU16(&rosterCount)) {
        return ParseError::BufferUnderflow;
    }
    summary.roster.resize(rosterCount);
    for (auto& entry : summary.roster) {
        const ParseError rosterError = readRosterEntry(reader, &entry);
        if (rosterError != ParseError::None) {
            return rosterError;
        }
    }
    if (!readTeamScores(reader, &summary.teamScores)) {
        return ParseError::BufferUnderflow;
    }
    if (!reader.readU16(&eventCount)) {
        return ParseError::BufferUnderflow;
    }
    gameplayEvents.events.resize(eventCount);
    for (auto& event : gameplayEvents.events) {
        std::uint8_t eventKind = 0;
        if (!reader.readU8(&eventKind) ||
            !reader.readI32(&event.sourcePlayerId) ||
            !reader.readI32(&event.targetEntityId) ||
            !readVec3(reader, &event.origin) ||
            !readVec3(reader, &event.direction) ||
            !reader.readBool(&event.hit)) {
            return ParseError::BufferUnderflow;
        }
        if (eventKind > static_cast<std::uint8_t>(SnapshotEventKind::PlayerKilled)) {
            return ParseError::InvalidEventKind;
        }
        event.kind = static_cast<SnapshotEventKind>(eventKind);
    }

    return ParseError::None;
}

bool playerCommandEqual(const sim::PlayerCommand& lhs, const sim::PlayerCommand& rhs) {
    return lhs.seq == rhs.seq &&
           userCmdEqual(lhs.toUserCmd(), rhs.toUserCmd()) &&
           lhs.toCommandTiming().viewedServerTimeUs == rhs.toCommandTiming().viewedServerTimeUs &&
           lhs.toCommandTiming().interpolationDelayMs == rhs.toCommandTiming().interpolationDelayMs &&
           lhs.toControlTiming().viewedServerTimeUs == rhs.toControlTiming().viewedServerTimeUs &&
           lhs.toControlTiming().interpolationDelayMs == rhs.toControlTiming().interpolationDelayMs &&
           lhs.requestedTeam == rhs.requestedTeam &&
           lhs.reportedLatencyMs == rhs.reportedLatencyMs &&
           lhs.reportedLossPct == rhs.reportedLossPct;
}

bool tryReadTeamId(std::uint8_t rawTeam, sim::TeamId* teamOut) {
    if (teamOut == nullptr) {
        return false;
    }

    if (rawTeam > static_cast<std::uint8_t>(sim::TeamId::Spectator)) {
        return false;
    }

    *teamOut = static_cast<sim::TeamId>(rawTeam);
    return true;
}

bool tryReadSessionActionKind(std::uint8_t rawKind, SessionActionKind* kindOut) {
    if (kindOut == nullptr) {
        return false;
    }

    if (rawKind > static_cast<std::uint8_t>(SessionActionKind::SpawnFrozenBotAhead)) {
        return false;
    }

    *kindOut = static_cast<SessionActionKind>(rawKind);
    return true;
}

bool tryReadSessionPresence(std::uint8_t rawPresence, sim::SessionPresence* presenceOut) {
    if (presenceOut == nullptr) {
        return false;
    }

    switch (rawPresence) {
        case static_cast<std::uint8_t>(sim::SessionPresence::Connecting):
            *presenceOut = sim::SessionPresence::Connecting;
            return true;
        case static_cast<std::uint8_t>(sim::SessionPresence::Connected):
            *presenceOut = sim::SessionPresence::Connected;
            return true;
        case static_cast<std::uint8_t>(sim::SessionPresence::Disconnected):
            *presenceOut = sim::SessionPresence::Disconnected;
            return true;
        default:
            return false;
    }
}

bool tryReadParticipationState(std::uint8_t rawState, sim::ParticipationState* stateOut) {
    if (stateOut == nullptr) {
        return false;
    }

    switch (rawState) {
        case static_cast<std::uint8_t>(sim::ParticipationState::TeamSelection):
            *stateOut = sim::ParticipationState::TeamSelection;
            return true;
        case static_cast<std::uint8_t>(sim::ParticipationState::Playing):
            *stateOut = sim::ParticipationState::Playing;
            return true;
        case static_cast<std::uint8_t>(sim::ParticipationState::Spectating):
            *stateOut = sim::ParticipationState::Spectating;
            return true;
        default:
            return false;
    }
}

bool tryReadControlBindingKind(std::uint8_t rawKind, sim::ControlBindingKind* kindOut) {
    if (kindOut == nullptr) {
        return false;
    }

    switch (rawKind) {
        case static_cast<std::uint8_t>(sim::ControlBindingKind::None):
            *kindOut = sim::ControlBindingKind::None;
            return true;
        case static_cast<std::uint8_t>(sim::ControlBindingKind::Actor):
            *kindOut = sim::ControlBindingKind::Actor;
            return true;
        default:
            return false;
    }
}

bool tryReadPaneSlot(std::uint8_t rawSlot, sim::PaneSlot* slotOut) {
    if (slotOut == nullptr) {
        return false;
    }

    switch (rawSlot) {
        case static_cast<std::uint8_t>(sim::PaneSlot::Left):
            *slotOut = sim::PaneSlot::Left;
            return true;
        case static_cast<std::uint8_t>(sim::PaneSlot::Right):
            *slotOut = sim::PaneSlot::Right;
            return true;
        default:
            return false;
    }
}

bool tryReadPaneViewMode(std::uint8_t rawMode, sim::PaneViewMode* modeOut) {
    if (modeOut == nullptr) {
        return false;
    }

    switch (rawMode) {
        case static_cast<std::uint8_t>(sim::PaneViewMode::PlayerControlled):
            *modeOut = sim::PaneViewMode::PlayerControlled;
            return true;
        case static_cast<std::uint8_t>(sim::PaneViewMode::SpectatorFreeFly):
            *modeOut = sim::PaneViewMode::SpectatorFreeFly;
            return true;
        case static_cast<std::uint8_t>(sim::PaneViewMode::SpectatorFollowFirstPerson):
            *modeOut = sim::PaneViewMode::SpectatorFollowFirstPerson;
            return true;
        case static_cast<std::uint8_t>(sim::PaneViewMode::SpectatorFollowThirdPerson):
            *modeOut = sim::PaneViewMode::SpectatorFollowThirdPerson;
            return true;
        case static_cast<std::uint8_t>(sim::PaneViewMode::ReplayCamera):
            *modeOut = sim::PaneViewMode::ReplayCamera;
            return true;
        default:
            return false;
    }
}

bool tryReadRuntimeReconciliationStrategy(std::uint8_t rawStrategy,
                                          sim::RuntimeReconciliationStrategy* strategyOut) {
    if (strategyOut == nullptr) {
        return false;
    }

    switch (rawStrategy) {
        case static_cast<std::uint8_t>(sim::RuntimeReconciliationStrategy::Snap):
            *strategyOut = sim::RuntimeReconciliationStrategy::Snap;
            return true;
        case static_cast<std::uint8_t>(sim::RuntimeReconciliationStrategy::Smooth):
            *strategyOut = sim::RuntimeReconciliationStrategy::Smooth;
            return true;
        default:
            return false;
    }
}

bool tryReadShotEvaluationMode(std::uint8_t rawMode, ShotEvaluationMode* modeOut) {
    if (modeOut == nullptr) {
        return false;
    }

    switch (rawMode) {
        case static_cast<std::uint8_t>(ShotEvaluationMode::SeenPosition):
            *modeOut = ShotEvaluationMode::SeenPosition;
            return true;
        case static_cast<std::uint8_t>(ShotEvaluationMode::LivePosition):
            *modeOut = ShotEvaluationMode::LivePosition;
            return true;
        default:
            return false;
    }
}

bool tryReadRuntimeParamScope(std::uint8_t rawScope, RuntimeParamScope* scopeOut) {
    if (scopeOut == nullptr) {
        return false;
    }

    switch (rawScope) {
        case static_cast<std::uint8_t>(RuntimeParamScope::Global):
            *scopeOut = RuntimeParamScope::Global;
            return true;
        case static_cast<std::uint8_t>(RuntimeParamScope::Player):
            *scopeOut = RuntimeParamScope::Player;
            return true;
        case static_cast<std::uint8_t>(RuntimeParamScope::Bot):
            *scopeOut = RuntimeParamScope::Bot;
            return true;
        case static_cast<std::uint8_t>(RuntimeParamScope::Session):
            *scopeOut = RuntimeParamScope::Session;
            return true;
        default:
            return false;
    }
}

bool tryReadStagedApplyBoundary(std::uint8_t rawBoundary,
                                sim::StagedApplyBoundary* boundaryOut) {
    if (boundaryOut == nullptr) {
        return false;
    }

    switch (rawBoundary) {
        case static_cast<std::uint8_t>(sim::StagedApplyBoundary::NextTick):
            *boundaryOut = sim::StagedApplyBoundary::NextTick;
            return true;
        case static_cast<std::uint8_t>(sim::StagedApplyBoundary::NextSnapshot):
            *boundaryOut = sim::StagedApplyBoundary::NextSnapshot;
            return true;
        case static_cast<std::uint8_t>(sim::StagedApplyBoundary::NextSessionRestart):
            *boundaryOut = sim::StagedApplyBoundary::NextSessionRestart;
            return true;
        default:
            return false;
    }
}

void writeVec3(ByteWriter& writer, const sim::Vec3& value) {
    writer.writeF32(value.x);
    writer.writeF32(value.y);
    writer.writeF32(value.z);
}

bool readVec3(ByteReader& reader, sim::Vec3* value) {
    return reader.readF32(&value->x) &&
           reader.readF32(&value->y) &&
           reader.readF32(&value->z);
}

void writePlayerState(ByteWriter& writer, const sim::PlayerState& state) {
    writer.writeI32(state.playerId);
    writeVec3(writer, state.position);
    writeVec3(writer, state.velocity);
    writer.writeF32(state.yaw);
    writer.writeF32(state.pitch);
    writer.writeF32(state.health);
    writer.writeF32(state.maxHealth);
    writer.writeF32(state.weaponCooldownRemaining);
    writer.writeI32(state.jumpsUsed);
    writer.writeBool(state.grounded);
}

bool readPlayerState(ByteReader& reader, sim::PlayerState* state) {
    return reader.readI32(&state->playerId) &&
           readVec3(reader, &state->position) &&
           readVec3(reader, &state->velocity) &&
           reader.readF32(&state->yaw) &&
           reader.readF32(&state->pitch) &&
           reader.readF32(&state->health) &&
           reader.readF32(&state->maxHealth) &&
           reader.readF32(&state->weaponCooldownRemaining) &&
           reader.readI32(&state->jumpsUsed) &&
           reader.readBool(&state->grounded);
}

void writeRemoteActorState(ByteWriter& writer, const sim::RemoteActorState& state) {
    writer.writeI32(state.entityId);
    writeVec3(writer, state.position);
    writeVec3(writer, state.velocity);
    writer.writeF32(state.yaw);
    writer.writeF32(state.pitch);
    writer.writeF32(state.health);
    writer.writeF32(state.radius);
    writer.writeBool(state.alive);
}

bool readRemoteActorState(ByteReader& reader, sim::RemoteActorState* state) {
    return reader.readI32(&state->entityId) &&
           readVec3(reader, &state->position) &&
           readVec3(reader, &state->velocity) &&
           reader.readF32(&state->yaw) &&
           reader.readF32(&state->pitch) &&
           reader.readF32(&state->health) &&
           reader.readF32(&state->radius) &&
           reader.readBool(&state->alive);
}

void writeRosterEntry(ByteWriter& writer, const sim::RosterEntry& entry) {
    writer.writeI32(entry.actorId);
    writer.writeU8(static_cast<std::uint8_t>(entry.team));
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

ParseError readRosterEntry(ByteReader& reader, sim::RosterEntry* entry) {
    if (entry == nullptr) {
        return ParseError::BufferUnderflow;
    }

    std::uint8_t teamValue = 0;
    std::uint8_t sessionPresenceValue = 0;
    std::uint8_t participationValue = 0;
    if (!reader.readI32(&entry->actorId) ||
        !reader.readU8(&teamValue) ||
        !reader.readU8(&sessionPresenceValue) ||
        !reader.readU8(&participationValue)) {
        return ParseError::BufferUnderflow;
    }

    if (!tryReadTeamId(teamValue, &entry->team)) {
        return ParseError::InvalidTeamId;
    }
    if (!tryReadSessionPresence(sessionPresenceValue, &entry->sessionPresence)) {
        return ParseError::InvalidSessionPresence;
    }
    if (!tryReadParticipationState(participationValue, &entry->participation)) {
        return ParseError::InvalidParticipationState;
    }

    const ParseError controlError = readControlBinding(reader, &entry->control);
    if (controlError != ParseError::None) {
        return controlError;
    }

    std::uint8_t reconciliationStrategy = 0u;
    if (!reader.readBool(&entry->isBot) ||
        !reader.readU16(&entry->kills) ||
        !reader.readU16(&entry->deaths) ||
        !reader.readU16(&entry->assists) ||
        !reader.readBool(&entry->alive) ||
        !reader.readBool(&entry->interpolationEnabled) ||
        !reader.readBool(&entry->predictionEnabled) ||
        !reader.readU8(&reconciliationStrategy) ||
        !reader.readU32(&entry->smoothCorrectionWindowMs) ||
        !reader.readU16(&entry->latencyMs) ||
        !reader.readU8(&entry->lossPct) ||
        !reader.readString(&entry->displayName)) {
        return ParseError::BufferUnderflow;
    }
    if (!tryReadRuntimeReconciliationStrategy(reconciliationStrategy,
                                              &entry->reconciliationStrategy)) {
        return ParseError::InvalidRuntimeReconciliationStrategy;
    }
    return ParseError::None;
}

void writeTeamScores(ByteWriter& writer, const sim::TeamScores& scores) {
    writer.writeU16(scores.attackers);
    writer.writeU16(scores.defenders);
}

bool readTeamScores(ByteReader& reader, sim::TeamScores* scores) {
    return reader.readU16(&scores->attackers) &&
           reader.readU16(&scores->defenders);
}

void writeTimingCadence(ByteWriter& writer, const sim::TimingCadence& cadence) {
    writer.writeU16(cadence.authoritativeTickHz);
    writer.writeU16(cadence.snapshotCadenceHz);
    writer.writeU16(cadence.commandCadenceHz);
}

bool readTimingCadence(ByteReader& reader, sim::TimingCadence* cadence) {
    return reader.readU16(&cadence->authoritativeTickHz) &&
           reader.readU16(&cadence->snapshotCadenceHz) &&
           reader.readU16(&cadence->commandCadenceHz);
}

void writeAuthoritativeTime(ByteWriter& writer, const sim::AuthoritativeTime& time) {
    writer.writeU32(time.serverTick);
    writer.writeU64(time.serverTimeUs);
    writer.writeU64(time.viewedServerTimeUs);
}

bool readAuthoritativeTime(ByteReader& reader, sim::AuthoritativeTime* time) {
    return reader.readU32(&time->serverTick) &&
           reader.readU64(&time->serverTimeUs) &&
           reader.readU64(&time->viewedServerTimeUs);
}

void writeControlBinding(ByteWriter& writer, const sim::ControlBinding& binding) {
    writer.writeU8(static_cast<std::uint8_t>(binding.kind));
    writer.writeI32(binding.actorId);
}

ParseError readControlBinding(ByteReader& reader, sim::ControlBinding* binding) {
    if (binding == nullptr) {
        return ParseError::BufferUnderflow;
    }

    std::uint8_t kindValue = 0u;
    if (!reader.readU8(&kindValue) ||
        !reader.readI32(&binding->actorId)) {
        return ParseError::BufferUnderflow;
    }

    if (!tryReadControlBindingKind(kindValue, &binding->kind)) {
        return ParseError::InvalidControlBindingKind;
    }

    return ParseError::None;
}

void writeParticipantState(ByteWriter& writer, const sim::ParticipantState& state) {
    writer.writeU8(static_cast<std::uint8_t>(state.presence));
    writer.writeU8(static_cast<std::uint8_t>(state.team));
    writer.writeU8(static_cast<std::uint8_t>(state.participation));
    writeControlBinding(writer, state.control);
}

ParseError readParticipantState(ByteReader& reader, sim::ParticipantState* state) {
    if (state == nullptr) {
        return ParseError::BufferUnderflow;
    }

    std::uint8_t presenceValue = 0u;
    std::uint8_t teamValue = 0u;
    std::uint8_t participationValue = 0u;
    if (!reader.readU8(&presenceValue) ||
        !reader.readU8(&teamValue) ||
        !reader.readU8(&participationValue)) {
        return ParseError::BufferUnderflow;
    }

    if (!tryReadSessionPresence(presenceValue, &state->presence)) {
        return ParseError::InvalidSessionPresence;
    }
    if (!tryReadTeamId(teamValue, &state->team)) {
        return ParseError::InvalidTeamId;
    }
    if (!tryReadParticipationState(participationValue, &state->participation)) {
        return ParseError::InvalidParticipationState;
    }
    return readControlBinding(reader, &state->control);
}

void writePaneViewState(ByteWriter& writer, const sim::PaneViewState& state) {
    writer.writeU8(static_cast<std::uint8_t>(state.slot));
    writer.writeU8(static_cast<std::uint8_t>(state.mode));
    writer.writeBool(state.focused);
    writer.writeI32(state.followTargetActorId);
}

ParseError readPaneViewState(ByteReader& reader, sim::PaneViewState* state) {
    if (state == nullptr) {
        return ParseError::BufferUnderflow;
    }

    std::uint8_t slotValue = 0u;
    std::uint8_t modeValue = 0u;
    if (!reader.readU8(&slotValue) ||
        !reader.readU8(&modeValue) ||
        !reader.readBool(&state->focused) ||
        !reader.readI32(&state->followTargetActorId)) {
        return ParseError::BufferUnderflow;
    }

    if (!tryReadPaneSlot(slotValue, &state->slot)) {
        return ParseError::InvalidPaneSlot;
    }
    if (!tryReadPaneViewMode(modeValue, &state->mode)) {
        return ParseError::InvalidPaneViewMode;
    }

    return ParseError::None;
}

void writePlayerCommand(ByteWriter& writer, const sim::PlayerCommand& command) {
    const sim::UserCmd userCmd = command.toUserCmd();
    const sim::CommandTiming timing = command.toCommandTiming();
    const sim::CommandTiming controlTiming = command.toControlTiming();
    writer.writeU32(command.seq);
    writer.writeF32(userCmd.dtSeconds);
    writer.writeF32(userCmd.moveX);
    writer.writeF32(userCmd.moveY);
    writer.writeF32(userCmd.yaw);
    writer.writeF32(userCmd.pitch);
    writer.writeU32(userCmd.buttons);
    writer.writeU64(timing.viewedServerTimeUs);
    writer.writeU32(timing.interpolationDelayMs);
    writer.writeU64(controlTiming.viewedServerTimeUs);
    writer.writeU32(controlTiming.interpolationDelayMs);
    writer.writeU8(static_cast<std::uint8_t>(command.requestedTeam));
    writer.writeU16(command.reportedLatencyMs);
    writer.writeU8(command.reportedLossPct);
}

bool readPlayerCommand(ByteReader& reader, sim::PlayerCommand* command) {
    sim::UserCmd userCmd;
    sim::CommandTiming timing;
    sim::CommandTiming controlTiming;
    std::uint8_t requestedTeam = 0;
    if (!reader.readU32(&command->seq) ||
        !reader.readF32(&userCmd.dtSeconds) ||
        !reader.readF32(&userCmd.moveX) ||
        !reader.readF32(&userCmd.moveY) ||
        !reader.readF32(&userCmd.yaw) ||
        !reader.readF32(&userCmd.pitch) ||
        !reader.readU32(&userCmd.buttons) ||
        !reader.readU64(&timing.viewedServerTimeUs) ||
        !reader.readU32(&timing.interpolationDelayMs) ||
        !reader.readU64(&controlTiming.viewedServerTimeUs) ||
        !reader.readU32(&controlTiming.interpolationDelayMs) ||
        !reader.readU8(&requestedTeam) ||
        !reader.readU16(&command->reportedLatencyMs) ||
        !reader.readU8(&command->reportedLossPct)) {
        return false;
    }

    command->applyUserCmd(userCmd);
    command->applyCommandTiming(timing);
    command->applyControlTiming(controlTiming);
    return tryReadTeamId(requestedTeam, &command->requestedTeam);
}

void writeHostedSessionMetadata(ByteWriter& writer, const HostedSessionMetadata& metadata) {
    writer.writeString(metadata.sessionLabel);
    writer.writeString(metadata.hostPlayerName);
    writer.writeU16(metadata.hostPeerId);
    writer.writeI32(metadata.levelSlot);
    writer.writeU32(metadata.levelHash);
    writer.writeU16(metadata.publicJoinPort);
    writer.writeU16(metadata.maxHumanPlayers);
    writer.writeU8(static_cast<std::uint8_t>(metadata.shotEvaluationMode));
    writer.writeBool(metadata.botsFrozen);
    writer.writeBool(metadata.botsCanShoot);
    writer.writeBool(metadata.studyEventLoggingEnabled);
    writer.writeString(metadata.studyEventRunId);
}

ParseError readHostedSessionMetadata(ByteReader& reader, HostedSessionMetadata* metadata) {
    if (metadata == nullptr) {
        return ParseError::BufferUnderflow;
    }

    std::uint8_t shotMode = 0u;
    if (!reader.readString(&metadata->sessionLabel) ||
        !reader.readString(&metadata->hostPlayerName) ||
        !reader.readU16(&metadata->hostPeerId) ||
        !reader.readI32(&metadata->levelSlot) ||
        !reader.readU32(&metadata->levelHash) ||
        !reader.readU16(&metadata->publicJoinPort) ||
        !reader.readU16(&metadata->maxHumanPlayers) ||
        !reader.readU8(&shotMode) ||
        !reader.readBool(&metadata->botsFrozen) ||
        !reader.readBool(&metadata->botsCanShoot) ||
        !reader.readBool(&metadata->studyEventLoggingEnabled) ||
        !reader.readString(&metadata->studyEventRunId)) {
        return ParseError::BufferUnderflow;
    }

    if (!tryReadShotEvaluationMode(shotMode, &metadata->shotEvaluationMode)) {
        return ParseError::InvalidShotEvaluationMode;
    }

    return ParseError::None;
}

template <typename T>
void writeRuntimeParamRecord(ByteWriter& writer, const T& message) {
    writer.writeU8(static_cast<std::uint8_t>(message.scope));
    writer.writeI32(message.targetId);
    writer.writeString(message.key);
    writer.writeF32(message.value);
}

template <typename T>
ParseError readRuntimeParamRecord(ByteReader& reader, T* message) {
    if (message == nullptr) {
        return ParseError::BufferUnderflow;
    }

    std::uint8_t scopeValue = 0;
    if (!reader.readU8(&scopeValue) ||
        !reader.readI32(&message->targetId) ||
        !reader.readString(&message->key) ||
        !reader.readF32(&message->value)) {
        return ParseError::BufferUnderflow;
    }

    if (!tryReadRuntimeParamScope(scopeValue, &message->scope)) {
        return ParseError::InvalidRuntimeParamScope;
    }

    return ParseError::None;
}

void writeHeader(ByteWriter& writer, const PacketHeader& header) {
    writer.writeU32(header.magic);
    writer.writeU16(header.version);
    writer.writeU16(header.peerId);
    writer.writeU8(static_cast<std::uint8_t>(header.channel));
    writer.writeU32(header.seq);
    writer.writeU32(header.ack);
    writer.writeU32(header.ackBits);
    writer.writeU8(static_cast<std::uint8_t>(header.kind));
}

ParseError readHeader(ByteReader& reader, PacketHeader* header) {
    std::uint8_t channelValue = 0;
    std::uint8_t kindValue = 0;

    if (!reader.readU32(&header->magic) ||
        !reader.readU16(&header->version) ||
        !reader.readU16(&header->peerId) ||
        !reader.readU8(&channelValue) ||
        !reader.readU32(&header->seq) ||
        !reader.readU32(&header->ack) ||
        !reader.readU32(&header->ackBits) ||
        !reader.readU8(&kindValue)) {
        return ParseError::BufferUnderflow;
    }

    if (header->magic != kProtocolMagic) {
        return ParseError::InvalidMagic;
    }
    if (header->version != kProtocolVersion) {
        return ParseError::UnsupportedVersion;
    }
    if (channelValue > static_cast<std::uint8_t>(Channel::ProxyStats)) {
        return ParseError::InvalidChannel;
    }
    if (kindValue > static_cast<std::uint8_t>(PacketKind::SessionActionResult)) {
        return ParseError::InvalidKind;
    }

    header->channel = static_cast<Channel>(channelValue);
    header->kind = static_cast<PacketKind>(kindValue);
    return ParseError::None;
}

void writePacketPayload(ByteWriter& writer, const PacketPayload& payload) {
    std::visit([&writer](const auto& message) {
        using T = std::decay_t<decltype(message)>;

        if constexpr (std::is_same_v<T, HelloMessage>) {
            writer.writeU32(message.sessionId);
            writer.writeU16(message.requestedPeerId);
            writer.writeString(message.playerName);
            writer.writeU8(static_cast<std::uint8_t>(message.requestedTeam));
        } else if constexpr (std::is_same_v<T, WelcomeMessage>) {
            HostedSessionMetadata metadata = message.sessionMetadata;
            metadata.levelSlot = message.levelSlot;
            metadata.levelHash = message.levelHash;
            writer.writeU32(message.sessionId);
            writer.writeU16(message.assignedPeerId);
            writer.writeU16(message.snapshotRateHz);
            writeTimingCadence(writer, message.cadence);
            writer.writeI32(message.levelSlot);
            writer.writeU32(message.levelHash);
            writeParticipantState(writer, message.participantState);
            writePaneViewState(writer, message.paneView);
            writeAuthoritativeTime(writer, message.authoritativeTime);
            writeHostedSessionMetadata(writer, metadata);
        } else if constexpr (std::is_same_v<T, DisconnectMessage>) {
            writer.writeU16(message.reasonCode);
            writer.writeString(message.reason);
        } else if constexpr (std::is_same_v<T, TeamChangeRequest>) {
            writer.writeU8(static_cast<std::uint8_t>(message.requestedTeam));
        } else if constexpr (std::is_same_v<T, RuntimeParamChangeRequest> ||
                             std::is_same_v<T, RuntimeParamSnapshot>) {
            writeRuntimeParamRecord(writer, message);
        } else if constexpr (std::is_same_v<T, RuntimeParamApplyResult>) {
            writeRuntimeParamRecord(writer, message);
            writer.writeBool(message.applied);
            writer.writeU8(static_cast<std::uint8_t>(message.stagedApplyBoundary));
            writer.writeString(message.message);
        } else if constexpr (std::is_same_v<T, SessionActionRequest>) {
            writer.writeU8(static_cast<std::uint8_t>(message.kind));
        } else if constexpr (std::is_same_v<T, SessionActionResult>) {
            writer.writeU8(static_cast<std::uint8_t>(message.kind));
            writer.writeBool(message.applied);
            writer.writeI32(message.actorId);
            writer.writeString(message.message);
        } else if constexpr (std::is_same_v<T, CommandBundle>) {
            writer.writeU16(static_cast<std::uint16_t>(message.commands.size()));
            for (const sim::PlayerCommand& command : message.commands) {
                writePlayerCommand(writer, command);
            }
        } else if constexpr (std::is_same_v<T, ControlCommandBundle>) {
            writer.writeU16(static_cast<std::uint16_t>(message.commands.size()));
            for (const sim::PlayerCommand& command : message.commands) {
                writePlayerCommand(writer, command);
            }
        } else if constexpr (std::is_same_v<T, WorldSnapshot>) {
            writeWorldSnapshotMessage(writer, message);
        } else if constexpr (std::is_same_v<T, ControlWorldSnapshot>) {
            writeWorldSnapshotMessage(writer, message.snapshot);
        } else if constexpr (std::is_same_v<T, ProxyControl>) {
            writer.writeU16(message.targetPeerId);
            writer.writeBool(message.upstream);
            writer.writeF32(message.config.baseDelayMs);
            writer.writeF32(message.config.jitterMs);
            writer.writeF32(message.config.lossPct);
            writer.writeF32(message.config.duplicatePct);
            writer.writeF32(message.config.reorderPct);
            writer.writeU32(message.config.seed);
        } else if constexpr (std::is_same_v<T, ProxyStats>) {
            writer.writeU64(message.receivedPackets);
            writer.writeU64(message.forwardedPackets);
            writer.writeU64(message.droppedPackets);
            writer.writeU64(message.duplicatedPackets);
            writer.writeU64(message.reorderedPackets);
            writer.writeU32(message.queuedPackets);
        }
    }, payload);
}

ParseError readPacketPayload(ByteReader& reader, PacketKind kind, PacketPayload* payload) {
    switch (kind) {
        case PacketKind::Hello: {
            HelloMessage message;
            std::uint8_t requestedTeam = 0;
            if (!reader.readU32(&message.sessionId) ||
                !reader.readU16(&message.requestedPeerId) ||
                !reader.readString(&message.playerName) ||
                !reader.readU8(&requestedTeam)) {
                return ParseError::BufferUnderflow;
            }
            if (!tryReadTeamId(requestedTeam, &message.requestedTeam)) {
                return ParseError::InvalidTeamId;
            }
            *payload = message;
            return ParseError::None;
        }
        case PacketKind::Welcome: {
            WelcomeMessage message;
            if (!reader.readU32(&message.sessionId) ||
                !reader.readU16(&message.assignedPeerId) ||
                !reader.readU16(&message.snapshotRateHz) ||
                !readTimingCadence(reader, &message.cadence) ||
                !reader.readI32(&message.levelSlot) ||
                !reader.readU32(&message.levelHash)) {
                return ParseError::BufferUnderflow;
            }
            {
                const ParseError participantError = readParticipantState(reader, &message.participantState);
                if (participantError != ParseError::None) {
                    return participantError;
                }
            }
            {
                const ParseError paneError = readPaneViewState(reader, &message.paneView);
                if (paneError != ParseError::None) {
                    return paneError;
                }
            }
            if (!readAuthoritativeTime(reader, &message.authoritativeTime)) {
                return ParseError::BufferUnderflow;
            }
            const ParseError metadataError = readHostedSessionMetadata(reader, &message.sessionMetadata);
            if (metadataError != ParseError::None) {
                return metadataError;
            }
            message.sessionMetadata.levelSlot = message.levelSlot;
            message.sessionMetadata.levelHash = message.levelHash;
            *payload = message;
            return ParseError::None;
        }
        case PacketKind::Disconnect: {
            DisconnectMessage message;
            if (!reader.readU16(&message.reasonCode) ||
                !reader.readString(&message.reason)) {
                return ParseError::BufferUnderflow;
            }
            *payload = message;
            return ParseError::None;
        }
        case PacketKind::TeamChangeRequest: {
            TeamChangeRequest message;
            std::uint8_t requestedTeam = 0;
            if (!reader.readU8(&requestedTeam)) {
                return ParseError::BufferUnderflow;
            }
            if (!tryReadTeamId(requestedTeam, &message.requestedTeam)) {
                return ParseError::InvalidTeamId;
            }
            *payload = message;
            return ParseError::None;
        }
        case PacketKind::RuntimeParamChangeRequest: {
            RuntimeParamChangeRequest message;
            const ParseError error = readRuntimeParamRecord(reader, &message);
            if (error != ParseError::None) {
                return error;
            }
            *payload = message;
            return ParseError::None;
        }
        case PacketKind::RuntimeParamSnapshot: {
            RuntimeParamSnapshot message;
            const ParseError error = readRuntimeParamRecord(reader, &message);
            if (error != ParseError::None) {
                return error;
            }
            *payload = message;
            return ParseError::None;
        }
        case PacketKind::RuntimeParamApplyResult: {
            RuntimeParamApplyResult message;
            const ParseError error = readRuntimeParamRecord(reader, &message);
            if (error != ParseError::None) {
                return error;
            }
            std::uint8_t stagedApplyBoundary = 0u;
            if (!reader.readBool(&message.applied) ||
                !reader.readU8(&stagedApplyBoundary) ||
                !reader.readString(&message.message)) {
                return ParseError::BufferUnderflow;
            }
            if (!tryReadStagedApplyBoundary(stagedApplyBoundary, &message.stagedApplyBoundary)) {
                return ParseError::InvalidStagedApplyBoundary;
            }
            *payload = message;
            return ParseError::None;
        }
        case PacketKind::SessionActionRequest: {
            SessionActionRequest message;
            std::uint8_t kindValue = 0u;
            if (!reader.readU8(&kindValue)) {
                return ParseError::BufferUnderflow;
            }
            if (!tryReadSessionActionKind(kindValue, &message.kind)) {
                return ParseError::InvalidSessionActionKind;
            }
            *payload = message;
            return ParseError::None;
        }
        case PacketKind::SessionActionResult: {
            SessionActionResult message;
            std::uint8_t kindValue = 0u;
            if (!reader.readU8(&kindValue) ||
                !reader.readBool(&message.applied) ||
                !reader.readI32(&message.actorId) ||
                !reader.readString(&message.message)) {
                return ParseError::BufferUnderflow;
            }
            if (!tryReadSessionActionKind(kindValue, &message.kind)) {
                return ParseError::InvalidSessionActionKind;
            }
            *payload = message;
            return ParseError::None;
        }
        case PacketKind::CommandBundle: {
            CommandBundle message;
            std::uint16_t count = 0;
            if (!reader.readU16(&count)) {
                return ParseError::BufferUnderflow;
            }
            message.commands.resize(count);
            for (auto& command : message.commands) {
                if (!readPlayerCommand(reader, &command)) {
                    return ParseError::BufferUnderflow;
                }
            }
            *payload = message;
            return ParseError::None;
        }
        case PacketKind::ControlCommandBundle: {
            ControlCommandBundle message;
            std::uint16_t count = 0;
            if (!reader.readU16(&count)) {
                return ParseError::BufferUnderflow;
            }
            message.commands.resize(count);
            for (auto& command : message.commands) {
                if (!readPlayerCommand(reader, &command)) {
                    return ParseError::BufferUnderflow;
                }
            }
            *payload = message;
            return ParseError::None;
        }
        case PacketKind::WorldSnapshot: {
            WorldSnapshot message;
            const ParseError error = readWorldSnapshotMessage(reader, &message);
            if (error != ParseError::None) {
                return error;
            }
            *payload = message;
            return ParseError::None;
        }
        case PacketKind::ControlWorldSnapshot: {
            ControlWorldSnapshot message;
            const ParseError error = readWorldSnapshotMessage(reader, &message.snapshot);
            if (error != ParseError::None) {
                return error;
            }
            *payload = message;
            return ParseError::None;
        }
        case PacketKind::ProxyControl: {
            ProxyControl message;
            if (!reader.readU16(&message.targetPeerId) ||
                !reader.readBool(&message.upstream) ||
                !reader.readF32(&message.config.baseDelayMs) ||
                !reader.readF32(&message.config.jitterMs) ||
                !reader.readF32(&message.config.lossPct) ||
                !reader.readF32(&message.config.duplicatePct) ||
                !reader.readF32(&message.config.reorderPct) ||
                !reader.readU32(&message.config.seed)) {
                return ParseError::BufferUnderflow;
            }
            *payload = message;
            return ParseError::None;
        }
        case PacketKind::ProxyStats: {
            ProxyStats message;
            if (!reader.readU64(&message.receivedPackets) ||
                !reader.readU64(&message.forwardedPackets) ||
                !reader.readU64(&message.droppedPackets) ||
                !reader.readU64(&message.duplicatedPackets) ||
                !reader.readU64(&message.reorderedPackets) ||
                !reader.readU32(&message.queuedPackets)) {
                return ParseError::BufferUnderflow;
            }
            *payload = message;
            return ParseError::None;
        }
    }

    return ParseError::InvalidKind;
}

}  // namespace

const char* toString(ParseError error) {
    switch (error) {
        case ParseError::None: return "none";
        case ParseError::BufferUnderflow: return "buffer_underflow";
        case ParseError::InvalidMagic: return "invalid_magic";
        case ParseError::UnsupportedVersion: return "unsupported_version";
        case ParseError::InvalidChannel: return "invalid_channel";
        case ParseError::InvalidKind: return "invalid_kind";
        case ParseError::InvalidEventKind: return "invalid_event_kind";
        case ParseError::InvalidStringLength: return "invalid_string_length";
        case ParseError::TrailingData: return "trailing_data";
        case ParseError::InvalidTeamId: return "invalid_team_id";
        case ParseError::InvalidShotEvaluationMode: return "invalid_shot_evaluation_mode";
        case ParseError::InvalidRuntimeParamScope: return "invalid_runtime_param_scope";
        case ParseError::InvalidStagedApplyBoundary: return "invalid_staged_apply_boundary";
        case ParseError::InvalidSessionPresence: return "invalid_session_presence";
        case ParseError::InvalidParticipationState: return "invalid_participation_state";
        case ParseError::InvalidControlBindingKind: return "invalid_control_binding_kind";
        case ParseError::InvalidPaneSlot: return "invalid_pane_slot";
        case ParseError::InvalidPaneViewMode: return "invalid_pane_view_mode";
        case ParseError::InvalidRuntimeReconciliationStrategy:
            return "invalid_runtime_reconciliation_strategy";
        case ParseError::InvalidSessionActionKind:
            return "invalid_session_action_kind";
    }
    return "unknown";
}

const char* toString(ShotEvaluationMode mode) {
    switch (mode) {
        case ShotEvaluationMode::SeenPosition: return "Seen Position";
        case ShotEvaluationMode::LivePosition: return "Live Position";
    }
    return "Unknown";
}

bool tryParseShotEvaluationMode(std::uint8_t rawMode, ShotEvaluationMode* modeOut) {
    return tryReadShotEvaluationMode(rawMode, modeOut);
}

bool tryParseShotEvaluationModeValue(float rawValue, ShotEvaluationMode* modeOut) {
    if (modeOut == nullptr || !std::isfinite(rawValue)) {
        return false;
    }

    const float rounded = std::round(rawValue);
    if (std::fabs(rawValue - rounded) > 0.0001f ||
        rounded < 0.0f ||
        rounded > static_cast<float>(std::numeric_limits<std::uint8_t>::max())) {
        return false;
    }

    return tryReadShotEvaluationMode(static_cast<std::uint8_t>(rounded), modeOut);
}

bool tryParseSessionTickRateHzValue(float rawValue, std::uint16_t* tickRateHzOut) {
    if (tickRateHzOut == nullptr || !std::isfinite(rawValue)) {
        return false;
    }

    const float rounded = std::round(rawValue);
    if (std::fabs(rawValue - rounded) > 0.0001f ||
        rounded < static_cast<float>(kMinSessionTickRateHz) ||
        rounded > static_cast<float>(kMaxSessionTickRateHz)) {
        return false;
    }

    *tickRateHzOut = static_cast<std::uint16_t>(rounded);
    return true;
}

bool tryParseSessionSnapshotRateHzValue(float rawValue, std::uint16_t* snapshotRateHzOut) {
    return tryParseSessionTickRateHzValue(rawValue, snapshotRateHzOut);
}

PacketKind packetKindForPayload(const PacketPayload& payload) {
    return std::visit([](const auto& message) -> PacketKind {
        using T = std::decay_t<decltype(message)>;
        if constexpr (std::is_same_v<T, HelloMessage>) {
            return PacketKind::Hello;
        } else if constexpr (std::is_same_v<T, WelcomeMessage>) {
            return PacketKind::Welcome;
        } else if constexpr (std::is_same_v<T, DisconnectMessage>) {
            return PacketKind::Disconnect;
        } else if constexpr (std::is_same_v<T, TeamChangeRequest>) {
            return PacketKind::TeamChangeRequest;
        } else if constexpr (std::is_same_v<T, RuntimeParamChangeRequest>) {
            return PacketKind::RuntimeParamChangeRequest;
        } else if constexpr (std::is_same_v<T, RuntimeParamSnapshot>) {
            return PacketKind::RuntimeParamSnapshot;
        } else if constexpr (std::is_same_v<T, RuntimeParamApplyResult>) {
            return PacketKind::RuntimeParamApplyResult;
        } else if constexpr (std::is_same_v<T, SessionActionRequest>) {
            return PacketKind::SessionActionRequest;
        } else if constexpr (std::is_same_v<T, SessionActionResult>) {
            return PacketKind::SessionActionResult;
        } else if constexpr (std::is_same_v<T, CommandBundle>) {
            return PacketKind::CommandBundle;
        } else if constexpr (std::is_same_v<T, ControlCommandBundle>) {
            return PacketKind::ControlCommandBundle;
        } else if constexpr (std::is_same_v<T, WorldSnapshot>) {
            return PacketKind::WorldSnapshot;
        } else if constexpr (std::is_same_v<T, ControlWorldSnapshot>) {
            return PacketKind::ControlWorldSnapshot;
        } else if constexpr (std::is_same_v<T, ProxyControl>) {
            return PacketKind::ProxyControl;
        } else if constexpr (std::is_same_v<T, ProxyStats>) {
            return PacketKind::ProxyStats;
        } else {
            static_assert(DependentFalse<T>::value, "unsupported packet payload type");
        }
    }, payload);
}

ByteBuffer serializeHeader(const PacketHeader& header) {
    ByteWriter writer;
    writeHeader(writer, header);
    return std::move(writer).take();
}

ParseError deserializeHeader(const ByteBuffer& bytes, PacketHeader* headerOut, std::size_t* bytesConsumed) {
    if (headerOut == nullptr) {
        return ParseError::BufferUnderflow;
    }
    ByteReader reader(bytes);
    ParseError error = readHeader(reader, headerOut);
    if (bytesConsumed != nullptr && error == ParseError::None) {
        *bytesConsumed = reader.offset();
    }
    return error;
}

ByteBuffer serializePacket(const Packet& packet) {
    PacketHeader header = packet.header;
    header.magic = kProtocolMagic;
    header.version = kProtocolVersion;
    header.kind = packetKindForPayload(packet.payload);

    ByteWriter writer;
    writeHeader(writer, header);
    writePacketPayload(writer, packet.payload);
    return std::move(writer).take();
}

ParseResult deserializePacket(const ByteBuffer& bytes) {
    ParseResult result;
    ByteReader reader(bytes);

    ParseError headerError = readHeader(reader, &result.packet.header);
    if (headerError != ParseError::None) {
        result.error = headerError;
        return result;
    }

    ParseError payloadError = readPacketPayload(reader, result.packet.header.kind, &result.packet.payload);
    if (payloadError != ParseError::None) {
        result.error = payloadError;
        return result;
    }

    if (!reader.atEnd()) {
        result.error = ParseError::TrailingData;
        return result;
    }

    result.ok = true;
    result.error = ParseError::None;
    return result;
}

bool isNewerSequence(std::uint32_t candidateSeq, std::uint32_t referenceSeq) {
    return candidateSeq != referenceSeq &&
           static_cast<std::uint32_t>(candidateSeq - referenceSeq) < 0x80000000u;
}

bool shouldAcceptSequence(std::uint32_t newestAcceptedSeq, std::uint32_t candidateSeq) {
    return isNewerSequence(candidateSeq, newestAcceptedSeq);
}

bool shouldRetransmit(const ReliableControlState& state, std::uint64_t nowMs) {
    return state.awaitingAck &&
           nowMs >= state.lastSendTimeMs &&
           (nowMs - state.lastSendTimeMs) >= state.retransmitTimeoutMs;
}

bool operator==(const PacketHeader& lhs, const PacketHeader& rhs) {
    return lhs.magic == rhs.magic &&
           lhs.version == rhs.version &&
           lhs.peerId == rhs.peerId &&
           lhs.channel == rhs.channel &&
           lhs.seq == rhs.seq &&
           lhs.ack == rhs.ack &&
           lhs.ackBits == rhs.ackBits &&
           lhs.kind == rhs.kind;
}

bool operator==(const HelloMessage& lhs, const HelloMessage& rhs) {
    return lhs.sessionId == rhs.sessionId &&
           lhs.requestedPeerId == rhs.requestedPeerId &&
           lhs.playerName == rhs.playerName &&
           lhs.requestedTeam == rhs.requestedTeam;
}

bool operator==(const HostedSessionMetadata& lhs, const HostedSessionMetadata& rhs) {
    return lhs.sessionLabel == rhs.sessionLabel &&
           lhs.hostPlayerName == rhs.hostPlayerName &&
           lhs.hostPeerId == rhs.hostPeerId &&
           lhs.levelSlot == rhs.levelSlot &&
           lhs.levelHash == rhs.levelHash &&
           lhs.publicJoinPort == rhs.publicJoinPort &&
           lhs.maxHumanPlayers == rhs.maxHumanPlayers &&
           lhs.shotEvaluationMode == rhs.shotEvaluationMode &&
           lhs.botsFrozen == rhs.botsFrozen &&
           lhs.botsCanShoot == rhs.botsCanShoot &&
           lhs.studyEventLoggingEnabled == rhs.studyEventLoggingEnabled &&
           lhs.studyEventRunId == rhs.studyEventRunId;
}

bool operator==(const WelcomeMessage& lhs, const WelcomeMessage& rhs) {
    return lhs.sessionId == rhs.sessionId &&
           lhs.assignedPeerId == rhs.assignedPeerId &&
           lhs.snapshotRateHz == rhs.snapshotRateHz &&
           timingCadenceEqual(lhs.cadence, rhs.cadence) &&
           lhs.levelSlot == rhs.levelSlot &&
           lhs.levelHash == rhs.levelHash &&
           participantStateEqual(lhs.participantState, rhs.participantState) &&
           paneViewStateEqual(lhs.paneView, rhs.paneView) &&
           authoritativeTimeEqual(lhs.authoritativeTime, rhs.authoritativeTime) &&
           lhs.sessionMetadata == rhs.sessionMetadata;
}

bool operator==(const DisconnectMessage& lhs, const DisconnectMessage& rhs) {
    return lhs.reasonCode == rhs.reasonCode &&
           lhs.reason == rhs.reason;
}

bool operator==(const TeamChangeRequest& lhs, const TeamChangeRequest& rhs) {
    return lhs.requestedTeam == rhs.requestedTeam;
}

bool operator==(const RuntimeParamChangeRequest& lhs, const RuntimeParamChangeRequest& rhs) {
    return lhs.scope == rhs.scope &&
           lhs.targetId == rhs.targetId &&
           lhs.key == rhs.key &&
           lhs.value == rhs.value;
}

bool operator==(const RuntimeParamSnapshot& lhs, const RuntimeParamSnapshot& rhs) {
    return lhs.scope == rhs.scope &&
           lhs.targetId == rhs.targetId &&
           lhs.key == rhs.key &&
           lhs.value == rhs.value;
}

bool operator==(const RuntimeParamApplyResult& lhs, const RuntimeParamApplyResult& rhs) {
    return lhs.scope == rhs.scope &&
           lhs.targetId == rhs.targetId &&
           lhs.key == rhs.key &&
           lhs.value == rhs.value &&
           lhs.applied == rhs.applied &&
           lhs.stagedApplyBoundary == rhs.stagedApplyBoundary &&
           lhs.message == rhs.message;
}

bool operator==(const SessionActionRequest& lhs, const SessionActionRequest& rhs) {
    return lhs.kind == rhs.kind;
}

bool operator==(const SessionActionResult& lhs, const SessionActionResult& rhs) {
    return lhs.kind == rhs.kind &&
           lhs.applied == rhs.applied &&
           lhs.actorId == rhs.actorId &&
           lhs.message == rhs.message;
}

bool operator==(const CommandBundle& lhs, const CommandBundle& rhs) {
    if (lhs.commands.size() != rhs.commands.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.commands.size(); ++index) {
        if (!playerCommandEqual(lhs.commands[index], rhs.commands[index])) {
            return false;
        }
    }
    return true;
}

bool operator==(const ControlCommandBundle& lhs, const ControlCommandBundle& rhs) {
    if (lhs.commands.size() != rhs.commands.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.commands.size(); ++index) {
        if (!playerCommandEqual(lhs.commands[index], rhs.commands[index])) {
            return false;
        }
    }
    return true;
}

bool operator==(const GameplayEvent& lhs, const GameplayEvent& rhs) {
    return gameplayEventEqual(lhs, rhs);
}

bool operator==(const ReplicationSnapshot& lhs, const ReplicationSnapshot& rhs) {
    if (lhs.serverTick != rhs.serverTick ||
        lhs.serverTimeUs != rhs.serverTimeUs ||
        lhs.ackedInputSeq != rhs.ackedInputSeq ||
        !timingCadenceEqual(lhs.cadence, rhs.cadence) ||
        !authoritativeTimeEqual(lhs.authoritativeTime, rhs.authoritativeTime) ||
        !participantStateEqual(lhs.localParticipantState, rhs.localParticipantState) ||
        !paneViewStateEqual(lhs.localPaneView, rhs.localPaneView) ||
        !playerStateEqual(lhs.localPlayerState, rhs.localPlayerState) ||
        lhs.remotePlayers.size() != rhs.remotePlayers.size() ||
        lhs.controlRemotePlayers.size() != rhs.controlRemotePlayers.size() ||
        lhs.remoteEnemies.size() != rhs.remoteEnemies.size()) {
        return false;
    }

    for (std::size_t index = 0; index < lhs.remotePlayers.size(); ++index) {
        if (!playerStateEqual(lhs.remotePlayers[index], rhs.remotePlayers[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < lhs.controlRemotePlayers.size(); ++index) {
        if (!playerStateEqual(lhs.controlRemotePlayers[index], rhs.controlRemotePlayers[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < lhs.remoteEnemies.size(); ++index) {
        if (!remoteActorEqual(lhs.remoteEnemies[index], rhs.remoteEnemies[index])) {
            return false;
        }
    }
    return true;
}

bool operator==(const SessionSummary& lhs, const SessionSummary& rhs) {
    if (!(lhs.sessionMetadata == rhs.sessionMetadata) ||
        lhs.roster.size() != rhs.roster.size() ||
        !teamScoresEqual(lhs.teamScores, rhs.teamScores)) {
        return false;
    }

    for (std::size_t index = 0; index < lhs.roster.size(); ++index) {
        if (!rosterEntryEqual(lhs.roster[index], rhs.roster[index])) {
            return false;
        }
    }
    return true;
}

bool operator==(const GameplayEventBatch& lhs, const GameplayEventBatch& rhs) {
    if (lhs.events.size() != rhs.events.size()) {
        return false;
    }

    for (std::size_t index = 0; index < lhs.events.size(); ++index) {
        if (!gameplayEventEqual(lhs.events[index], rhs.events[index])) {
            return false;
        }
    }
    return true;
}

bool operator==(const WorldSnapshot& lhs, const WorldSnapshot& rhs) {
    return lhs.replication() == rhs.replication() &&
           lhs.summary() == rhs.summary() &&
           lhs.gameplayEvents() == rhs.gameplayEvents();
}

bool operator==(const ControlWorldSnapshot& lhs, const ControlWorldSnapshot& rhs) {
    return lhs.snapshot == rhs.snapshot;
}

bool operator==(const ProxyLinkConfig& lhs, const ProxyLinkConfig& rhs) {
    return lhs.baseDelayMs == rhs.baseDelayMs &&
           lhs.jitterMs == rhs.jitterMs &&
           lhs.lossPct == rhs.lossPct &&
           lhs.duplicatePct == rhs.duplicatePct &&
           lhs.reorderPct == rhs.reorderPct &&
           lhs.seed == rhs.seed;
}

bool operator==(const ProxyControl& lhs, const ProxyControl& rhs) {
    return lhs.targetPeerId == rhs.targetPeerId &&
           lhs.upstream == rhs.upstream &&
           lhs.config == rhs.config;
}

bool operator==(const ProxyStats& lhs, const ProxyStats& rhs) {
    return lhs.receivedPackets == rhs.receivedPackets &&
           lhs.forwardedPackets == rhs.forwardedPackets &&
           lhs.droppedPackets == rhs.droppedPackets &&
           lhs.duplicatedPackets == rhs.duplicatedPackets &&
           lhs.reorderedPackets == rhs.reorderedPackets &&
           lhs.queuedPackets == rhs.queuedPackets;
}

bool operator==(const Packet& lhs, const Packet& rhs) {
    return lhs.header == rhs.header && lhs.payload == rhs.payload;
}

}  // namespace net
