#include "net/SessionDiscovery.hpp"
#include "net/SessionLaunchConfig.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

net::SessionAdvertisement makeAdvertisement() {
    net::SessionLaunchConfig config =
        net::makeStudySessionLaunchConfig(4,
                                          "host-player",
                                          41000u,
                                          1u,
                                          1u,
                                          net::ShotEvaluationMode::SeenPosition,
                                          2u);
    config.sessionLabel = "Player LAN Match";
    return net::makeSessionAdvertisement(config, "192.168.0.24", 1u);
}

void testAdvertisementRoundTripPreservesSessionMetadata() {
    const net::SessionAdvertisement advertisement = makeAdvertisement();

    const net::ByteBuffer bytes = net::serializeSessionAdvertisement(advertisement);
    const net::SessionAdvertisementParseResult parsed =
        net::deserializeSessionAdvertisement(bytes);

    expect(parsed.ok, "discovery advertisement should deserialize successfully");
    expect(parsed.advertisement == advertisement,
           "discovery advertisement should round-trip exactly");
    expect(parsed.advertisement.joinPort == 41000u &&
               parsed.advertisement.levelSlot == 4,
           "discovery advertisement should preserve join port and level identity");
    expect(parsed.advertisement.humanPlayers == 1u &&
               parsed.advertisement.maxHumanPlayers == 2u,
           "discovery advertisement should preserve occupancy information");
    expect(parsed.advertisement.shotEvaluationMode == net::ShotEvaluationMode::SeenPosition,
           "discovery advertisement should preserve the authoritative shot-evaluation rule");
    expect(net::detailLineForSessionAdvertisement(parsed.advertisement).find(
               net::shotEvaluationModeSummary(parsed.advertisement.shotEvaluationMode)) !=
               std::string::npos,
           "discovery advertisement detail text should include the canonical shot-rule explanation");
    expect(parsed.advertisement.surface == net::SessionProductSurface::LabStudy &&
               parsed.advertisement.entryPoint == net::SessionEntryPoint::LabStudy &&
               parsed.advertisement.localParticipantCount == 2u,
           "discovery advertisement should preserve the shared launch surface or entry point or local participant count");
    expect(parsed.advertisement.studyOptions.enablePredictionToggle &&
               parsed.advertisement.studyOptions.enableShotStrategyToggle &&
               parsed.advertisement.studyOptions.enableReplayCapture &&
               !parsed.advertisement.studyOptions.enableEventLogging,
           "discovery advertisement should preserve the shared study-option contract without parser drift while keeping event logging opt-in");
}

void testHostedMetadataAndDiscoveryRowsShareShotRuleSummary() {
    net::SessionLaunchConfig config =
        net::makeHostSessionLaunchConfig(4,
                                         "host-player",
                                         41000u,
                                         0u,
                                         0u,
                                         0u,
                                         net::kDefaultProxyServerPort,
                                         net::kProtocolVersion,
                                         sim::TeamId::Attacker,
                                         1u,
                                         net::ShotEvaluationMode::LivePosition);
    config.sessionLabel = "Metadata Match";

    const net::HostedSessionMetadata metadata = net::makeHostedSessionMetadata(config);
    const net::SessionAdvertisement advertisement =
        net::makeSessionAdvertisement(metadata, "192.168.0.24", 1u);

    expect(advertisement.shotEvaluationMode == metadata.shotEvaluationMode,
           "discovery advertisements should preserve the authoritative shot-evaluation mode from hosted session metadata");
    expect(net::shotEvaluationModeSummary(advertisement.shotEvaluationMode) ==
               net::shotEvaluationModeSummary(metadata.shotEvaluationMode),
           "hosted session metadata and LAN discovery should share the same shot-rule explanation text");
    expect(net::detailLineForSessionAdvertisement(advertisement).find(
               net::shotEvaluationModeSummary(metadata.shotEvaluationMode)) != std::string::npos,
           "LAN discovery detail text should expose the shot-rule expectation carried by hosted metadata");
}

void testDuplicateAdvertisementsUpdateExistingBrowserEntry() {
    net::SessionBrowserCache cache;

    net::SessionAdvertisement advertisement = makeAdvertisement();
    cache.upsert(advertisement, 1'000'000u);

    advertisement.humanPlayers = 2u;
    advertisement.sessionLabel = "Player LAN Match (Full)";
    cache.upsert(advertisement, 1'500'000u);

    expect(cache.entries().size() == 1u,
           "duplicate advertisements for the same join endpoint should update in place");
    expect(cache.entries().front().advertisement.humanPlayers == 2u,
           "browser cache should preserve the newest occupancy data");
    expect(cache.entries().front().advertisement.sessionLabel == "Player LAN Match (Full)",
           "browser cache should preserve the newest session metadata when updating in place");
    expect(cache.entries().front().lastSeenUs == 1'500'000u,
           "browser cache should refresh the last-seen timestamp for duplicate advertisements");
}

