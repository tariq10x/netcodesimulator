#include "net/Codec.hpp"
#include "net/SessionDiscovery.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace net {
namespace {

using codec::ByteReader;
using codec::ByteWriter;

void writeHeader(ByteWriter& writer, SessionDiscoveryKind kind) {
    writer.writeU32(kSessionDiscoveryMagic);
    writer.writeU8(kSessionDiscoveryVersion);
    writer.writeU8(static_cast<std::uint8_t>(kind));
}

SessionDiscoveryParseError readHeader(ByteReader& reader, SessionDiscoveryKind* kindOut) {
    std::uint32_t magic = 0u;
    std::uint8_t version = 0u;
    std::uint8_t kind = 0u;

    if (!reader.readU32(&magic) ||
        !reader.readU8(&version) ||
        !reader.readU8(&kind)) {
        return SessionDiscoveryParseError::BufferUnderflow;
    }

    if (magic != kSessionDiscoveryMagic) {
        return SessionDiscoveryParseError::InvalidMagic;
    }
    if (version != kSessionDiscoveryVersion) {
        return SessionDiscoveryParseError::InvalidVersion;
    }
    if (kind > static_cast<std::uint8_t>(SessionDiscoveryKind::Advertisement)) {
        return SessionDiscoveryParseError::InvalidKind;
    }

    if (kindOut != nullptr) {
        *kindOut = static_cast<SessionDiscoveryKind>(kind);
    }
    return SessionDiscoveryParseError::None;
}

bool tryReadSessionProductSurface(std::uint8_t rawSurface, SessionProductSurface* surfaceOut) {
    if (surfaceOut == nullptr) {
        return false;
    }

    switch (rawSurface) {
        case static_cast<std::uint8_t>(SessionProductSurface::Multiplayer):
            *surfaceOut = SessionProductSurface::Multiplayer;
            return true;
        case static_cast<std::uint8_t>(SessionProductSurface::LabStudy):
            *surfaceOut = SessionProductSurface::LabStudy;
            return true;
        case static_cast<std::uint8_t>(SessionProductSurface::Replay):
            *surfaceOut = SessionProductSurface::Replay;
            return true;
        case static_cast<std::uint8_t>(SessionProductSurface::LevelEditor):
            *surfaceOut = SessionProductSurface::LevelEditor;
            return true;
        default:
            return false;
    }
}

bool tryReadSessionEntryPoint(std::uint8_t rawEntryPoint, SessionEntryPoint* entryPointOut) {
    if (entryPointOut == nullptr) {
        return false;
    }

    switch (rawEntryPoint) {
        case static_cast<std::uint8_t>(SessionEntryPoint::Host):
            *entryPointOut = SessionEntryPoint::Host;
            return true;
        case static_cast<std::uint8_t>(SessionEntryPoint::Join):
            *entryPointOut = SessionEntryPoint::Join;
            return true;
        case static_cast<std::uint8_t>(SessionEntryPoint::LabStudy):
            *entryPointOut = SessionEntryPoint::LabStudy;
            return true;
        case static_cast<std::uint8_t>(SessionEntryPoint::Replay):
            *entryPointOut = SessionEntryPoint::Replay;
            return true;
        case static_cast<std::uint8_t>(SessionEntryPoint::LevelEditor):
            *entryPointOut = SessionEntryPoint::LevelEditor;
            return true;
        default:
            return false;
    }
}

bool hasCompatibleDiscoverySurface(const SessionAdvertisement& advertisement) {
    switch (advertisement.entryPoint) {
        case SessionEntryPoint::Host:
            return advertisement.surface == SessionProductSurface::Multiplayer;
        case SessionEntryPoint::LabStudy:
            return advertisement.surface == SessionProductSurface::LabStudy;
        case SessionEntryPoint::Join:
        case SessionEntryPoint::Replay:
        case SessionEntryPoint::LevelEditor:
        case SessionEntryPoint::None:
            return false;
    }
    return false;
}

bool sameAdvertisementIdentity(const SessionAdvertisement& lhs, const SessionAdvertisement& rhs) {
    return lhs.joinHost == rhs.joinHost &&
           lhs.joinPort == rhs.joinPort;
}

}  // namespace

SessionBrowserCache::SessionBrowserCache(std::uint64_t staleTimeoutUs)
    : staleTimeoutUs_(staleTimeoutUs) {}

