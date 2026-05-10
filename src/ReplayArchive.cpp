#include "app/ReplayArchive.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>

#include "app/UserDataPaths.hpp"
#include "net/Codec.hpp"

namespace app {
namespace {

constexpr std::uint32_t kReplayMagic = 0x52504C4Eu;  // "NLPR" in little-endian storage.
constexpr std::uint16_t kReplayFormatVersion = 2u;
constexpr std::uint16_t kMinimumReplayFormatVersion = 1u;
constexpr std::uint32_t kMaxReplayFrames = 240'000u;
constexpr std::uint32_t kMaxFrameItems = 2'048u;
constexpr std::uint32_t kMaxTextItems = 256u;
constexpr std::uint32_t kMaxScoreboardSections = 8u;
constexpr std::uint32_t kMaxScoreboardEntries = 128u;

using net::ByteBuffer;
using net::codec::ByteReader;
using net::codec::ByteWriter;

void setError(std::string* errorOut, const std::string& error) {
    if (errorOut != nullptr) {
        *errorOut = error;
    }
}

std::uint64_t currentUnixSeconds() {
    return static_cast<std::uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
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

std::string sanitizeFilenamePart(std::string value) {
    if (value.empty()) {
        return "replay";
    }

    for (char& ch : value) {
        const bool allowed =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' ||
            ch == '_';
        if (!allowed) {
            ch = '_';
        }
    }
    return value;
}

std::string timestampForFilename(std::uint64_t unixSeconds) {
    const std::time_t timestamp = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &timestamp);
#else
    if (const std::tm* local = std::localtime(&timestamp); local != nullptr) {
        tm = *local;
    }
#endif
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return stream.str();
}

std::filesystem::path uniqueReplayPath(const std::filesystem::path& directory,
                                       const client::ReplayRecordingMetadata& metadata) {
    const std::string base =
        "replay_" + timestampForFilename(metadata.createdUnixSeconds) + "_" +
        sanitizeFilenamePart(metadata.title.empty() ? "session" : metadata.title);
    std::filesystem::path candidate = directory / (base + ReplayArchive::kReplayExtension);
    if (!std::filesystem::exists(candidate)) {
        return candidate;
    }

    for (int suffix = 2; suffix < 1000; ++suffix) {
        candidate = directory / (base + "_" + std::to_string(suffix) +
                                 ReplayArchive::kReplayExtension);
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return directory / (base + "_latest" + ReplayArchive::kReplayExtension);
}

void writeColor(ByteWriter& writer, Color color) {
    writer.writeU8(color.r);
    writer.writeU8(color.g);
    writer.writeU8(color.b);
    writer.writeU8(color.a);
}

bool readColor(ByteReader& reader, Color* colorOut) {
    if (colorOut == nullptr) {
        return false;
    }
    std::uint8_t r = 0u;
    std::uint8_t g = 0u;
    std::uint8_t b = 0u;
    std::uint8_t a = 0u;
    if (!reader.readU8(&r) ||
        !reader.readU8(&g) ||
        !reader.readU8(&b) ||
        !reader.readU8(&a)) {
        return false;
    }
    *colorOut = Color{r, g, b, a};
    return true;
}

void writeVector3(ByteWriter& writer, Vector3 value) {
    writer.writeF32(value.x);
    writer.writeF32(value.y);
    writer.writeF32(value.z);
}

bool readVector3(ByteReader& reader, Vector3* valueOut) {
    if (valueOut == nullptr) {
        return false;
    }
    return reader.readF32(&valueOut->x) &&
           reader.readF32(&valueOut->y) &&
           reader.readF32(&valueOut->z);
}

bool readInt(ByteReader& reader, int* valueOut) {
    std::int32_t value = 0;
    if (valueOut == nullptr || !reader.readI32(&value)) {
        return false;
    }
    *valueOut = static_cast<int>(value);
    return true;
}

void writeCamera(ByteWriter& writer, const Camera3D& camera) {
    writeVector3(writer, camera.position);
    writeVector3(writer, camera.target);
    writeVector3(writer, camera.up);
    writer.writeF32(camera.fovy);
    writer.writeI32(camera.projection);
}

bool readCamera(ByteReader& reader, Camera3D* cameraOut) {
    if (cameraOut == nullptr) {
        return false;
    }
    int projection = CAMERA_PERSPECTIVE;
    if (!readVector3(reader, &cameraOut->position) ||
        !readVector3(reader, &cameraOut->target) ||
        !readVector3(reader, &cameraOut->up) ||
        !reader.readF32(&cameraOut->fovy) ||
        !readInt(reader, &projection)) {
        return false;
    }
    cameraOut->projection = projection;
    return true;
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

void writePaneSlot(ByteWriter& writer, sim::PaneSlot slot) {
    writer.writeU8(static_cast<std::uint8_t>(slot));
}

bool readPaneSlot(ByteReader& reader, sim::PaneSlot* slotOut) {
    std::uint8_t raw = 0u;
    if (slotOut == nullptr || !reader.readU8(&raw) ||
        raw > static_cast<std::uint8_t>(sim::PaneSlot::Right)) {
        return false;
    }
    *slotOut = static_cast<sim::PaneSlot>(raw);
    return true;
}

void writePaneMode(ByteWriter& writer, sim::PaneViewMode mode) {
    writer.writeU8(static_cast<std::uint8_t>(mode));
}

bool readPaneMode(ByteReader& reader, sim::PaneViewMode* modeOut) {
    std::uint8_t raw = 0u;
    if (modeOut == nullptr || !reader.readU8(&raw) ||
        raw > static_cast<std::uint8_t>(sim::PaneViewMode::ReplayCamera)) {
        return false;
    }
    *modeOut = static_cast<sim::PaneViewMode>(raw);
    return true;
}

bool readCount(ByteReader& reader, std::uint32_t maxCount, std::uint32_t* countOut) {
    std::uint32_t count = 0u;
    if (countOut == nullptr || !reader.readU32(&count) || count > maxCount) {
        return false;
    }
    *countOut = count;
    return true;
}

void writeLaserBeam(ByteWriter& writer, const LaserBeam3D& beam) {
    writeVector3(writer, beam.getStart());
    writeVector3(writer, beam.getEnd());
    writer.writeF32(beam.getLifetime());
    writer.writeF32(beam.getMaxLifetime());
    writeColor(writer, beam.getColor());
    writer.writeF32(beam.getThickness());
    writer.writeBool(beam.isGhostBeam());
    writer.writeBool(beam.canDamageEnemy());
}

bool readLaserBeam(ByteReader& reader, LaserBeam3D* beamOut) {
    Vector3 start{};
    Vector3 end{};
    float lifetime = 0.0f;
    float maxLifetime = 0.0f;
    Color color{};
    float thickness = 0.0f;
    bool ghost = false;
    bool canDamage = false;
    if (beamOut == nullptr ||
        !readVector3(reader, &start) ||
        !readVector3(reader, &end) ||
        !reader.readF32(&lifetime) ||
        !reader.readF32(&maxLifetime) ||
        !readColor(reader, &color) ||
        !reader.readF32(&thickness) ||
        !reader.readBool(&ghost) ||
        !reader.readBool(&canDamage)) {
        return false;
    }

    LaserBeam3D beam(start, end, color, maxLifetime, thickness, ghost, canDamage);
    beam.update(lifetime);
    *beamOut = beam;
    return true;
}

void writeRemoteEnemy(ByteWriter& writer, const client::RemoteEnemyRenderItem& item) {
    writer.writeI32(item.entityId);
    writeVector3(writer, item.displayPosition);
    writer.writeF32(item.yawRadians);
    writer.writeF32(item.healthPercent);
    writer.writeBool(item.alive);
    writeColor(writer, item.tint);
}

bool readRemoteEnemy(ByteReader& reader, client::RemoteEnemyRenderItem* itemOut) {
    if (itemOut == nullptr) {
        return false;
    }
    return readInt(reader, &itemOut->entityId) &&
           readVector3(reader, &itemOut->displayPosition) &&
           reader.readF32(&itemOut->yawRadians) &&
           reader.readF32(&itemOut->healthPercent) &&
           reader.readBool(&itemOut->alive) &&
           readColor(reader, &itemOut->tint);
}

void writeRemotePlayer(ByteWriter& writer, const client::RemotePlayerRenderItem& item) {
    writer.writeI32(item.actorId);
    writeVector3(writer, item.eyePosition);
    writeVector3(writer, item.rootPosition);
    writer.writeF32(item.yawRadians);
    writer.writeF32(item.pitchRadians);
    writer.writeF32(item.healthPercent);
    writer.writeBool(item.alive);
    writeTeam(writer, item.team);
    writeColor(writer, item.tint);
    writer.writeBool(item.ghost);
}

bool readRemotePlayer(ByteReader& reader, client::RemotePlayerRenderItem* itemOut) {
    if (itemOut == nullptr) {
        return false;
    }
    return readInt(reader, &itemOut->actorId) &&
           readVector3(reader, &itemOut->eyePosition) &&
           readVector3(reader, &itemOut->rootPosition) &&
           reader.readF32(&itemOut->yawRadians) &&
           reader.readF32(&itemOut->pitchRadians) &&
           reader.readF32(&itemOut->healthPercent) &&
           reader.readBool(&itemOut->alive) &&
           readTeam(reader, &itemOut->team) &&
           readColor(reader, &itemOut->tint) &&
           reader.readBool(&itemOut->ghost);
}

void writeStringVector(ByteWriter& writer, const std::vector<std::string>& values) {
    writer.writeU32(static_cast<std::uint32_t>(values.size()));
    for (const std::string& value : values) {
        writer.writeString(value);
    }
}

bool readStringVector(ByteReader& reader, std::vector<std::string>* valuesOut) {
    std::uint32_t count = 0u;
    if (valuesOut == nullptr || !readCount(reader, kMaxTextItems, &count)) {
        return false;
    }
    valuesOut->clear();
    valuesOut->reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index) {
        std::string value;
        if (!reader.readString(&value)) {
            return false;
        }
        valuesOut->push_back(std::move(value));
    }
    return true;
}

void writeCompactScore(ByteWriter& writer, const client::CompactScoreView& view) {
    writer.writeBool(view.available);
    writer.writeU16(view.attackerScore);
    writer.writeU16(view.defenderScore);
    writer.writeString(view.localIdentity);
    writeTeam(writer, view.localTeam);
    writer.writeI32(view.localKills);
    writer.writeI32(view.localDeaths);
    writer.writeBool(view.localAlive);
}

bool readCompactScore(ByteReader& reader, client::CompactScoreView* viewOut) {
    if (viewOut == nullptr) {
        return false;
    }
    return reader.readBool(&viewOut->available) &&
           reader.readU16(&viewOut->attackerScore) &&
           reader.readU16(&viewOut->defenderScore) &&
           reader.readString(&viewOut->localIdentity) &&
           readTeam(reader, &viewOut->localTeam) &&
           reader.readI32(&viewOut->localKills) &&
           reader.readI32(&viewOut->localDeaths) &&
           reader.readBool(&viewOut->localAlive);
}

void writeKillFeedEntry(ByteWriter& writer, const client::KillFeedEntryView& entry) {
    writer.writeString(entry.attackerLabel);
    writer.writeString(entry.victimLabel);
    writeTeam(writer, entry.attackerTeam);
    writeTeam(writer, entry.victimTeam);
    writer.writeBool(entry.attackerIsLocalPlayer);
    writer.writeBool(entry.victimIsLocalPlayer);
}

bool readKillFeedEntry(ByteReader& reader, client::KillFeedEntryView* entryOut) {
    if (entryOut == nullptr) {
        return false;
    }
    return reader.readString(&entryOut->attackerLabel) &&
           reader.readString(&entryOut->victimLabel) &&
           readTeam(reader, &entryOut->attackerTeam) &&
           readTeam(reader, &entryOut->victimTeam) &&
           reader.readBool(&entryOut->attackerIsLocalPlayer) &&
           reader.readBool(&entryOut->victimIsLocalPlayer);
}

void writeScoreboardEntry(ByteWriter& writer, const client::ScoreboardEntryView& entry) {
    writer.writeString(entry.identity);
    writer.writeString(entry.rowLabel);
    writeTeam(writer, entry.team);
    writer.writeI32(entry.actorId);
    writer.writeI32(entry.kills);
    writer.writeI32(entry.deaths);
    writer.writeU16(entry.pingMs);
    writer.writeBool(entry.alive);
    writer.writeBool(entry.isBot);
    writer.writeBool(entry.isLocalPlayer);
}

bool readScoreboardEntry(ByteReader& reader, client::ScoreboardEntryView* entryOut) {
    if (entryOut == nullptr) {
        return false;
    }
    return reader.readString(&entryOut->identity) &&
           reader.readString(&entryOut->rowLabel) &&
           readTeam(reader, &entryOut->team) &&
           readInt(reader, &entryOut->actorId) &&
           readInt(reader, &entryOut->kills) &&
           readInt(reader, &entryOut->deaths) &&
           reader.readU16(&entryOut->pingMs) &&
           reader.readBool(&entryOut->alive) &&
           reader.readBool(&entryOut->isBot) &&
           reader.readBool(&entryOut->isLocalPlayer);
}

void writeScoreboardSection(ByteWriter& writer, const client::ScoreboardSectionView& section) {
    writeTeam(writer, section.team);
    writer.writeU16(section.score);
    writer.writeU32(static_cast<std::uint32_t>(section.entries.size()));
    for (const client::ScoreboardEntryView& entry : section.entries) {
        writeScoreboardEntry(writer, entry);
    }
}

bool readScoreboardSection(ByteReader& reader, client::ScoreboardSectionView* sectionOut) {
    std::uint32_t count = 0u;
    if (sectionOut == nullptr ||
        !readTeam(reader, &sectionOut->team) ||
        !reader.readU16(&sectionOut->score) ||
        !readCount(reader, kMaxScoreboardEntries, &count)) {
        return false;
    }
    sectionOut->entries.clear();
    sectionOut->entries.reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index) {
        client::ScoreboardEntryView entry;
        if (!readScoreboardEntry(reader, &entry)) {
            return false;
        }
        sectionOut->entries.push_back(std::move(entry));
    }
    return true;
}

void writeReplayStatus(ByteWriter& writer, const client::ReplayStatusView& replay) {
    writer.writeBool(replay.recordingActive);
    writer.writeBool(replay.playbackActive);
    writer.writeBool(replay.overlayVisible);
    writer.writeString(replay.statusLine);

    writer.writeBool(replay.rule.available);
    writer.writeString(replay.rule.label);
    writer.writeString(replay.rule.explanation);

    writer.writeBool(replay.paneBinding.available);
    writePaneSlot(writer, replay.paneBinding.slot);
    writePaneMode(writer, replay.paneBinding.mode);
    writer.writeString(replay.paneBinding.slotLabel);
    writer.writeString(replay.paneBinding.bindingLabel);
    writer.writeString(replay.paneBinding.modeLabel);

    writer.writeBool(replay.spectator.available);
    writePaneMode(writer, replay.spectator.mode);
    writer.writeString(replay.spectator.modeLabel);
    writer.writeString(replay.spectator.followTargetLabel);
    writer.writeBool(replay.spectator.sessionSpectator);
    writer.writeBool(replay.spectator.canReturnToCharacter);

    writer.writeBool(replay.checkpoint.detachedCameraActive);
    writer.writeI32(replay.checkpoint.activeIndex);
    writer.writeU32(static_cast<std::uint32_t>(replay.checkpoint.checkpointCount));
    writer.writeF32(replay.checkpoint.transitionToNextSeconds);
    writer.writeBool(replay.checkpoint.transitionEditable);
}

bool readReplayStatus(ByteReader& reader, client::ReplayStatusView* replayOut) {
    std::uint32_t checkpointCount = 0u;
    if (replayOut == nullptr ||
        !reader.readBool(&replayOut->recordingActive) ||
        !reader.readBool(&replayOut->playbackActive) ||
        !reader.readBool(&replayOut->overlayVisible) ||
        !reader.readString(&replayOut->statusLine) ||
        !reader.readBool(&replayOut->rule.available) ||
        !reader.readString(&replayOut->rule.label) ||
        !reader.readString(&replayOut->rule.explanation) ||
        !reader.readBool(&replayOut->paneBinding.available) ||
        !readPaneSlot(reader, &replayOut->paneBinding.slot) ||
        !readPaneMode(reader, &replayOut->paneBinding.mode) ||
        !reader.readString(&replayOut->paneBinding.slotLabel) ||
        !reader.readString(&replayOut->paneBinding.bindingLabel) ||
        !reader.readString(&replayOut->paneBinding.modeLabel) ||
        !reader.readBool(&replayOut->spectator.available) ||
        !readPaneMode(reader, &replayOut->spectator.mode) ||
        !reader.readString(&replayOut->spectator.modeLabel) ||
        !reader.readString(&replayOut->spectator.followTargetLabel) ||
        !reader.readBool(&replayOut->spectator.sessionSpectator) ||
        !reader.readBool(&replayOut->spectator.canReturnToCharacter) ||
        !reader.readBool(&replayOut->checkpoint.detachedCameraActive) ||
        !readInt(reader, &replayOut->checkpoint.activeIndex) ||
        !readCount(reader, kMaxFrameItems, &checkpointCount) ||
        !reader.readF32(&replayOut->checkpoint.transitionToNextSeconds) ||
        !reader.readBool(&replayOut->checkpoint.transitionEditable)) {
        return false;
    }
    replayOut->checkpoint.checkpointCount = checkpointCount;
    return true;
}

void writeRenderFrame(ByteWriter& writer, const client::RenderFrame& frame) {
    writer.writeBool(frame.hasSnapshot);
    writer.writeBool(frame.arena.visible);
    writer.writeF32(frame.arena.dimFactor);
    writeCamera(writer, frame.camera);

    writer.writeU32(static_cast<std::uint32_t>(frame.combatTraces.size()));
    for (const LaserBeam3D& trace : frame.combatTraces) {
        writeLaserBeam(writer, trace);
    }

    writer.writeU32(static_cast<std::uint32_t>(frame.remoteEnemies.size()));
    for (const client::RemoteEnemyRenderItem& enemy : frame.remoteEnemies) {
        writeRemoteEnemy(writer, enemy);
    }

    writer.writeU32(static_cast<std::uint32_t>(frame.remotePlayers.size()));
    for (const client::RemotePlayerRenderItem& player : frame.remotePlayers) {
        writeRemotePlayer(writer, player);
    }

    writer.writeU32(static_cast<std::uint32_t>(frame.remotePlayerGhosts.size()));
    for (const client::RemotePlayerRenderItem& player : frame.remotePlayerGhosts) {
        writeRemotePlayer(writer, player);
    }

    writer.writeBool(frame.waiting.visible);
    writer.writeString(frame.waiting.title);
    writer.writeString(frame.waiting.stateLine);
    writer.writeString(frame.waiting.statusMessage);
    writer.writeString(frame.waiting.hostedSessionLine);
    writer.writeString(frame.waiting.waitingText);
    writer.writeString(frame.waiting.joinHint);

    writeStringVector(writer, frame.hud.lines);

    writer.writeBool(frame.compactScore.visible);
    writeCompactScore(writer, frame.compactScore.score);

    writer.writeBool(frame.killFeed.visible);
    writer.writeU32(static_cast<std::uint32_t>(frame.killFeed.entries.size()));
    for (const client::KillFeedEntryView& entry : frame.killFeed.entries) {
        writeKillFeedEntry(writer, entry);
    }

    writer.writeBool(frame.scoreboard.visible);
    writer.writeU16(frame.scoreboard.attackerScore);
    writer.writeU16(frame.scoreboard.defenderScore);
    writer.writeU32(static_cast<std::uint32_t>(frame.scoreboard.sections.size()));
    for (const client::ScoreboardSectionView& section : frame.scoreboard.sections) {
        writeScoreboardSection(writer, section);
    }

    writer.writeBool(frame.teamMenu.visible);
    writer.writeString(frame.teamMenu.currentTeamLabel);
    writer.writeString(frame.teamMenu.selectedTeamLabel);

    writeReplayStatus(writer, frame.replay);
    writer.writeBool(frame.localNetworkPanelVisible);
    writer.writeBool(frame.diagnosticsPanelVisible);
}

bool readRenderFrame(ByteReader& reader, client::RenderFrame* frameOut) {
    if (frameOut == nullptr ||
        !reader.readBool(&frameOut->hasSnapshot) ||
        !reader.readBool(&frameOut->arena.visible) ||
        !reader.readF32(&frameOut->arena.dimFactor) ||
        !readCamera(reader, &frameOut->camera)) {
        return false;
    }

    std::uint32_t count = 0u;
    if (!readCount(reader, kMaxFrameItems, &count)) {
        return false;
    }
    frameOut->combatTraces.clear();
    frameOut->combatTraces.reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index) {
        LaserBeam3D beam(Vector3{}, Vector3{});
        if (!readLaserBeam(reader, &beam)) {
            return false;
        }
        frameOut->combatTraces.push_back(beam);
    }

