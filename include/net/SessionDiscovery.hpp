#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/Protocol.hpp"
#include "net/SessionLaunchConfig.hpp"

namespace net {

constexpr std::uint32_t kSessionDiscoveryMagic = 0x44534C54u;  // "TLSD" as little-endian bytes.
constexpr std::uint8_t kSessionDiscoveryVersion = 3u;
constexpr std::uint64_t kDefaultSessionDiscoveryEntryTimeoutUs = 3'000'000u;

enum class SessionDiscoveryKind : std::uint8_t {
    Query = 0,
    Advertisement = 1
};

enum class SessionDiscoveryParseError : std::uint8_t {
    None = 0,
    BufferUnderflow = 1,
    InvalidMagic = 2,
    InvalidVersion = 3,
    InvalidKind = 4,
    InvalidShotEvaluationMode = 5,
    TrailingData = 6,
    InvalidSurface = 7,
    InvalidEntryPoint = 8
};

enum class BrowserCompatibilityState : std::uint8_t {
    Compatible = 0,
    IncompatibleProtocol = 1,
    InvalidMetadata = 2
};

struct SessionDiscoveryQuery {
    std::uint16_t protocolVersion{kProtocolVersion};
};

struct SessionAdvertisement {
    std::string sessionLabel{};
    std::string hostPlayerName{};
    std::int32_t levelSlot{-1};
    std::uint32_t levelHash{0u};
    std::string joinHost{};
    std::uint16_t joinPort{0u};
    std::uint16_t humanPlayers{0u};
    std::uint16_t maxHumanPlayers{2u};
    std::uint16_t protocolVersion{kProtocolVersion};
    ShotEvaluationMode shotEvaluationMode{ShotEvaluationMode::SeenPosition};
    SessionProductSurface surface{SessionProductSurface::Multiplayer};
    SessionEntryPoint entryPoint{SessionEntryPoint::Host};
    std::uint8_t localParticipantCount{kDefaultLocalParticipantCount};
    SessionStudyOptions studyOptions{};
};

struct SessionBrowserEntry {
    SessionAdvertisement advertisement{};
    BrowserCompatibilityState compatibility{BrowserCompatibilityState::Compatible};
    std::uint64_t lastSeenUs{0u};
    std::uint64_t expiresAtUs{0u};
};

struct SessionDiscoveryQueryParseResult {
    bool ok{false};
    SessionDiscoveryParseError error{SessionDiscoveryParseError::None};
    SessionDiscoveryQuery query{};
};

struct SessionAdvertisementParseResult {
    bool ok{false};
    SessionDiscoveryParseError error{SessionDiscoveryParseError::None};
    SessionAdvertisement advertisement{};
};

class SessionBrowserCache {
public:
    explicit SessionBrowserCache(std::uint64_t staleTimeoutUs = kDefaultSessionDiscoveryEntryTimeoutUs);

    void upsert(const SessionAdvertisement& advertisement, std::uint64_t nowUs);
    void expireStale(std::uint64_t nowUs);
    const std::vector<SessionBrowserEntry>& entries() const;

private:
    std::uint64_t staleTimeoutUs_{kDefaultSessionDiscoveryEntryTimeoutUs};
    std::vector<SessionBrowserEntry> entries_{};
};

const char* toString(SessionDiscoveryParseError error);
const char* toString(BrowserCompatibilityState state);
const char* shotEvaluationModeExplanation(ShotEvaluationMode mode);
std::string shotEvaluationModeSummary(ShotEvaluationMode mode);

BrowserCompatibilityState evaluateBrowserCompatibility(const SessionAdvertisement& advertisement);
std::string displayLabelForSessionAdvertisement(const SessionAdvertisement& advertisement);
std::string detailLineForSessionAdvertisement(const SessionAdvertisement& advertisement);
SessionAdvertisement makeSessionAdvertisement(const HostedSessionMetadata& metadata,
                                             const std::string& joinHost,
                                             std::uint16_t humanPlayers,
                                             std::uint16_t protocolVersion = kProtocolVersion);
SessionAdvertisement makeSessionAdvertisement(const SessionLaunchConfig& launchConfig,
                                             const std::string& joinHost,
                                             std::uint16_t humanPlayers,
                                             std::uint16_t protocolVersion = kProtocolVersion);

ByteBuffer serializeSessionDiscoveryQuery(const SessionDiscoveryQuery& query);
SessionDiscoveryQueryParseResult deserializeSessionDiscoveryQuery(const ByteBuffer& bytes);

ByteBuffer serializeSessionAdvertisement(const SessionAdvertisement& advertisement);
SessionAdvertisementParseResult deserializeSessionAdvertisement(const ByteBuffer& bytes);

bool operator==(const SessionDiscoveryQuery& lhs, const SessionDiscoveryQuery& rhs);
bool operator==(const SessionAdvertisement& lhs, const SessionAdvertisement& rhs);
bool operator==(const SessionBrowserEntry& lhs, const SessionBrowserEntry& rhs);

}  // namespace net