void SessionBrowserCache::upsert(const SessionAdvertisement& advertisement, std::uint64_t nowUs) {
    const BrowserCompatibilityState compatibility = evaluateBrowserCompatibility(advertisement);
    const std::uint64_t expiresAtUs = nowUs + staleTimeoutUs_;

    const auto it = std::find_if(
        entries_.begin(),
        entries_.end(),
        [&advertisement](const SessionBrowserEntry& entry) {
            return sameAdvertisementIdentity(entry.advertisement, advertisement);
        });

    if (it != entries_.end()) {
        it->advertisement = advertisement;
        it->compatibility = compatibility;
        it->lastSeenUs = nowUs;
        it->expiresAtUs = expiresAtUs;
        return;
    }

    entries_.push_back(SessionBrowserEntry{advertisement, compatibility, nowUs, expiresAtUs});
}

void SessionBrowserCache::expireStale(std::uint64_t nowUs) {
    entries_.erase(
        std::remove_if(
            entries_.begin(),
            entries_.end(),
            [nowUs](const SessionBrowserEntry& entry) {
                return nowUs >= entry.expiresAtUs;
            }),
        entries_.end());
}

const std::vector<SessionBrowserEntry>& SessionBrowserCache::entries() const {
    return entries_;
}

const char* toString(SessionDiscoveryParseError error) {
    switch (error) {
        case SessionDiscoveryParseError::None: return "none";
        case SessionDiscoveryParseError::BufferUnderflow: return "buffer_underflow";
        case SessionDiscoveryParseError::InvalidMagic: return "invalid_magic";
        case SessionDiscoveryParseError::InvalidVersion: return "invalid_version";
        case SessionDiscoveryParseError::InvalidKind: return "invalid_kind";
        case SessionDiscoveryParseError::InvalidShotEvaluationMode: return "invalid_shot_evaluation_mode";
        case SessionDiscoveryParseError::TrailingData: return "trailing_data";
        case SessionDiscoveryParseError::InvalidSurface: return "invalid_surface";
        case SessionDiscoveryParseError::InvalidEntryPoint: return "invalid_entry_point";
    }
    return "unknown";
}

const char* toString(BrowserCompatibilityState state) {
    switch (state) {
        case BrowserCompatibilityState::Compatible: return "compatible";
        case BrowserCompatibilityState::IncompatibleProtocol: return "incompatible_protocol";
        case BrowserCompatibilityState::InvalidMetadata: return "invalid_metadata";
    }
    return "unknown";
}

const char* shotEvaluationModeExplanation(ShotEvaluationMode mode) {
    switch (mode) {
        case ShotEvaluationMode::SeenPosition:
            return "host rewinds targets to the shooter's view.";
        case ShotEvaluationMode::LivePosition:
            return "host judges hits against each target's live position.";
    }
    return "host uses the authoritative shot rule.";
}

std::string shotEvaluationModeSummary(ShotEvaluationMode mode) {
    return std::string("Rule ") + toString(mode) + ": " + shotEvaluationModeExplanation(mode);
}

BrowserCompatibilityState evaluateBrowserCompatibility(const SessionAdvertisement& advertisement) {
    if (advertisement.protocolVersion != kProtocolVersion) {
        return BrowserCompatibilityState::IncompatibleProtocol;
    }
    if (advertisement.joinHost.empty() ||
        advertisement.joinPort == 0u ||
        advertisement.levelSlot < 0 ||
        advertisement.levelHash == 0u ||
        advertisement.maxHumanPlayers == 0u ||
        advertisement.humanPlayers > advertisement.maxHumanPlayers ||
        advertisement.localParticipantCount == 0u ||
        !hasCompatibleDiscoverySurface(advertisement)) {
        return BrowserCompatibilityState::InvalidMetadata;
    }
    return BrowserCompatibilityState::Compatible;
}

std::string displayLabelForSessionAdvertisement(const SessionAdvertisement& advertisement) {
    if (!advertisement.sessionLabel.empty()) {
        return advertisement.sessionLabel;
    }
    if (!advertisement.hostPlayerName.empty()) {
        return advertisement.hostPlayerName;
    }
    return "Hosted Session";
}

std::string detailLineForSessionAdvertisement(const SessionAdvertisement& advertisement) {
    return advertisement.joinHost + ":" + std::to_string(advertisement.joinPort) +
           " | " + shotEvaluationModeSummary(advertisement.shotEvaluationMode);
}

SessionAdvertisement makeSessionAdvertisement(const HostedSessionMetadata& metadata,
                                             const std::string& joinHost,
                                             std::uint16_t humanPlayers,
                                             std::uint16_t protocolVersion) {
    SessionLaunchConfig config;
    config.entryPoint = SessionEntryPoint::Host;
    config.surface = SessionProductSurface::Multiplayer;
    config.sessionLabel = metadata.sessionLabel;
    config.playerName = metadata.hostPlayerName;
    config.levelSlot = metadata.levelSlot;
    config.levelHash = metadata.levelHash;
    config.shotEvaluationMode = metadata.shotEvaluationMode;
    config.studyEventRunId = metadata.studyEventRunId;
    config.studyOptions.enableEventLogging = metadata.studyEventLoggingEnabled;
    config.publicJoinPort = metadata.publicJoinPort;
    config.maxHumanPlayers = metadata.maxHumanPlayers;
    normalizeSessionLaunchConfig(&config);
    return makeSessionAdvertisement(config, joinHost, humanPlayers, protocolVersion);
}