    if (!readCount(reader, kMaxFrameItems, &count)) {
        return false;
    }
    frameOut->remoteEnemies.clear();
    frameOut->remoteEnemies.reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index) {
        client::RemoteEnemyRenderItem item;
        if (!readRemoteEnemy(reader, &item)) {
            return false;
        }
        frameOut->remoteEnemies.push_back(item);
    }

    if (!readCount(reader, kMaxFrameItems, &count)) {
        return false;
    }
    frameOut->remotePlayers.clear();
    frameOut->remotePlayers.reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index) {
        client::RemotePlayerRenderItem item;
        if (!readRemotePlayer(reader, &item)) {
            return false;
        }
        frameOut->remotePlayers.push_back(item);
    }

    if (!readCount(reader, kMaxFrameItems, &count)) {
        return false;
    }
    frameOut->remotePlayerGhosts.clear();
    frameOut->remotePlayerGhosts.reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index) {
        client::RemotePlayerRenderItem item;
        if (!readRemotePlayer(reader, &item)) {
            return false;
        }
        frameOut->remotePlayerGhosts.push_back(item);
    }

    if (!reader.readBool(&frameOut->waiting.visible) ||
        !reader.readString(&frameOut->waiting.title) ||
        !reader.readString(&frameOut->waiting.stateLine) ||
        !reader.readString(&frameOut->waiting.statusMessage) ||
        !reader.readString(&frameOut->waiting.hostedSessionLine) ||
        !reader.readString(&frameOut->waiting.waitingText) ||
        !reader.readString(&frameOut->waiting.joinHint) ||
        !readStringVector(reader, &frameOut->hud.lines) ||
        !reader.readBool(&frameOut->compactScore.visible) ||
        !readCompactScore(reader, &frameOut->compactScore.score) ||
        !reader.readBool(&frameOut->killFeed.visible)) {
        return false;
    }

    if (!readCount(reader, kMaxTextItems, &count)) {
        return false;
    }
    frameOut->killFeed.entries.clear();
    frameOut->killFeed.entries.reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index) {
        client::KillFeedEntryView entry;
        if (!readKillFeedEntry(reader, &entry)) {
            return false;
        }
        frameOut->killFeed.entries.push_back(std::move(entry));
    }

    if (!reader.readBool(&frameOut->scoreboard.visible) ||
        !reader.readU16(&frameOut->scoreboard.attackerScore) ||
        !reader.readU16(&frameOut->scoreboard.defenderScore) ||
        !readCount(reader, kMaxScoreboardSections, &count)) {
        return false;
    }
    frameOut->scoreboard.sections.clear();
    frameOut->scoreboard.sections.reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index) {
        client::ScoreboardSectionView section;
        if (!readScoreboardSection(reader, &section)) {
            return false;
        }
        frameOut->scoreboard.sections.push_back(std::move(section));
    }

    return reader.readBool(&frameOut->teamMenu.visible) &&
           reader.readString(&frameOut->teamMenu.currentTeamLabel) &&
           reader.readString(&frameOut->teamMenu.selectedTeamLabel) &&
           readReplayStatus(reader, &frameOut->replay) &&
           reader.readBool(&frameOut->localNetworkPanelVisible) &&
           reader.readBool(&frameOut->diagnosticsPanelVisible);
}

