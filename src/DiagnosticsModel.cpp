#include "net/DiagnosticsModel.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "net/SessionDiscovery.hpp"
#include "net/TransportArtifactAdapter.hpp"

namespace net {
namespace {

float clampNonNegative(float value) {
    return std::max(0.0f, value);
}

float clampPercent(float value) {
    return std::clamp(value, 0.0f, 100.0f);
}

int rounded(float value) {
    return static_cast<int>(std::lround(static_cast<double>(value)));
}

std::string formatConfigLine(const char* label, const ProxyLinkConfig& config) {
    std::ostringstream line;
    line << label
         << " Delay " << rounded(config.baseDelayMs) << "ms"
         << " | Jitter " << rounded(config.jitterMs) << "ms"
         << " | Loss " << rounded(config.lossPct) << "%"
         << " | Dup " << rounded(config.duplicatePct) << "%"
         << " | Reorder " << rounded(config.reorderPct) << "%";
    return line.str();
}

std::string formatStatsLine(const char* label, const ProxyStats& stats) {
    std::ostringstream line;
    line << label
         << " Rx " << stats.receivedPackets
         << " | Fwd " << stats.forwardedPackets
         << " | Drop " << stats.droppedPackets
         << " | Dup " << stats.duplicatedPackets
         << " | Reorder " << stats.reorderedPackets
         << " | Queue " << stats.queuedPackets;
    return line.str();
}

std::string formatLocalNetworkLine(const char* label, const LocalNetworkSettings& settings) {
    std::ostringstream line;
    line << label
         << " Latency " << rounded(settings.latencyMs) << "ms"
         << " | Loss " << rounded(settings.lossPct) << "%";
    return line.str();
}

std::string targetDescription(std::uint16_t targetId) {
    if (targetId == 0u) {
        return "No transport target selected";
    }

    std::ostringstream line;
    line << "Applies to "
         << (isBotTransportTargetId(targetId) ? "bot " : "player ")
         << targetId;
    return line.str();
}

std::string formatBoundary(sim::StagedApplyBoundary boundary) {
    switch (boundary) {
        case sim::StagedApplyBoundary::NextTick:
            return "next_tick";
        case sim::StagedApplyBoundary::NextSnapshot:
            return "next_snapshot";
        case sim::StagedApplyBoundary::NextSessionRestart:
            return "next_session_restart";
    }
    return "next_tick";
}

std::vector<std::string> runtimeParameterSummaryLines(
    const RuntimeParameterViewState& view) {
    if (!view.available) {
        return {};
    }

    std::vector<std::string> lines;
    lines.reserve(3u);
    lines.push_back(formatLocalNetworkLine(
        "Requested",
        LocalNetworkSettings{view.requestedLatencyMs, view.requestedLossPct}));
    lines.push_back(formatLocalNetworkLine(
        view.hasAuthoritativeValues ? "Applied" : "Awaiting",
        LocalNetworkSettings{view.effectiveLatencyMs, view.effectiveLossPct}));

    std::string status = "Status pending_authoritative_apply";
    if (view.hasApplyResult) {
        const std::string message = view.statusMessage.empty()
            ? std::string(view.applied ? "applied" : "rejected")
            : view.statusMessage;
        status = "Status " + message +
                 " | Boundary " + formatBoundary(view.stagedApplyBoundary);
    } else if (view.hasAuthoritativeValues) {
        status = "Status authoritative_snapshot";
    }

    lines.push_back(status);
    return lines;
}

const char* shotEvaluationStateLabel(ShotEvaluationStateView state) {
    switch (state) {
        case ShotEvaluationStateView::LiveState:
            return "live_state";
        case ShotEvaluationStateView::RewoundState:
            return "rewound_state";
    }
    return "live_state";
}

std::string shotOutcomeLabel(bool hit) {
    return hit ? "authoritative_hit" : "authoritative_miss";
}

std::string formatTimeUs(std::uint64_t valueUs) {
    std::ostringstream line;
    line << rounded(static_cast<float>(valueUs) / 1000.0f) << "ms";
    return line.str();
}

std::vector<std::string> shotEvaluationSummaryLines(const ShotEvaluationViewState& view) {
    if (!view.available) {
        return {};
    }

    std::vector<std::string> lines;
    lines.reserve(3u);
    lines.push_back("Shot Viewed " + formatTimeUs(view.viewedTimeUs) +
                    " | Rewind " + formatTimeUs(view.rewindTimeUs));
    lines.push_back("Shot State " + view.evaluatedStateLabel +
                    " | Rule " + view.activeRuleLabel);
    lines.push_back("Shot Outcome " + view.outcomeLabel +
                    " | " + view.activeRuleExplanation);
    return lines;
}

const char* reconciliationStrategyLabel(DiagnosticsReconciliationStrategyView strategy) {
    switch (strategy) {
        case DiagnosticsReconciliationStrategyView::Snap:
            return "snap";
        case DiagnosticsReconciliationStrategyView::Smooth:
            return "smooth";
    }
    return "smooth";
}

const char* correctionModeLabel(DiagnosticsCorrectionModeView mode) {
    switch (mode) {
        case DiagnosticsCorrectionModeView::None:
            return "none";
        case DiagnosticsCorrectionModeView::Smooth:
            return "smooth";
        case DiagnosticsCorrectionModeView::Snap:
            return "snap";
    }
    return "none";
}

bool isSpectatorPaneMode(sim::PaneViewMode mode) {
    return mode == sim::PaneViewMode::SpectatorFreeFly ||
           mode == sim::PaneViewMode::SpectatorFollowFirstPerson ||
           mode == sim::PaneViewMode::SpectatorFollowThirdPerson;
}

std::string slotLabel(sim::PaneSlot slot) {
    return slot == sim::PaneSlot::Right ? "RIGHT" : "LEFT";
}

std::string focusLabel(bool focused) {
    return focused ? "Focused" : "Background";
}

std::string paneModeLabel(sim::PaneViewMode mode) {
    switch (mode) {
        case sim::PaneViewMode::PlayerControlled:
            return "Player";
        case sim::PaneViewMode::SpectatorFreeFly:
            return "Free Fly";
        case sim::PaneViewMode::SpectatorFollowFirstPerson:
            return "Follow First Person";
        case sim::PaneViewMode::SpectatorFollowThirdPerson:
            return "Follow Third Person";
        case sim::PaneViewMode::ReplayCamera:
            return "Replay";
    }
    return "Player";
}

std::string actorLabel(const std::vector<sim::RosterEntry>& roster, int actorId) {
    if (actorId < 0) {
        return "Unbound";
    }

    const auto it = std::find_if(roster.begin(),
                                 roster.end(),
                                 [actorId](const sim::RosterEntry& entry) {
                                     return entry.actorId == actorId;
                                 });
    if (it == roster.end()) {
        return "Actor " + std::to_string(actorId);
    }
    if (!it->displayName.empty()) {
        return it->displayName;
    }
    return it->isBot ? "BOT " + std::to_string(actorId)
                     : "Player " + std::to_string(actorId);
}

std::vector<std::string> studySummaryLines(const DiagnosticsStudyViewState& view) {
    std::vector<std::string> lines;
    if (view.authority.available) {
        std::ostringstream line;
        line << "Authority Tick " << view.authority.authoritativeTime.serverTick
             << " | Ack " << view.authority.ackedInputSeq
             << " | Rule " << view.authority.activeRuleLabel;
        lines.push_back(line.str());
    }
    if (view.prediction.available) {
        std::ostringstream line;
        line << "Prediction Pending " << view.prediction.pendingInputCount
             << " | Replayed " << view.prediction.replayedCommandCount
             << " | Strategy " << view.prediction.reconciliationStrategyLabel
             << " | Correction " << view.prediction.correctionModeLabel;
        lines.push_back(line.str());
    }
    if (view.pane.available) {
        lines.push_back("Pane " + view.pane.paneLabel +
                        " | " + view.pane.focusLabel +
                        " | " + view.pane.modeLabel);
    }
    if (view.spectator.available) {
        lines.push_back("Spectator Follow " + view.spectator.followTargetLabel +
                        " | Team " + view.spectator.participantTeamLabel);
    }
    return lines;
}

}  // namespace

DiagnosticsModel::DiagnosticsModel(std::uint16_t targetPeerId)
    : targetPeerId_(targetPeerId) {}

void DiagnosticsModel::setTargetPeerId(std::uint16_t peerId) {
    if (targetPeerId_ == peerId) {
        return;
    }

    targetPeerId_ = peerId;
    authoritativeLocalNetworkSettings_ = {};
    lastLocalNetworkApplyResult_ = {};
    hasAuthoritativeLocalNetworkSettings_ = false;
    hasLocalNetworkApplyResult_ = false;
    clearDirectionalOverrides();
    syncLocalNetworkSettings();
}

std::uint16_t DiagnosticsModel::targetPeerId() const {
    return targetPeerId_;
}

RuntimeParamScope DiagnosticsModel::targetScope() const {
    return runtimeParamScopeForTargetId(targetPeerId_);
}

const ProxyLinkConfig& DiagnosticsModel::linkConfig(bool upstream) const {
    return upstream ? upstreamConfig_ : downstreamConfig_;
}

const ProxyStats& DiagnosticsModel::linkStats(bool upstream) const {
    return upstream ? upstreamStats_ : downstreamStats_;
}

void DiagnosticsModel::setBaseDelayMs(bool upstream, float value) {
    mutableLinkConfig(upstream)->baseDelayMs = clampNonNegative(value);
    if (upstream) {
        upstreamBaseDelayOverridden_ = true;
    } else {
        downstreamBaseDelayOverridden_ = true;
    }
}

void DiagnosticsModel::setJitterMs(bool upstream, float value) {
    mutableLinkConfig(upstream)->jitterMs = clampNonNegative(value);
}

void DiagnosticsModel::setLossPct(bool upstream, float value) {
    mutableLinkConfig(upstream)->lossPct = clampPercent(value);
    if (upstream) {
        upstreamLossOverridden_ = true;
    } else {
        downstreamLossOverridden_ = true;
    }
}

void DiagnosticsModel::setDuplicatePct(bool upstream, float value) {
    mutableLinkConfig(upstream)->duplicatePct = clampPercent(value);
}

void DiagnosticsModel::setReorderPct(bool upstream, float value) {
    mutableLinkConfig(upstream)->reorderPct = clampPercent(value);
}

const LocalNetworkSettings& DiagnosticsModel::localNetworkSettings() const {
    return localNetworkSettings_;
}

const LocalNetworkSettings& DiagnosticsModel::authoritativeLocalNetworkSettings() const {
    return hasAuthoritativeLocalNetworkSettings_
        ? authoritativeLocalNetworkSettings_
        : localNetworkSettings_;
}

bool DiagnosticsModel::hasAuthoritativeLocalNetworkSettings() const {
    return hasAuthoritativeLocalNetworkSettings_;
}

void DiagnosticsModel::setLocalLatencyMs(float value) {
    localNetworkSettings_.latencyMs = clampNonNegative(value);
    clearBaseDelayOverrides();
    syncLocalNetworkSettings();
}

void DiagnosticsModel::setLocalLossPct(float value) {
    localNetworkSettings_.lossPct = clampPercent(value);
    clearLossOverrides();
    syncLocalNetworkSettings();
}

void DiagnosticsModel::syncAuthoritativeLocalNetworkSettings(std::uint16_t latencyMs,
                                                             std::uint8_t lossPct) {
    const bool pendingLocalOverride =
        hasAuthoritativeLocalNetworkSettings_ &&
        (localNetworkSettings_.latencyMs != authoritativeLocalNetworkSettings_.latencyMs ||
         localNetworkSettings_.lossPct != authoritativeLocalNetworkSettings_.lossPct);
    authoritativeLocalNetworkSettings_.latencyMs = static_cast<float>(latencyMs);
    authoritativeLocalNetworkSettings_.lossPct = static_cast<float>(lossPct);
    hasAuthoritativeLocalNetworkSettings_ = true;
    if (!pendingLocalOverride ||
        (localNetworkSettings_.latencyMs == authoritativeLocalNetworkSettings_.latencyMs &&
         localNetworkSettings_.lossPct == authoritativeLocalNetworkSettings_.lossPct)) {
        localNetworkSettings_ = authoritativeLocalNetworkSettings_;
    }
    syncLocalNetworkSettings();
}

ProxyControl DiagnosticsModel::buildControl(bool upstream) const {
    ProxyControl control;
    control.targetPeerId = targetPeerId_;
    control.upstream = upstream;
    control.config = linkConfig(upstream);
    return control;
}

std::vector<RuntimeParamChangeRequest> DiagnosticsModel::buildLocalNetworkRequests() const {
    if (targetPeerId_ == 0u) {
        return {};
    }

    const auto targetId = static_cast<std::int32_t>(targetPeerId_);
    const RuntimeParamScope scope = targetScope();
    std::vector<RuntimeParamChangeRequest> requests;
    requests.reserve(2);
    requests.push_back(RuntimeParamChangeRequest{
        scope,
        targetId,
        runtimeParamKeyForTarget(targetPeerId_, "latency_ms"),
        localNetworkSettings_.latencyMs
    });
    requests.push_back(RuntimeParamChangeRequest{
        scope,
        targetId,
        runtimeParamKeyForTarget(targetPeerId_, "loss_pct"),
        localNetworkSettings_.lossPct
    });
    return requests;
}

void DiagnosticsModel::applyControl(TransportArtifactAdapter* proxy, bool upstream) const {
    if (proxy == nullptr) {
        return;
    }

    const ProxyControl control = buildControl(upstream);
    if (control.targetPeerId == 0u) {
        proxy->setDefaultLinkConfig(control.upstream, control.config);
    } else {
        proxy->setPeerLinkConfig(control.targetPeerId, control.upstream, control.config);
    }
}

bool DiagnosticsModel::consumeRuntimeParamSnapshot(const RuntimeParamSnapshot& snapshot) {
    if (!applyLocalNetworkValue(snapshot.scope,
                                snapshot.targetId,
                                snapshot.key,
                                snapshot.value,
                                &authoritativeLocalNetworkSettings_)) {
        return false;
    }

    hasAuthoritativeLocalNetworkSettings_ = true;
    localNetworkSettings_ = authoritativeLocalNetworkSettings_;
    syncLocalNetworkSettings();
    return true;
}

bool DiagnosticsModel::consumeRuntimeParamApplyResult(const RuntimeParamApplyResult& result) {
    LocalNetworkSettings nextAuthoritative = authoritativeLocalNetworkSettings_;
    if (!applyLocalNetworkValue(result.scope,
                                result.targetId,
                                result.key,
                                result.value,
                                &nextAuthoritative)) {
        return false;
    }

    hasLocalNetworkApplyResult_ = true;
    lastLocalNetworkApplyResult_ = result;
    if (result.applied) {
        authoritativeLocalNetworkSettings_ = nextAuthoritative;
        hasAuthoritativeLocalNetworkSettings_ = true;
        localNetworkSettings_ = authoritativeLocalNetworkSettings_;
        syncLocalNetworkSettings();
    }
    return true;
}

void DiagnosticsModel::refreshFromProxy(const TransportArtifactAdapter& proxy) {
    if (targetPeerId_ == 0u) {
        upstreamStats_ = proxy.aggregateStats(true);
        downstreamStats_ = proxy.aggregateStats(false);
        return;
    }

    if (!transportTargetUsesProxyLink(targetPeerId_)) {
        upstreamStats_ = {};
        downstreamStats_ = {};
        return;
    }

    upstreamStats_ = proxy.peerStats(targetPeerId_, true);
    downstreamStats_ = proxy.peerStats(targetPeerId_, false);
}

RuntimeParameterViewState DiagnosticsModel::runtimeParameterView() const {
    RuntimeParameterViewState view;
    view.available = targetPeerId_ != 0u;
    view.requestedLatencyMs = localNetworkSettings_.latencyMs;
    view.requestedLossPct = localNetworkSettings_.lossPct;
    view.hasAuthoritativeValues = hasAuthoritativeLocalNetworkSettings_;
    const LocalNetworkSettings effective = authoritativeLocalNetworkSettings();
    view.effectiveLatencyMs = effective.latencyMs;
    view.effectiveLossPct = effective.lossPct;
    view.hasApplyResult = hasLocalNetworkApplyResult_;
    view.applied = lastLocalNetworkApplyResult_.applied;
    view.stagedApplyBoundary = lastLocalNetworkApplyResult_.stagedApplyBoundary;
    view.statusMessage = lastLocalNetworkApplyResult_.message;
    return view;
}

void DiagnosticsModel::recordShotEvaluation(const ShotEvaluationRecord& record) {
    shotEvaluationView_.available = true;
    shotEvaluationView_.viewedTimeUs = record.viewedTimeUs;
    shotEvaluationView_.rewindTimeUs = record.rewindTimeUs;
    shotEvaluationView_.evaluatedState = record.evaluatedState;
    shotEvaluationView_.evaluatedStateLabel = shotEvaluationStateLabel(record.evaluatedState);
    shotEvaluationView_.shotEvaluationMode = record.shotEvaluationMode;
    shotEvaluationView_.activeRuleLabel = toString(record.shotEvaluationMode);
    shotEvaluationView_.activeRuleExplanation = shotEvaluationModeExplanation(record.shotEvaluationMode);
    shotEvaluationView_.authoritativeHit = record.authoritativeHit;
    shotEvaluationView_.outcomeLabel = shotOutcomeLabel(record.authoritativeHit);
    studyView_.shot = shotEvaluationView_;
}

ShotEvaluationViewState DiagnosticsModel::shotEvaluationView() const {
    return shotEvaluationView_;
}

void DiagnosticsModel::recordStudyState(const DiagnosticsStudyRecord& record) {
    studyView_.authority.available = true;
    studyView_.authority.cadence = record.cadence;
    studyView_.authority.authoritativeTime = record.authoritativeTime;
    studyView_.authority.ackedInputSeq = record.ackedInputSeq;
    studyView_.authority.shotEvaluationMode = record.shotEvaluationMode;
    studyView_.authority.activeRuleLabel = toString(record.shotEvaluationMode);
    studyView_.authority.activeRuleExplanation =
        shotEvaluationModeExplanation(record.shotEvaluationMode);

    studyView_.prediction.available = true;
    studyView_.prediction.reconciliationStrategy = record.reconciliationStrategy;
    studyView_.prediction.correctionMode = record.correctionMode;
    studyView_.prediction.reconciliationStrategyLabel =
        reconciliationStrategyLabel(record.reconciliationStrategy);
    studyView_.prediction.correctionModeLabel =
        correctionModeLabel(record.correctionMode);
    studyView_.prediction.correctionMagnitude = record.correctionMagnitude;
    studyView_.prediction.replayedCommandCount = record.replayedCommandCount;
    studyView_.prediction.pendingInputCount = record.pendingInputCount;
    studyView_.prediction.smoothWindowMs = record.smoothWindowMs;

    studyView_.pane.available = true;
    studyView_.pane.slot = record.paneView.slot;
    studyView_.pane.focused = record.paneView.focused;
    studyView_.pane.mode = record.paneView.mode;
    studyView_.pane.slotLabel = slotLabel(record.paneView.slot);
    studyView_.pane.focusLabel = focusLabel(record.paneView.focused);
    studyView_.pane.modeLabel = paneModeLabel(record.paneView.mode);
    studyView_.pane.boundActorId = isSpectatorPaneMode(record.paneView.mode)
        ? record.paneView.followTargetActorId
        : record.participantState.control.actorId;
    studyView_.pane.bindingLabel = actorLabel(record.roster, studyView_.pane.boundActorId);
    studyView_.pane.paneLabel = studyView_.pane.slotLabel + ": " +
        (isSpectatorPaneMode(record.paneView.mode)
             ? "Follow " + studyView_.pane.bindingLabel
             : studyView_.pane.bindingLabel);

    studyView_.spectator.available =
        record.participantState.participation == sim::ParticipationState::Spectating ||
        isSpectatorPaneMode(record.paneView.mode);
    studyView_.spectator.sessionSpectator =
        record.participantState.participation == sim::ParticipationState::Spectating;
    studyView_.spectator.paneObservation = isSpectatorPaneMode(record.paneView.mode);
    studyView_.spectator.canReturnToCharacter = record.canReturnToCharacter;
    studyView_.spectator.participantTeam = record.participantState.team;
    studyView_.spectator.followTargetActorId = record.paneView.followTargetActorId;
    studyView_.spectator.participantTeamLabel = sim::toString(record.participantState.team);
    studyView_.spectator.followTargetLabel =
        actorLabel(record.roster, record.paneView.followTargetActorId);

    studyView_.shot = shotEvaluationView_;
}

DiagnosticsStudyViewState DiagnosticsModel::studyView() const {
    return studyView_;
}

std::vector<std::string> DiagnosticsModel::shotEvaluationSummaryLines() const {
    return ::net::shotEvaluationSummaryLines(shotEvaluationView_);
}

std::vector<std::string> DiagnosticsModel::summaryLines() const {
    std::vector<std::string> lines;
    lines.reserve(10);
    lines.push_back(formatConfigLine("Up", upstreamConfig_));
    lines.push_back(formatStatsLine("Up", upstreamStats_));
    lines.push_back(formatConfigLine("Down", downstreamConfig_));
    lines.push_back(formatStatsLine("Down", downstreamStats_));
    const auto runtimeLines = runtimeParameterSummaryLines(runtimeParameterView());
    lines.insert(lines.end(), runtimeLines.begin(), runtimeLines.end());
    const auto shotLines = shotEvaluationSummaryLines();
    lines.insert(lines.end(), shotLines.begin(), shotLines.end());
    const auto studyLines = ::net::studySummaryLines(studyView_);
    lines.insert(lines.end(), studyLines.begin(), studyLines.end());
    return lines;
}

std::vector<std::string> DiagnosticsModel::localNetworkSummaryLines() const {
    std::vector<std::string> lines = {targetDescription(targetPeerId_)};
    const auto runtimeLines = runtimeParameterSummaryLines(runtimeParameterView());
    lines.insert(lines.end(), runtimeLines.begin(), runtimeLines.end());
    return lines;
}

ProxyLinkConfig* DiagnosticsModel::mutableLinkConfig(bool upstream) {
    return upstream ? &upstreamConfig_ : &downstreamConfig_;
}

bool DiagnosticsModel::applyLocalNetworkValue(RuntimeParamScope scope,
                                              std::int32_t targetId,
                                              const std::string& key,
                                              float value,
                                              LocalNetworkSettings* settings) {
    if (settings == nullptr ||
        targetPeerId_ == 0u ||
        scope != targetScope() ||
        targetId != static_cast<std::int32_t>(targetPeerId_)) {
        return false;
    }

    if (key == runtimeParamKeyForTarget(targetPeerId_, "latency_ms")) {
        settings->latencyMs = clampNonNegative(value);
        return true;
    }

    if (key == runtimeParamKeyForTarget(targetPeerId_, "loss_pct")) {
        settings->lossPct = clampPercent(value);
        return true;
    }

    return false;
}

void DiagnosticsModel::syncLocalNetworkSettings() {
    if (!upstreamBaseDelayOverridden_) {
        upstreamConfig_.baseDelayMs = localNetworkSettings_.latencyMs;
    }
    if (!downstreamBaseDelayOverridden_) {
        downstreamConfig_.baseDelayMs = localNetworkSettings_.latencyMs;
    }
    if (!upstreamLossOverridden_) {
        upstreamConfig_.lossPct = localNetworkSettings_.lossPct;
    }
    if (!downstreamLossOverridden_) {
        downstreamConfig_.lossPct = localNetworkSettings_.lossPct;
    }
}

void DiagnosticsModel::clearBaseDelayOverrides() {
    upstreamBaseDelayOverridden_ = false;
    downstreamBaseDelayOverridden_ = false;
}

void DiagnosticsModel::clearLossOverrides() {
    upstreamLossOverridden_ = false;
    downstreamLossOverridden_ = false;
}

void DiagnosticsModel::clearDirectionalOverrides() {
    clearBaseDelayOverrides();
    clearLossOverrides();
}

}  // namespace net