SessionAdvertisement makeSessionAdvertisement(const SessionLaunchConfig& launchConfig,
                                             const std::string& joinHost,
                                             std::uint16_t humanPlayers,
                                             std::uint16_t protocolVersion) {
    SessionLaunchConfig normalized = launchConfig;
    normalizeSessionLaunchConfig(&normalized);

    SessionAdvertisement advertisement;
    advertisement.sessionLabel = normalized.sessionLabel;
    advertisement.hostPlayerName = normalized.playerName;
    advertisement.levelSlot = normalized.levelSlot;
    advertisement.levelHash = normalized.levelHash;
    advertisement.joinHost = joinHost;
    advertisement.joinPort = normalized.publicJoinPort;
    advertisement.humanPlayers = humanPlayers;
    advertisement.maxHumanPlayers = normalized.maxHumanPlayers;
    advertisement.protocolVersion = protocolVersion;
    advertisement.shotEvaluationMode = normalized.shotEvaluationMode;
    advertisement.surface = normalized.surface;
    advertisement.entryPoint = normalized.entryPoint;
    advertisement.localParticipantCount = normalized.localParticipantCount;
    advertisement.studyOptions = normalized.studyOptions;
    return advertisement;
}

ByteBuffer serializeSessionDiscoveryQuery(const SessionDiscoveryQuery& query) {
    ByteWriter writer;
    writeHeader(writer, SessionDiscoveryKind::Query);
    writer.writeU16(query.protocolVersion);
    return std::move(writer).take();
}

SessionDiscoveryQueryParseResult deserializeSessionDiscoveryQuery(const ByteBuffer& bytes) {
    SessionDiscoveryQueryParseResult result;
    ByteReader reader(bytes);
    SessionDiscoveryKind kind = SessionDiscoveryKind::Query;
    result.error = readHeader(reader, &kind);
    if (result.error != SessionDiscoveryParseError::None) {
        return result;
    }
    if (kind != SessionDiscoveryKind::Query) {
        result.error = SessionDiscoveryParseError::InvalidKind;
        return result;
    }
    if (!reader.readU16(&result.query.protocolVersion)) {
        result.error = SessionDiscoveryParseError::BufferUnderflow;
        return result;
    }
    if (!reader.atEnd()) {
        result.error = SessionDiscoveryParseError::TrailingData;
        return result;
    }

    result.ok = true;
    result.error = SessionDiscoveryParseError::None;
    return result;
}

ByteBuffer serializeSessionAdvertisement(const SessionAdvertisement& advertisement) {
    ByteWriter writer;
    writeHeader(writer, SessionDiscoveryKind::Advertisement);
    writer.writeString(advertisement.sessionLabel);
    writer.writeString(advertisement.hostPlayerName);
    writer.writeI32(advertisement.levelSlot);
    writer.writeU32(advertisement.levelHash);
    writer.writeString(advertisement.joinHost);
    writer.writeU16(advertisement.joinPort);
    writer.writeU16(advertisement.humanPlayers);
    writer.writeU16(advertisement.maxHumanPlayers);
    writer.writeU16(advertisement.protocolVersion);
    writer.writeU8(static_cast<std::uint8_t>(advertisement.shotEvaluationMode));
    writer.writeU8(static_cast<std::uint8_t>(advertisement.surface));
    writer.writeU8(static_cast<std::uint8_t>(advertisement.entryPoint));
    writer.writeU8(advertisement.localParticipantCount);
    writer.writeU8(advertisement.studyOptions.enablePredictionToggle ? 1u : 0u);
    writer.writeU8(advertisement.studyOptions.enableShotStrategyToggle ? 1u : 0u);
    writer.writeU8(advertisement.studyOptions.enableReplayCapture ? 1u : 0u);
    writer.writeU8(advertisement.studyOptions.enableEventLogging ? 1u : 0u);
    return std::move(writer).take();
}