void testStaleEntriesExpireDeterministically() {
    net::SessionBrowserCache cache(3'000'000u);

    cache.upsert(makeAdvertisement(), 2'000'000u);
    expect(cache.entries().size() == 1u,
           "browser cache should store advertisements before expiry");

    cache.expireStale(4'999'999u);
    expect(cache.entries().size() == 1u,
           "browser cache should keep entries until the stale timeout elapses");

    cache.expireStale(5'000'000u);
    expect(cache.entries().empty(),
           "browser cache should remove entries deterministically once the stale timeout is reached");
}

void testInvalidShotEvaluationModeIsRejectedDeterministically() {
    const net::SessionAdvertisement advertisement = makeAdvertisement();
    net::ByteBuffer bytes = net::serializeSessionAdvertisement(advertisement);
    bytes[bytes.size() - 8u] = 0xFFu;

    const net::SessionAdvertisementParseResult parsed =
        net::deserializeSessionAdvertisement(bytes);

    expect(!parsed.ok, "discovery advertisements with unknown shot modes should be rejected");
    expect(parsed.error == net::SessionDiscoveryParseError::InvalidShotEvaluationMode,
           "discovery advertisements with unknown shot modes should surface a deterministic parse error");
}

void testInvalidDiscoveryEntryPointIsRejectedDeterministically() {
    const net::SessionAdvertisement advertisement = makeAdvertisement();
    net::ByteBuffer bytes = net::serializeSessionAdvertisement(advertisement);
    bytes[bytes.size() - 6u] = 0xFFu;

    const net::SessionAdvertisementParseResult parsed =
        net::deserializeSessionAdvertisement(bytes);

    expect(!parsed.ok, "discovery advertisements with unknown entry points should be rejected");
    expect(parsed.error == net::SessionDiscoveryParseError::InvalidEntryPoint,
           "discovery advertisements with unknown entry points should surface a deterministic parse error");
}

void testCompatibilityFilteringStaysDeterministicAcrossRefreshedAdvertisements() {
    net::SessionBrowserCache cache;

    net::SessionLaunchConfig hostConfig =
        net::makeHostSessionLaunchConfig(4,
                                         "host-player",
                                         41000u,
                                         0u,
                                         0u,
                                         0u,
                                         net::kDefaultProxyServerPort,
                                         net::kProtocolVersion,
                                         sim::TeamId::Attacker,
                                         1u,
                                         net::ShotEvaluationMode::LivePosition);
    hostConfig.sessionLabel = "Host Match";
    cache.upsert(net::makeSessionAdvertisement(hostConfig, "192.168.0.24", 1u), 1'000'000u);
    expect(cache.entries().size() == 1u &&
               cache.entries().front().compatibility == net::BrowserCompatibilityState::Compatible,
           "browser cache should treat a valid hosted runtime advertisement as compatible");

    net::SessionLaunchConfig studyConfig =
        net::makeStudySessionLaunchConfig(4,
                                          "host-player",
                                          41000u,
                                          1u,
                                          1u,
                                          net::ShotEvaluationMode::SeenPosition,
                                          1u);
    studyConfig.sessionLabel = "Study Match";
    cache.upsert(net::makeSessionAdvertisement(studyConfig, "192.168.0.24", 1u), 1'500'000u);
    expect(cache.entries().size() == 1u &&
               cache.entries().front().compatibility == net::BrowserCompatibilityState::Compatible &&
               cache.entries().front().advertisement.surface == net::SessionProductSurface::LabStudy &&
               cache.entries().front().advertisement.shotEvaluationMode ==
                   net::ShotEvaluationMode::SeenPosition,
           "browser cache should keep compatibility deterministic when the same hosted endpoint refreshes with new launch metadata");

    net::SessionLaunchConfig replayConfig = net::makeReplaySessionLaunchConfig(4);
    replayConfig.sessionLabel = "Replay Only";
    cache.upsert(net::makeSessionAdvertisement(replayConfig, "192.168.0.24", 1u), 2'000'000u);
    expect(cache.entries().size() == 1u &&
               cache.entries().front().compatibility == net::BrowserCompatibilityState::InvalidMetadata,
           "browser cache should deterministically mark non-runtime replay advertisements as incompatible on refresh");
}

}  // namespace

int main() {
    try {
        testAdvertisementRoundTripPreservesSessionMetadata();
        testHostedMetadataAndDiscoveryRowsShareShotRuleSummary();
        testDuplicateAdvertisementsUpdateExistingBrowserEntry();
        testStaleEntriesExpireDeterministically();
        testInvalidShotEvaluationModeIsRejectedDeterministically();
        testInvalidDiscoveryEntryPointIsRejectedDeterministically();
        testCompatibilityFilteringStaysDeterministicAcrossRefreshedAdvertisements();
        std::cout << "SessionDiscoveryTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "SessionDiscoveryTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