void writeHeader(ByteWriter& writer,
                 const client::ReplayRecordingMetadata& metadata,
                 std::size_t frameCount) {
    writer.writeU32(kReplayMagic);
    writer.writeU16(kReplayFormatVersion);
    writer.writeString(metadata.title);
    writer.writeString(metadata.sourceLabel);
    writer.writeI32(metadata.levelSlot);
    writer.writeU32(metadata.levelHash);
    writer.writeU64(metadata.createdUnixSeconds);
    writer.writeU32(static_cast<std::uint32_t>(frameCount));
}

bool readHeader(ByteReader& reader,
                client::ReplayRecordingMetadata* metadataOut,
                std::size_t* frameCountOut,
                std::string* errorOut) {
    std::uint32_t magic = 0u;
    std::uint16_t version = 0u;
    std::uint32_t frameCount = 0u;
    if (!reader.readU32(&magic) || magic != kReplayMagic) {
        setError(errorOut, "invalid replay file");
        return false;
    }
    if (!reader.readU16(&version) ||
        version < kMinimumReplayFormatVersion ||
        version > kReplayFormatVersion) {
        setError(errorOut, "unsupported replay version");
        return false;
    }

    client::ReplayRecordingMetadata metadata;
    metadata.formatVersion = version;
    if (!reader.readString(&metadata.title) ||
        !reader.readString(&metadata.sourceLabel) ||
        !readInt(reader, &metadata.levelSlot) ||
        !reader.readU32(&metadata.levelHash) ||
        !reader.readU64(&metadata.createdUnixSeconds) ||
        !readCount(reader, kMaxReplayFrames, &frameCount)) {
        setError(errorOut, "corrupt replay header");
        return false;
    }

    if (metadataOut != nullptr) {
        *metadataOut = std::move(metadata);
    }
    if (frameCountOut != nullptr) {
        *frameCountOut = frameCount;
    }
    return true;
}