SessionAdvertisementParseResult deserializeSessionAdvertisement(const ByteBuffer& bytes) {
    SessionAdvertisementParseResult result;
    ByteReader reader(bytes);
    SessionDiscoveryKind kind = SessionDiscoveryKind::Query;
    result.error = readHeader(reader, &kind);
    if (result.error != SessionDiscoveryParseError::None) {
        return result;
    }
    if (kind != SessionDiscoveryKind::Advertisement) {
        result.error = SessionDiscoveryParseError::InvalidKind;
        return result;
    }
    std::uint8_t shotMode = 0u;
    std::uint8_t surface = 0u;
    std::uint8_t entryPoint = 0u;
    std::uint8_t predictionToggle = 0u;
    std::uint8_t shotStrategyToggle = 0u;
    std::uint8_t replayCapture = 0u;
    std::uint8_t eventLogging = 0u;
    if (!reader.readString(&result.advertisement.sessionLabel) ||
        !reader.readString(&result.advertisement.hostPlayerName) ||
        !reader.readI32(&result.advertisement.levelSlot) ||
        !reader.readU32(&result.advertisement.levelHash) ||
        !reader.readString(&result.advertisement.joinHost) ||
        !reader.readU16(&result.advertisement.joinPort) ||
        !reader.readU16(&result.advertisement.humanPlayers) ||
        !reader.readU16(&result.advertisement.maxHumanPlayers) ||
        !reader.readU16(&result.advertisement.protocolVersion) ||
        !reader.readU8(&shotMode) ||
        !reader.readU8(&surface) ||
        !reader.readU8(&entryPoint) ||
        !reader.readU8(&result.advertisement.localParticipantCount) ||
        !reader.readU8(&predictionToggle) ||
        !reader.readU8(&shotStrategyToggle) ||
        !reader.readU8(&replayCapture) ||
        !reader.readU8(&eventLogging)) {
        result.error = SessionDiscoveryParseError::BufferUnderflow;
        return result;
    }
    if (!tryParseShotEvaluationMode(shotMode, &result.advertisement.shotEvaluationMode)) {
        result.error = SessionDiscoveryParseError::InvalidShotEvaluationMode;
        return result;
    }
    if (!tryReadSessionProductSurface(surface, &result.advertisement.surface)) {
        result.error = SessionDiscoveryParseError::InvalidSurface;
        return result;
    }
    if (!tryReadSessionEntryPoint(entryPoint, &result.advertisement.entryPoint)) {
        result.error = SessionDiscoveryParseError::InvalidEntryPoint;
        return result;
    }
    result.advertisement.studyOptions.enablePredictionToggle = predictionToggle != 0u;
    result.advertisement.studyOptions.enableShotStrategyToggle = shotStrategyToggle != 0u;
    result.advertisement.studyOptions.enableReplayCapture = replayCapture != 0u;
    result.advertisement.studyOptions.enableEventLogging = eventLogging != 0u;
    if (!reader.atEnd()) {
        result.error = SessionDiscoveryParseError::TrailingData;
        return result;
    }

    result.ok = true;
    result.error = SessionDiscoveryParseError::None;
    return result;
}

bool operator==(const SessionDiscoveryQuery& lhs, const SessionDiscoveryQuery& rhs) {
    return lhs.protocolVersion == rhs.protocolVersion;
}

bool operator==(const SessionAdvertisement& lhs, const SessionAdvertisement& rhs) {
    return lhs.sessionLabel == rhs.sessionLabel &&
           lhs.hostPlayerName == rhs.hostPlayerName &&
           lhs.levelSlot == rhs.levelSlot &&
           lhs.levelHash == rhs.levelHash &&
           lhs.joinHost == rhs.joinHost &&
           lhs.joinPort == rhs.joinPort &&
           lhs.humanPlayers == rhs.humanPlayers &&
           lhs.maxHumanPlayers == rhs.maxHumanPlayers &&
           lhs.protocolVersion == rhs.protocolVersion &&
           lhs.shotEvaluationMode == rhs.shotEvaluationMode &&
           lhs.surface == rhs.surface &&
           lhs.entryPoint == rhs.entryPoint &&
           lhs.localParticipantCount == rhs.localParticipantCount &&
           lhs.studyOptions.enablePredictionToggle == rhs.studyOptions.enablePredictionToggle &&
           lhs.studyOptions.enableShotStrategyToggle == rhs.studyOptions.enableShotStrategyToggle &&
           lhs.studyOptions.enableReplayCapture == rhs.studyOptions.enableReplayCapture &&
           lhs.studyOptions.enableEventLogging == rhs.studyOptions.enableEventLogging;
}

bool operator==(const SessionBrowserEntry& lhs, const SessionBrowserEntry& rhs) {
    return lhs.advertisement == rhs.advertisement &&
           lhs.compatibility == rhs.compatibility &&
           lhs.lastSeenUs == rhs.lastSeenUs &&
           lhs.expiresAtUs == rhs.expiresAtUs;
}

}  // namespace net
