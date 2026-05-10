#include "net/DiagnosticsModel.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "net/ProxyRuntime.hpp"
#include "net/TransportArtifactAdapter.hpp"

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

net::Packet makeCommandPacket(std::uint32_t seq, std::uint16_t peerId = 1u) {
    net::Packet packet;
    packet.header.peerId = peerId;
    packet.header.channel = net::Channel::Command;
    packet.header.seq = seq;
    packet.header.kind = net::PacketKind::CommandBundle;
    packet.payload = net::CommandBundle{};
    return packet;
}

std::vector<net::Packet> drainPackets(net::UdpSocket* socket) {
    std::vector<net::Packet> packets;
    net::ReceivedDatagram datagram;
    while (true) {
        const net::ReceiveStatus status = socket->receive(&datagram);
        if (status == net::ReceiveStatus::WouldBlock) {
            break;
        }
        expect(status == net::ReceiveStatus::Received, "diagnostics test receive should not error");
        const auto parsed = net::deserializePacket(datagram.payload);
        expect(parsed.ok, "diagnostics tests should receive valid protocol packets");
        packets.push_back(parsed.packet);
    }
    return packets;
}

std::vector<net::Packet> drainPacketsOfKind(net::UdpSocket* socket, net::PacketKind kind) {
    std::vector<net::Packet> matchingPackets;
    const auto packets = drainPackets(socket);
    for (const auto& packet : packets) {
        if (packet.header.kind == kind) {
            matchingPackets.push_back(packet);
        }
    }
    return matchingPackets;
}