client::ReplayRecording normalizedRecording(client::ReplayRecording recording) {
    if (recording.metadata.createdUnixSeconds == 0u) {
        recording.metadata.createdUnixSeconds = currentUnixSeconds();
    }
    if (recording.metadata.title.empty()) {
        recording.metadata.title = ReplayArchive::defaultTitle(recording.metadata.createdUnixSeconds);
    }
    recording.metadata.formatVersion = kReplayFormatVersion;
    return recording;
}

}  // namespace

ReplayArchive::ReplayArchive(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

const std::filesystem::path& ReplayArchive::directory() const {
    return directory_;
}

bool ReplayArchive::save(const client::ReplayRecording& recording,
                         std::filesystem::path* savedPathOut,
                         std::string* errorOut) const {
    if (recording.frames.empty()) {
        setError(errorOut, "no recorded frames to save");
        return false;
    }
    if (recording.frames.size() > kMaxReplayFrames) {
        setError(errorOut, "recording is too large");
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) {
        setError(errorOut, "failed to create replay directory");
        return false;
    }

    client::ReplayRecording normalized = normalizedRecording(recording);
    ByteWriter writer;
    try {
        writeHeader(writer, normalized.metadata, normalized.frames.size());
        for (const client::RecordedReplayFrame& frame : normalized.frames) {
            writer.writeF32(frame.timestamp);
            writer.writeBool(frame.localPlayerRenderItem.has_value());
            if (frame.localPlayerRenderItem.has_value()) {
                writeRemotePlayer(writer, *frame.localPlayerRenderItem);
            }
            writeRenderFrame(writer, frame.frame);
        }
    } catch (const std::exception&) {
        setError(errorOut, "failed to encode replay");
        return false;
    }

    const std::filesystem::path savedPath = uniqueReplayPath(directory_, normalized.metadata);
    const std::filesystem::path tempPath = savedPath.string() + ".tmp";
    const ByteBuffer bytes = std::move(writer).take();
    if (!writeFileBytes(tempPath, bytes)) {
        setError(errorOut, "failed to write replay file");
        return false;
    }

    std::filesystem::rename(tempPath, savedPath, ec);
    if (ec) {
        std::filesystem::remove(savedPath, ec);
        ec.clear();
        std::filesystem::rename(tempPath, savedPath, ec);
        if (ec) {
            setError(errorOut, "failed to publish replay file");
            return false;
        }
    }

    if (savedPathOut != nullptr) {
        *savedPathOut = savedPath;
    }
    return true;
}

bool ReplayArchive::load(const std::filesystem::path& path,
                         client::ReplayRecording* recordingOut,
                         std::string* errorOut) const {
    if (recordingOut == nullptr) {
        setError(errorOut, "missing replay output");
        return false;
    }

    ByteBuffer bytes;
    if (!readFileBytes(path, &bytes)) {
        setError(errorOut, "failed to open replay file");
        return false;
    }

    ByteReader reader(bytes);
    client::ReplayRecording recording;
    std::size_t frameCount = 0u;
    if (!readHeader(reader, &recording.metadata, &frameCount, errorOut)) {
        return false;
    }

    recording.frames.reserve(frameCount);
    for (std::size_t index = 0u; index < frameCount; ++index) {
        client::RecordedReplayFrame frame;
        if (!reader.readF32(&frame.timestamp)) {
            setError(errorOut, "corrupt replay frame");
            return false;
        }
        if (recording.metadata.formatVersion >= 2u) {
            bool hasLocalPlayerRenderItem = false;
            if (!reader.readBool(&hasLocalPlayerRenderItem)) {
                setError(errorOut, "corrupt replay frame");
                return false;
            }
            if (hasLocalPlayerRenderItem) {
                client::RemotePlayerRenderItem localPlayerRenderItem;
                if (!readRemotePlayer(reader, &localPlayerRenderItem)) {
                    setError(errorOut, "corrupt replay frame");
                    return false;
                }
                frame.localPlayerRenderItem = localPlayerRenderItem;
            }
        }
        if (!readRenderFrame(reader, &frame.frame)) {
            setError(errorOut, "corrupt replay frame");
            return false;
        }
        recording.frames.push_back(std::move(frame));
    }

    if (!reader.atEnd()) {
        setError(errorOut, "replay file has trailing data");
        return false;
    }

    *recordingOut = std::move(recording);
    return true;
}

bool ReplayArchive::loadMetadata(const std::filesystem::path& path,
                                 client::ReplayRecordingMetadata* metadataOut,
                                 std::size_t* frameCountOut,
                                 std::string* errorOut) const {
    ByteBuffer bytes;
    if (!readFileBytes(path, &bytes)) {
        setError(errorOut, "failed to open replay file");
        return false;
    }

    ByteReader reader(bytes);
    return readHeader(reader, metadataOut, frameCountOut, errorOut);
}

std::vector<ReplayArchiveEntry> ReplayArchive::list() const {
    std::vector<ReplayArchiveEntry> entries;
    std::error_code ec;
    if (!std::filesystem::exists(directory_, ec)) {
        return entries;
    }

    for (const auto& item : std::filesystem::directory_iterator(directory_, ec)) {
        if (ec || !item.is_regular_file() || item.path().extension() != kReplayExtension) {
            continue;
        }

        ReplayArchiveEntry entry;
        entry.path = item.path();
        entry.fileSizeBytes = item.file_size(ec);
        if (ec) {
            entry.fileSizeBytes = 0u;
            ec.clear();
        }
        if (loadMetadata(item.path(), &entry.metadata, &entry.frameCount, nullptr)) {
            entries.push_back(std::move(entry));
        }
    }

    std::sort(entries.begin(),
              entries.end(),
              [](const ReplayArchiveEntry& lhs, const ReplayArchiveEntry& rhs) {
                  if (lhs.metadata.createdUnixSeconds != rhs.metadata.createdUnixSeconds) {
                      return lhs.metadata.createdUnixSeconds > rhs.metadata.createdUnixSeconds;
                  }
                  return lhs.path.filename().string() < rhs.path.filename().string();
              });
    return entries;
}

std::filesystem::path ReplayArchive::replayDirectory() {
    std::filesystem::path base = userDataDirectory() / "replays";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return ec ? std::filesystem::current_path() / "replays" : base;
}

std::string ReplayArchive::defaultTitle(std::uint64_t createdUnixSeconds) {
    return "Replay " + timestampForFilename(createdUnixSeconds == 0u
                                                 ? currentUnixSeconds()
                                                 : createdUnixSeconds);
}

}  // namespace app