template <typename Predicate>
bool waitForPredicate(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

void waitForProxyReceived(net::ProxyRuntime* proxy,
                          bool upstream,
                          std::uint64_t expectedCount,
                          std::uint64_t tickUs) {
    const bool received = waitForPredicate([&]() {
        proxy->tick(tickUs);
        return proxy->aggregateStats(upstream).receivedPackets >= expectedCount;
    }, std::chrono::milliseconds(250));
    expect(received, "diagnostics test timed out waiting for proxy ingress");
}

std::vector<net::Packet> waitForPacketsOfKind(net::UdpSocket* socket,
                                              net::PacketKind kind,
                                              std::size_t expectedCount) {
    std::vector<net::Packet> packets;
    const bool received = waitForPredicate([&]() {
        auto next = drainPacketsOfKind(socket, kind);
        if (!next.empty()) {
            packets.insert(packets.end(), next.begin(), next.end());
        }
        return packets.size() >= expectedCount;
    }, std::chrono::milliseconds(250));
    expect(received, "diagnostics test timed out waiting for forwarded packets of the expected kind");
    return packets;
}

bool contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

sim::RosterEntry makeRosterEntry(int actorId,
                                 sim::TeamId team,
                                 bool isBot,
                                 const std::string& displayName) {
    sim::RosterEntry entry;
    entry.actorId = actorId;
    entry.team = team;
    entry.sessionPresence = sim::SessionPresence::Connected;
    entry.participation = team == sim::TeamId::Spectator
        ? sim::ParticipationState::Spectating
        : (sim::isPlayableTeam(team)
               ? sim::ParticipationState::Playing
               : sim::ParticipationState::TeamSelection);
    entry.control = sim::ControlBinding{
        sim::isPlayableTeam(team) ? sim::ControlBindingKind::Actor
                                  : sim::ControlBindingKind::None,
        sim::isPlayableTeam(team) ? actorId : -1};
    entry.isBot = isBot;
    entry.alive = true;
    entry.displayName = displayName;
    return entry;
}

void testLocalNetworkSettingsMirrorLatencyAndLossAcrossDirections() {
    net::DiagnosticsModel model(7u);
    model.setLocalLatencyMs(120.0f);
    model.setLocalLossPct(5.0f);

    expect(model.localNetworkSettings().latencyMs == 120.0f &&
               model.localNetworkSettings().lossPct == 5.0f,
           "player-facing local network settings should preserve the configured latency and loss values");
    expect(model.linkConfig(true).baseDelayMs == 120.0f &&
               model.linkConfig(false).baseDelayMs == 120.0f,
           "player-facing latency should mirror deterministically to both upstream and downstream proxy configs");
    expect(model.linkConfig(true).lossPct == 5.0f &&
               model.linkConfig(false).lossPct == 5.0f,
           "player-facing loss should mirror deterministically to both upstream and downstream proxy configs");
    expect(model.linkConfig(true).jitterMs == 0.0f &&
               model.linkConfig(false).duplicatePct == 0.0f &&
               model.linkConfig(false).reorderPct == 0.0f,
           "player-facing local network settings should leave advanced proxy controls at their neutral defaults");
}

void testDirectionalProxyOverridesSurviveAuthoritativeSync() {
    net::DiagnosticsModel model(7u);
    model.setLocalLatencyMs(120.0f);
    model.setLocalLossPct(5.0f);

    model.setLossPct(true, 22.0f);
    model.setBaseDelayMs(false, 70.0f);
    model.syncAuthoritativeLocalNetworkSettings(120u, 5u);

    expect(model.linkConfig(true).lossPct == 22.0f &&
               model.linkConfig(false).lossPct == 5.0f,
           "directional upstream loss should survive authoritative symmetric loss refreshes");
    expect(model.linkConfig(true).baseDelayMs == 120.0f &&
               model.linkConfig(false).baseDelayMs == 70.0f,
           "directional downstream delay should survive authoritative symmetric latency refreshes");

    model.setLocalLossPct(12.0f);
    expect(model.linkConfig(true).lossPct == 12.0f &&
               model.linkConfig(false).lossPct == 12.0f,
           "using the symmetric packet-loss control should intentionally re-mirror both directions");

    model.setLocalLatencyMs(80.0f);
    expect(model.linkConfig(true).baseDelayMs == 80.0f &&
               model.linkConfig(false).baseDelayMs == 80.0f,
           "using the symmetric latency control should intentionally re-mirror both directions");
}

void testLocalNetworkSettingsBuildUnifiedRuntimeParamRequests() {
    net::DiagnosticsModel model(7u);
    model.setLocalLatencyMs(120.0f);
    model.setLocalLossPct(5.0f);

    const auto requests = model.buildLocalNetworkRequests();
    expect(requests.size() == 2u,
           "player-facing local network settings should produce one latency request and one loss request");
    expect(requests[0].scope == net::RuntimeParamScope::Player &&
               requests[0].targetId == 7 &&
               requests[0].key == "net.player[7].latency_ms" &&
               requests[0].value == 120.0f,
           "the diagnostics model should emit the shared player-scoped latency request shape");
    expect(requests[1].scope == net::RuntimeParamScope::Player &&
               requests[1].targetId == 7 &&
               requests[1].key == "net.player[7].loss_pct" &&
               requests[1].value == 5.0f,
           "the diagnostics model should emit the shared player-scoped loss request shape");
}

void testBotTargetBuildsBotScopedRuntimeParamRequests() {
    net::DiagnosticsModel model(net::kFirstBotTransportTargetId);
    model.setLocalLatencyMs(80.0f);
    model.setLocalLossPct(12.0f);

    const auto requests = model.buildLocalNetworkRequests();
    expect(requests.size() == 2u,
           "bot-facing local network settings should still produce one latency request and one loss request");
    expect(requests[0].scope == net::RuntimeParamScope::Bot &&
               requests[0].targetId == static_cast<std::int32_t>(net::kFirstBotTransportTargetId) &&
               requests[0].key == "net.bot[1000].latency_ms" &&
               requests[0].value == 80.0f,
           "the diagnostics model should emit the shared bot-scoped latency request shape");
    expect(requests[1].scope == net::RuntimeParamScope::Bot &&
               requests[1].targetId == static_cast<std::int32_t>(net::kFirstBotTransportTargetId) &&
               requests[1].key == "net.bot[1000].loss_pct" &&
               requests[1].value == 12.0f,
           "the diagnostics model should emit the shared bot-scoped loss request shape");

    const auto lines = model.localNetworkSummaryLines();
    expect(contains(lines, "Applies to bot 1000"),
           "bot-targeted summaries should expose the selected authoritative bot target");
}

void testRuntimeParameterViewKeepsConsoleAndPanelStateSynchronized() {
    net::DiagnosticsModel model(7u);
    model.setLocalLatencyMs(120.0f);
    model.setLocalLossPct(5.0f);

    const net::RuntimeParameterViewState view = model.runtimeParameterView();
    expect(view.available &&
               view.requestedLatencyMs == 120.0f &&
               view.requestedLossPct == 5.0f &&
               !view.hasAuthoritativeValues &&
               view.effectiveLatencyMs == 120.0f &&
               view.effectiveLossPct == 5.0f,
           "the diagnostics model should expose one shared runtime-parameter view before authoritative feedback arrives");

    const auto consoleLines = model.summaryLines();
    const auto panelLines = model.localNetworkSummaryLines();
    expect(contains(consoleLines, "Requested Latency 120ms | Loss 5%") &&
               contains(panelLines, "Requested Latency 120ms | Loss 5%"),
           "console and panel summaries should expose the same requested runtime-parameter values");
    expect(contains(consoleLines, "Awaiting Latency 120ms | Loss 5%") &&
               contains(panelLines, "Awaiting Latency 120ms | Loss 5%"),
           "console and panel summaries should expose the same effective values before authoritative feedback");
    expect(contains(consoleLines, "Status pending_authoritative_apply") &&
               contains(panelLines, "Status pending_authoritative_apply"),
           "console and panel summaries should share the same pending-apply status text");
}

void testLocalNetworkAuthoritativeFeedbackConsumesApplyResults() {
    net::DiagnosticsModel model(7u);
    model.setLocalLatencyMs(120.0f);
    model.setLocalLossPct(5.0f);

    expect(model.consumeRuntimeParamApplyResult(net::RuntimeParamApplyResult{
               net::RuntimeParamScope::Player,
               7,
               "net.player[7].latency_ms",
               130.0f,
               true,
               sim::StagedApplyBoundary::NextTick,
               "applied"
           }),
           "the diagnostics model should consume authoritative apply results for local latency");
    expect(model.consumeRuntimeParamApplyResult(net::RuntimeParamApplyResult{
               net::RuntimeParamScope::Player,
               7,
               "net.player[7].loss_pct",
               9.0f,
               true,
               sim::StagedApplyBoundary::NextTick,
               "applied"
           }),
           "the diagnostics model should consume authoritative apply results for local loss");
    expect(model.hasAuthoritativeLocalNetworkSettings(),
           "authoritative apply results should establish authoritative local-network state");
    expect(model.authoritativeLocalNetworkSettings().latencyMs == 130.0f &&
               model.authoritativeLocalNetworkSettings().lossPct == 9.0f,
           "authoritative apply results should become the diagnostics model's applied local-network settings");
    expect(model.localNetworkSettings().latencyMs == 130.0f &&
               model.localNetworkSettings().lossPct == 9.0f,
           "authoritative apply results should reconcile the live slider state to the applied values");

    const auto lines = model.localNetworkSummaryLines();
    expect(contains(lines, "Applied Latency 130ms | Loss 9%"),
           "local-network summary lines should expose authoritative apply-result values");
    expect(contains(lines, "Status applied | Boundary next_tick"),
           "local-network summary lines should expose the latest authoritative apply status");

    const auto consoleLines = model.summaryLines();
    expect(contains(consoleLines, "Applied Latency 130ms | Loss 9%") &&
               contains(consoleLines, "Status applied | Boundary next_tick"),
           "console summary lines should consume the same authoritative apply state as the local-network panel");
}

void testLocalNetworkAuthoritativeFeedbackConsumesSnapshots() {
    net::DiagnosticsModel model(7u);
    model.setLocalLatencyMs(120.0f);
    model.setLocalLossPct(5.0f);

    expect(model.consumeRuntimeParamSnapshot(net::RuntimeParamSnapshot{
               net::RuntimeParamScope::Player,
               7,
               "net.player[7].latency_ms",
               80.0f
           }),
           "the diagnostics model should consume authoritative runtime-parameter snapshots for local latency");
    expect(model.consumeRuntimeParamSnapshot(net::RuntimeParamSnapshot{
               net::RuntimeParamScope::Player,
               7,
               "net.player[7].loss_pct",
               3.0f
           }),
           "the diagnostics model should consume authoritative runtime-parameter snapshots for local loss");
    expect(model.hasAuthoritativeLocalNetworkSettings(),
           "authoritative snapshots should establish authoritative local-network state");
    expect(model.authoritativeLocalNetworkSettings().latencyMs == 80.0f &&
               model.authoritativeLocalNetworkSettings().lossPct == 3.0f,
           "authoritative snapshots should become the diagnostics model's applied local-network settings");
    expect(model.localNetworkSettings().latencyMs == 80.0f &&
               model.localNetworkSettings().lossPct == 3.0f,
           "authoritative snapshots should reconcile the live slider state to the shared snapshot values");
}

void testStagedApplyFeedbackAppearsOnBothSummaries() {
    net::DiagnosticsModel model(7u);
    model.setLocalLatencyMs(200.0f);
    model.setLocalLossPct(12.0f);

    expect(model.consumeRuntimeParamApplyResult(net::RuntimeParamApplyResult{
               net::RuntimeParamScope::Player,
               7,
               "net.player[7].latency_ms",
               200.0f,
               false,
               sim::StagedApplyBoundary::NextSessionRestart,
               "staged_for_restart"
           }),
           "the diagnostics model should consume staged apply feedback for player latency");

    const net::RuntimeParameterViewState view = model.runtimeParameterView();
    expect(view.hasApplyResult &&
               !view.applied &&
               view.stagedApplyBoundary == sim::StagedApplyBoundary::NextSessionRestart &&
               view.statusMessage == "staged_for_restart",
           "the shared runtime-parameter view should preserve staged-apply feedback");

    const auto consoleLines = model.summaryLines();
    const auto panelLines = model.localNetworkSummaryLines();
    expect(contains(consoleLines, "Status staged_for_restart | Boundary next_session_restart") &&
               contains(panelLines, "Status staged_for_restart | Boundary next_session_restart"),
           "console and panel summaries should expose the same staged-apply boundary feedback");
}

void testShotEvaluationViewExposesRuleTimingAndOutcome() {
    net::DiagnosticsModel model(7u);
    model.recordShotEvaluation(net::ShotEvaluationRecord{
        1'420'000u,
        1'100'000u,
        net::ShotEvaluationStateView::RewoundState,
        net::ShotEvaluationMode::SeenPosition,
        true
    });

    const net::ShotEvaluationViewState hitView = model.shotEvaluationView();
    expect(hitView.available &&
               hitView.viewedTimeUs == 1'420'000u &&
               hitView.rewindTimeUs == 1'100'000u &&
               hitView.evaluatedState == net::ShotEvaluationStateView::RewoundState &&
               hitView.evaluatedStateLabel == "rewound_state",
           "shot diagnostics should preserve viewed time or rewind time or evaluated state for the authoritative shot study");
    expect(hitView.shotEvaluationMode == net::ShotEvaluationMode::SeenPosition &&
               hitView.activeRuleLabel == "Seen Position" &&
               hitView.activeRuleExplanation ==
                   "host rewinds targets to the shooter's view.",
           "shot diagnostics should expose the active rule label and explanation for the authoritative shot study");
    expect(hitView.authoritativeHit &&
               hitView.outcomeLabel == "authoritative_hit",
           "shot diagnostics should preserve authoritative hit outcomes");

    const auto lines = model.summaryLines();
    expect(contains(lines, "Shot Viewed 1420ms | Rewind 1100ms"),
           "diagnostics summaries should expose the viewed and rewind times for studied shots");
    expect(contains(lines, "Shot State rewound_state | Rule Seen Position"),
           "diagnostics summaries should expose the evaluated state and authoritative rule label");
    expect(contains(lines, "Shot Outcome authoritative_hit | host rewinds targets to the shooter's view."),
           "diagnostics summaries should expose authoritative hit or miss outcomes alongside the rule explanation");

    model.recordShotEvaluation(net::ShotEvaluationRecord{
        1'800'000u,
        1'800'000u,
        net::ShotEvaluationStateView::LiveState,
        net::ShotEvaluationMode::LivePosition,
        false
    });
    const net::ShotEvaluationViewState missView = model.shotEvaluationView();
    expect(!missView.authoritativeHit &&
               missView.outcomeLabel == "authoritative_miss" &&
               missView.evaluatedStateLabel == "live_state" &&
               missView.activeRuleLabel == "Live Position",
           "shot diagnostics should also preserve miss outcomes and live-state evaluations");
}

void testStudyViewAssemblesAuthorityPredictionSpectatorAndPaneFacts() {
    net::DiagnosticsModel model(7u);
    model.recordShotEvaluation(net::ShotEvaluationRecord{
        1'420'000u,
        1'100'000u,
        net::ShotEvaluationStateView::RewoundState,
        net::ShotEvaluationMode::SeenPosition,
        true
    });

    net::DiagnosticsStudyRecord record;
    record.participantState.presence = sim::SessionPresence::Connected;
    record.participantState.team = sim::TeamId::Spectator;
    record.participantState.participation = sim::ParticipationState::Spectating;
    record.paneView.slot = sim::PaneSlot::Left;
    record.paneView.mode = sim::PaneViewMode::SpectatorFollowThirdPerson;
    record.paneView.focused = true;
    record.paneView.followTargetActorId = 1000;
    record.roster.push_back(makeRosterEntry(1000, sim::TeamId::Attacker, true, "BOT 1000"));
    record.cadence = sim::TimingCadence{60u, 20u, 60u};
    record.authoritativeTime = sim::AuthoritativeTime{18'244u, 304'066'666u, 303'900'000u};
    record.ackedInputSeq = 119u;
    record.reconciliationStrategy = net::DiagnosticsReconciliationStrategyView::Smooth;
    record.correctionMode = net::DiagnosticsCorrectionModeView::Snap;
    record.correctionMagnitude = 1.5f;
    record.replayedCommandCount = 2u;
    record.pendingInputCount = 3u;
    record.smoothWindowMs = 250u;
    record.shotEvaluationMode = net::ShotEvaluationMode::SeenPosition;
    record.canReturnToCharacter = true;
    model.recordStudyState(record);

    const net::DiagnosticsStudyViewState view = model.studyView();
    expect(view.authority.available &&
               view.authority.authoritativeTime.serverTick == 18'244u &&
               view.authority.ackedInputSeq == 119u &&
               view.authority.activeRuleLabel == "Seen Position" &&
               view.authority.activeRuleExplanation ==
                   "host rewinds targets to the shooter's view.",
           "typed study diagnostics should preserve authoritative timing and the active shot rule");
    expect(view.prediction.available &&
               view.prediction.pendingInputCount == 3u &&
               view.prediction.replayedCommandCount == 2u &&
               view.prediction.reconciliationStrategyLabel == "smooth" &&
               view.prediction.correctionModeLabel == "snap" &&
               view.prediction.correctionMagnitude == 1.5f,
           "typed study diagnostics should preserve pending-input and reconciliation facts");
    expect(view.pane.available &&
               view.pane.slot == sim::PaneSlot::Left &&
               view.pane.focused &&
               view.pane.boundActorId == 1000 &&
               view.pane.bindingLabel == "BOT 1000" &&
               view.pane.paneLabel == "LEFT: Follow BOT 1000",
           "typed study diagnostics should preserve pane labels and bound-actor identity");
    expect(view.spectator.available &&
               view.spectator.sessionSpectator &&
               view.spectator.paneObservation &&
               view.spectator.canReturnToCharacter &&
               view.spectator.followTargetActorId == 1000 &&
               view.spectator.followTargetLabel == "BOT 1000",
           "typed study diagnostics should preserve spectator follow-target state and returnability");
    expect(view.shot.available &&
               view.shot.evaluatedStateLabel == "rewound_state" &&
               view.shot.outcomeLabel == "authoritative_hit",
           "typed study diagnostics should surface the latest authoritative shot breakdown");

    const auto lines = model.summaryLines();
    expect(contains(lines, "Authority Tick 18244 | Ack 119 | Rule Seen Position") &&
               contains(lines, "Prediction Pending 3 | Replayed 2 | Strategy smooth | Correction snap") &&
               contains(lines, "Pane LEFT: Follow BOT 1000 | Focused | Follow Third Person"),
           "diagnostics summaries should reflect the typed authority, prediction, and pane facts");
}

void testStudyViewReplacesStaleFocusAndRuleFactsOnLaterUpdate() {
    net::DiagnosticsModel model(7u);

    net::DiagnosticsStudyRecord first;
    first.participantState.presence = sim::SessionPresence::Connected;
    first.participantState.team = sim::TeamId::Attacker;
    first.participantState.participation = sim::ParticipationState::Playing;
    first.participantState.control = sim::ControlBinding{sim::ControlBindingKind::Actor, 7};
    first.paneView.slot = sim::PaneSlot::Left;
    first.paneView.mode = sim::PaneViewMode::PlayerControlled;
    first.paneView.focused = true;
    first.roster.push_back(makeRosterEntry(7, sim::TeamId::Attacker, false, "Alpha"));
    first.cadence = sim::TimingCadence{60u, 20u, 60u};
    first.authoritativeTime = sim::AuthoritativeTime{900u, 15'000'000u, 14'900'000u};
    first.ackedInputSeq = 41u;
    first.reconciliationStrategy = net::DiagnosticsReconciliationStrategyView::Snap;
    first.correctionMode = net::DiagnosticsCorrectionModeView::Snap;
    first.pendingInputCount = 4u;
    first.replayedCommandCount = 4u;
    first.shotEvaluationMode = net::ShotEvaluationMode::SeenPosition;
    model.recordStudyState(first);

    net::DiagnosticsStudyRecord second;
    second.participantState.presence = sim::SessionPresence::Connected;
    second.participantState.team = sim::TeamId::Attacker;
    second.participantState.participation = sim::ParticipationState::Playing;
    second.participantState.control = sim::ControlBinding{sim::ControlBindingKind::Actor, 7};
    second.paneView.slot = sim::PaneSlot::Right;
    second.paneView.mode = sim::PaneViewMode::SpectatorFollowFirstPerson;
    second.paneView.focused = false;
    second.paneView.followTargetActorId = 1001;
    second.roster.push_back(makeRosterEntry(7, sim::TeamId::Attacker, false, "Alpha"));
    second.roster.push_back(makeRosterEntry(1001, sim::TeamId::Defender, true, "Observer Target"));
    second.cadence = sim::TimingCadence{60u, 20u, 60u};
    second.authoritativeTime = sim::AuthoritativeTime{901u, 15'016'667u, 15'000'000u};
    second.ackedInputSeq = 42u;
    second.reconciliationStrategy = net::DiagnosticsReconciliationStrategyView::Smooth;
    second.correctionMode = net::DiagnosticsCorrectionModeView::None;
    second.pendingInputCount = 0u;
    second.replayedCommandCount = 1u;
    second.smoothWindowMs = 125u;
    second.shotEvaluationMode = net::ShotEvaluationMode::LivePosition;
    second.canReturnToCharacter = true;
    model.recordStudyState(second);

    const net::DiagnosticsStudyViewState view = model.studyView();
    expect(view.authority.authoritativeTime.serverTick == 901u &&
               view.authority.ackedInputSeq == 42u &&
               view.authority.shotEvaluationMode == net::ShotEvaluationMode::LivePosition &&
               view.authority.activeRuleLabel == "Live Position",
           "later study updates should replace stale authoritative rule facts");
    expect(view.prediction.reconciliationStrategy == net::DiagnosticsReconciliationStrategyView::Smooth &&
               view.prediction.correctionMode == net::DiagnosticsCorrectionModeView::None &&
               view.prediction.pendingInputCount == 0u &&
               view.prediction.reconciliationStrategyLabel == "smooth" &&
               view.prediction.correctionModeLabel == "none",
           "later study updates should replace stale reconciliation and pending-input facts");
    expect(view.pane.slot == sim::PaneSlot::Right &&
               !view.pane.focused &&
               view.pane.boundActorId == 1001 &&
               view.pane.paneLabel == "RIGHT: Follow Observer Target" &&
               view.pane.focusLabel == "Background",
           "later study updates should replace stale pane focus and binding labels");
    expect(view.spectator.available &&
               !view.spectator.sessionSpectator &&
               view.spectator.paneObservation &&
               view.spectator.followTargetLabel == "Observer Target",
           "later study updates should preserve pane-local observation without reviving stale session spectator state");
}

void testRuntimeControlChangesTargetedLinkWithoutRestart() {
    net::UdpSocket client;
    net::UdpSocket server;
    expect(client.bind({"127.0.0.1", 0}), "client socket should bind");
    expect(server.bind({"127.0.0.1", 0}), "server socket should bind");

    net::ProxyConfig config;
    config.serverEndpoint = {"127.0.0.1", server.localPort()};
    net::ProxyRuntime proxy(config);
    expect(proxy.start(), "proxy should start");

    net::DiagnosticsModel model(1u);
    model.setBaseDelayMs(true, 50.0f);
    model.applyControl(&proxy, true);

    expect(client.sendTo({"127.0.0.1", proxy.clientListenPort()},
                         net::serializePacket(makeCommandPacket(1u, 1u))),
           "client should send through proxy");

    waitForProxyReceived(&proxy, true, 1u, 1'000'000u);
    expect(drainPacketsOfKind(&server, net::PacketKind::CommandBundle).empty(),
           "manipulated packets should remain queued until the applied delay elapses");

    proxy.tick(1'049'000u);
    expect(drainPacketsOfKind(&server, net::PacketKind::CommandBundle).empty(),
           "manipulated packets should not forward before the host control delay threshold");

    proxy.tick(1'050'000u);
    const auto packets = waitForPacketsOfKind(&server, net::PacketKind::CommandBundle, 1u);
    expect(packets.size() == 1u && packets.front().header.seq == 1u,
           "targeted host control should affect relay timing without restarting proxy");
}

void testStatsRefreshTracksQueueDepthAndForwardCounts() {
    net::UdpSocket client;
    net::UdpSocket server;
    expect(client.bind({"127.0.0.1", 0}), "client socket should bind");
    expect(server.bind({"127.0.0.1", 0}), "server socket should bind");

    net::ProxyConfig config;
    config.serverEndpoint = {"127.0.0.1", server.localPort()};
    net::ProxyRuntime proxy(config);
    expect(proxy.start(), "proxy should start");

    net::DiagnosticsModel model(1u);
    model.setBaseDelayMs(true, 40.0f);
    model.applyControl(&proxy, true);

    expect(client.sendTo({"127.0.0.1", proxy.clientListenPort()},
                         net::serializePacket(makeCommandPacket(2u, 1u))),
           "client should send delayed packet");

    waitForProxyReceived(&proxy, true, 1u, 2'000'000u);
    model.refreshFromProxy(proxy);
    expect(model.linkStats(true).receivedPackets == 1u,
           "diagnostics model should mirror proxy receive counts");
    expect(model.linkStats(true).forwardedPackets == 0u,
           "diagnostics model should report zero forwards while packet is queued");
    expect(model.linkStats(true).queuedPackets == 1u,
           "diagnostics model should report queued packet depth for the targeted link");

    proxy.tick(2'040'000u);
    waitForPacketsOfKind(&server, net::PacketKind::CommandBundle, 1u);
    model.refreshFromProxy(proxy);
    expect(model.linkStats(true).forwardedPackets == 1u,
           "diagnostics model should update forwarded counts after relay");
    expect(model.linkStats(true).queuedPackets == 0u,
           "diagnostics model should clear queue depth after relay completes");
}

void testSummaryLinesExposeLiveConfigAndQueueChanges() {
    net::UdpSocket client;
    net::UdpSocket server;
    expect(client.bind({"127.0.0.1", 0}), "client socket should bind");
    expect(server.bind({"127.0.0.1", 0}), "server socket should bind");

    net::ProxyConfig config;
    config.serverEndpoint = {"127.0.0.1", server.localPort()};
    net::ProxyRuntime proxy(config);
    expect(proxy.start(), "proxy should start");

    net::DiagnosticsModel model(1u);
    model.setBaseDelayMs(true, 25.0f);
    model.setReorderPct(true, 30.0f);
    model.setLossPct(false, 15.0f);
    model.applyControl(&proxy, false);
    model.applyControl(&proxy, true);

    expect(client.sendTo({"127.0.0.1", proxy.clientListenPort()},
                         net::serializePacket(makeCommandPacket(3u, 1u))),
           "client should send diagnostics packet");

    waitForProxyReceived(&proxy, true, 1u, 3'000'000u);
    model.refreshFromProxy(proxy);

    const auto lines = model.summaryLines();
    expect(contains(lines, "Delay 25ms"), "summary lines should expose configured delay");
    expect(contains(lines, "Loss 15%"), "summary lines should expose configured loss");
    expect(contains(lines, "Reorder 30%"), "summary lines should expose configured reorder");
    expect(contains(lines, "Queue 1"), "summary lines should expose live queue depth changes");
}

}  // namespace

int main() {
    try {
        testLocalNetworkSettingsMirrorLatencyAndLossAcrossDirections();
        testDirectionalProxyOverridesSurviveAuthoritativeSync();
        testLocalNetworkSettingsBuildUnifiedRuntimeParamRequests();
        testBotTargetBuildsBotScopedRuntimeParamRequests();
        testRuntimeParameterViewKeepsConsoleAndPanelStateSynchronized();
        testLocalNetworkAuthoritativeFeedbackConsumesApplyResults();
        testLocalNetworkAuthoritativeFeedbackConsumesSnapshots();
        testStagedApplyFeedbackAppearsOnBothSummaries();
        testShotEvaluationViewExposesRuleTimingAndOutcome();
        testStudyViewAssemblesAuthorityPredictionSpectatorAndPaneFacts();
        testStudyViewReplacesStaleFocusAndRuleFactsOnLaterUpdate();
        testRuntimeControlChangesTargetedLinkWithoutRestart();
        testStatsRefreshTracksQueueDepthAndForwardCounts();
        testSummaryLinesExposeLiveConfigAndQueueChanges();
        std::cout << "DiagnosticsModelTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "DiagnosticsModelTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
