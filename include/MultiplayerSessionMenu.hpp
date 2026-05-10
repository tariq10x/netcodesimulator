#pragma once

#include <raylib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "DisplayManager.hpp"
#include "MainMenu.hpp"
#include "TypographyService.hpp"
#include "net/SessionDiscovery.hpp"
#include "net/SessionLaunchConfig.hpp"
#include "net/UdpSocket.hpp"

class MultiplayerSessionMenu {
public:
    struct Result {
        GameMode mode{GameMode::MULTIPLAYER_SESSION};
        bool submitted{false};
        net::SessionLaunchConfig launchConfig{};
    };

    enum class JoinSubview : std::uint8_t {
        Browser = 0,
        DirectConnect = 1
    };

    enum class Step : std::uint8_t {
        Setup = 0,
        TeamChoice = 1
    };

private:
    enum class Field : std::uint8_t {
        None = 0,
        HostAddress = 1,
        ServerPort = 2,
        PlayerName = 3,
        SessionLabel = 4,
        TickRate = 5,
        SnapshotRate = 6,
        MaxHumanPlayers = 7,
        ShotEvaluationMode = 8,
        TotalBots = 9
    };

    struct TextField {
        Rectangle rect{};
        std::string label{};
        std::string* value{nullptr};
        Field field{Field::None};
    };

    static constexpr std::size_t kVisibleDiscoveryRowCount = 4u;
    static constexpr float kSetupFieldPanelX = 1920.0f / 2.0f - 320.0f;
    static constexpr float kSetupFieldWidth = 640.0f;
    static constexpr float kSetupFieldHeight = 50.0f;
    static constexpr float kHostSetupFieldStartY = 438.0f;
    static constexpr float kJoinDirectFieldStartY = 438.0f;
    static constexpr float kJoinBrowserPlayerFieldY = 780.0f;
    static constexpr float kSetupPreviewGapY = 28.0f;
    static constexpr float kSetupFieldColumnGapX = 20.0f;
    static constexpr float kHostHeaderY = 310.0f;
    static constexpr float kDiscoveryRowStartY = 410.0f;
    static constexpr float kDiscoveryRowHeight = 70.0f;
    static constexpr std::uint64_t kDiscoveryRefreshIntervalUs = 1'000'000u;

    net::SessionLaunchMode mode_{net::SessionLaunchMode::Join};
    net::SessionProductSurface surface_{net::SessionProductSurface::Multiplayer};
    Step step_{Step::Setup};
    JoinSubview joinSubview_{JoinSubview::Browser};
    bool hostAdvancedExpanded_{false};
    int selectedLevelSlot_{-1};
    std::uint8_t localParticipantCount_{net::kDefaultLocalParticipantCount};
    std::string hostAddressText_{"127.0.0.1"};
    std::string serverPortText_{std::to_string(net::kDefaultServerPort)};
    std::string playerNameText_{"player"};
    std::string preferredTeamText_{teamChoiceText(sim::TeamId::Defender)};
    std::string sessionLabelText_{};
    std::string tickRateText_{std::to_string(net::kDefaultSessionTickRateHz) + " Hz"};
    std::string snapshotRateText_{std::to_string(net::kDefaultHostedSnapshotRateHz) + " Hz"};
    std::string maxHumanPlayersText_{std::to_string(net::kDefaultHostedHumanPlayerCap)};
    std::string shotEvaluationModeText_{net::toString(net::ShotEvaluationMode::SeenPosition)};
    std::string totalBotCountText_{std::to_string(net::kDefaultHostedBotCount)};
    std::string validationError_{};
    std::string statusMessage_{};
    bool busy_{false};
    bool discoveryAutoScanPending_{false};
    std::uint32_t discoveryScanCount_{0u};
    std::uint64_t discoveryClockUs_{0u};
    std::uint64_t nextDiscoveryRefreshUs_{0u};
    std::uint16_t discoveryPort_{net::kDefaultSessionDiscoveryPort};
    int selectedDiscoveryIndex_{-1};
    std::string selectedDiscoveryHost_{};
    std::uint16_t selectedDiscoveryPort_{0u};
    Field activeField_{Field::None};
    bool clickReady_{false};
    Rectangle cornerBackButton_{40.0f, 56.0f, 180.0f, 48.0f};
    Rectangle launchButton_{};
    Rectangle refreshButton_{};
    Rectangle advancedButton_{};
    Rectangle browserSubviewButton_{};
    Rectangle directConnectSubviewButton_{};
    std::array<Rectangle, kVisibleDiscoveryRowCount> discoveryRowRects_{};
    std::array<Rectangle, 3> teamChoiceRects_{};
    sim::TeamId preferredTeam_{sim::TeamId::Defender};
    std::uint16_t tickRateHz_{net::kDefaultSessionTickRateHz};
    std::uint16_t snapshotRateHz_{net::kDefaultHostedSnapshotRateHz};
    net::ShotEvaluationMode shotEvaluationMode_{net::ShotEvaluationMode::SeenPosition};
    std::array<TextField, 9> fields_{};
    net::UdpSocket discoverySocket_{};
    net::SessionBrowserCache discoveryCache_{};

    static std::string trim(const std::string& value) {
        const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
            return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
        });
        const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
            return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
        }).base();
        return first >= last ? std::string{} : std::string(first, last);
    }

    static float setupFieldLabelOffsetY() {
        return TypographyTheme::style(TypographyStyleId::FieldLabel).lineHeight + 4.0f;
    }

    static float setupSingleColumnRowPitch() {
        return kSetupFieldHeight + setupFieldLabelOffsetY() + 12.0f;
    }

    static float setupHostAdvancedRowPitch() {
        return kSetupFieldHeight + setupFieldLabelOffsetY() + 10.0f;
    }

    static const char* teamChoiceText(sim::TeamId team) {
        switch (team) {
            case sim::TeamId::Attacker:
                return "Attackers";
            case sim::TeamId::Defender:
                return "Defenders";
            case sim::TeamId::Spectator:
                return "Spectator";
            default:
                return "Auto-Assign";
        }
    }

    static Color teamChoiceAccentColor(sim::TeamId team) {
        switch (team) {
            case sim::TeamId::Attacker:
                return Color{236, 82, 82, 255};
            case sim::TeamId::Defender:
                return Color{84, 148, 255, 255};
            case sim::TeamId::Spectator:
                return Color{180, 188, 205, 255};
            default:
                return Color{180, 188, 205, 255};
        }
    }

    static Rectangle centeredRect(const Rectangle& outer, float width, float height) {
        return Rectangle{
            outer.x + (outer.width - width) * 0.5f,
            outer.y + (outer.height - height) * 0.5f,
            width,
            height
        };
    }

    static void drawTeamCharacterGlyph(const Rectangle& icon, Color accent) {
        const Color ink = Fade(WHITE, 0.92f);
        const Vector2 head{icon.x + icon.width * 0.5f, icon.y + icon.height * 0.26f};
        const Rectangle torso{
            icon.x + icon.width * 0.33f,
            icon.y + icon.height * 0.42f,
            icon.width * 0.34f,
            icon.height * 0.34f
        };

        DrawCircleV(Vector2{icon.x + icon.width * 0.5f, icon.y + icon.height * 0.5f},
                    icon.width * 0.42f,
                    Fade(accent, 0.12f));
        DrawCircleV(head, icon.width * 0.12f, accent);
        DrawCircleLines(static_cast<int>(head.x), static_cast<int>(head.y), icon.width * 0.12f, ink);
        DrawRectangleRounded(torso, 0.42f, 8, Fade(accent, 0.88f));
        DrawRectangleRoundedLines(torso, 0.42f, 8, ink);
    }

    static void drawSpectatorGlyph(const Rectangle& icon, Color accent) {
        const Vector2 center{icon.x + icon.width * 0.5f, icon.y + icon.height * 0.5f};
        DrawCircleV(center, icon.width * 0.38f, Fade(accent, 0.1f));
        DrawCircleLines(static_cast<int>(center.x), static_cast<int>(center.y), icon.width * 0.28f, Fade(WHITE, 0.75f));
        DrawCircleLines(static_cast<int>(center.x), static_cast<int>(center.y), icon.width * 0.16f, accent);
        DrawCircleV(center, icon.width * 0.08f, accent);
    }

    static void drawTeamChoiceGlyph(sim::TeamId team, const Rectangle& icon) {
        const Color accent = teamChoiceAccentColor(team);
        if (team == sim::TeamId::Spectator) {
            drawSpectatorGlyph(icon, accent);
            return;
        }
        drawTeamCharacterGlyph(icon, accent);
    }

    static std::pair<std::uint16_t, std::uint16_t> balancedBotTeamCounts(
        std::uint16_t totalBots,
        sim::TeamId preferredTeam) {
        std::uint16_t attackerBots = static_cast<std::uint16_t>(totalBots / 2u);
        std::uint16_t defenderBots = static_cast<std::uint16_t>(totalBots / 2u);
        if ((totalBots % 2u) != 0u) {
            if (preferredTeam == sim::TeamId::Defender) {
                ++attackerBots;
            } else {
                ++defenderBots;
            }
        }
        return {attackerBots, defenderBots};
    }

    static std::string sessionLabelFallback(const std::string& sessionLabel,
                                            const std::string& playerName) {
        const std::string trimmedLabel = trim(sessionLabel);
        if (!trimmedLabel.empty()) {
            return trimmedLabel;
        }

        const std::string trimmedPlayerName = trim(playerName);
        return trimmedPlayerName.empty() ? std::string{"player"} : trimmedPlayerName;
    }

    void initializeLayout() {
        const float startY = kHostSetupFieldStartY;
        const float seedPitch = setupSingleColumnRowPitch();

        fields_[0] = TextField{{kSetupFieldPanelX, startY, kSetupFieldWidth, kSetupFieldHeight},
                               "Host Address",
                               &hostAddressText_,
                               Field::HostAddress};
        fields_[1] = TextField{{kSetupFieldPanelX,
                                startY + seedPitch,
                                kSetupFieldWidth,
                                kSetupFieldHeight},
                               "Port",
                               &serverPortText_,
                               Field::ServerPort};
        fields_[2] = TextField{{kSetupFieldPanelX,
                                startY + seedPitch * 2.0f,
                                kSetupFieldWidth,
                                kSetupFieldHeight},
                               "Player Name",
                               &playerNameText_,
                               Field::PlayerName};
        fields_[3] = TextField{{kSetupFieldPanelX,
                                startY + seedPitch * 3.0f,
                                kSetupFieldWidth,
                                kSetupFieldHeight},
                               "Session Label",
                               &sessionLabelText_,
                               Field::SessionLabel};
        fields_[4] = TextField{{kSetupFieldPanelX,
                                startY + seedPitch * 4.0f,
                                kSetupFieldWidth,
                                kSetupFieldHeight},
                               "Tick Rate",
                               &tickRateText_,
                               Field::TickRate};
        fields_[5] = TextField{{kSetupFieldPanelX,
                                startY + seedPitch * 5.0f,
                                kSetupFieldWidth,
                                kSetupFieldHeight},
                               "Snapshot Rate",
                               &snapshotRateText_,
                               Field::SnapshotRate};
        fields_[6] = TextField{{kSetupFieldPanelX,
                                startY + seedPitch * 6.0f,
                                kSetupFieldWidth,
                                kSetupFieldHeight},
                               "Human Player Cap",
                               &maxHumanPlayersText_,
                               Field::MaxHumanPlayers};
        fields_[7] = TextField{{kSetupFieldPanelX,
                                startY + seedPitch * 7.0f,
                                kSetupFieldWidth,
                                kSetupFieldHeight},
                               "Shot Rule",
                               &shotEvaluationModeText_,
                               Field::ShotEvaluationMode};
        fields_[8] = TextField{{kSetupFieldPanelX,
                                startY + seedPitch * 8.0f,
                                kSetupFieldWidth,
                                kSetupFieldHeight},
                               "Bots",
                               &totalBotCountText_,
                               Field::TotalBots};

        launchButton_ = Rectangle{1920.0f * 0.5f - 160.0f, 848.0f, 320.0f, 70.0f};
        refreshButton_ =
            Rectangle{kSetupFieldPanelX + kSetupFieldWidth - 180.0f, 280.0f, 180.0f, 48.0f};
        advancedButton_ =
            Rectangle{kSetupFieldPanelX + kSetupFieldWidth - 174.0f,
                      kHostHeaderY + 2.0f,
                      174.0f,
                      48.0f};
        browserSubviewButton_ = Rectangle{kSetupFieldPanelX, 280.0f, 220.0f, 48.0f};
        directConnectSubviewButton_ =
            Rectangle{kSetupFieldPanelX + 232.0f, 280.0f, 240.0f, 48.0f};

        for (std::size_t index = 0; index < discoveryRowRects_.size(); ++index) {
            discoveryRowRects_[index] = Rectangle{
                kSetupFieldPanelX,
                kDiscoveryRowStartY + static_cast<float>(index) * (kDiscoveryRowHeight + 14.0f),
                kSetupFieldWidth,
                kDiscoveryRowHeight
            };
        }

        const float choiceY = 390.0f;
        const float choiceWidth = 190.0f;
        for (std::size_t index = 0; index < teamChoiceRects_.size(); ++index) {
            teamChoiceRects_[index] = Rectangle{
                kSetupFieldPanelX + static_cast<float>(index) * (choiceWidth + 35.0f),
                choiceY,
                choiceWidth,
                250.0f
            };
        }
    }

    void syncShotEvaluationModeText() {
        shotEvaluationModeText_ = net::toString(shotEvaluationMode_);
    }

    void syncTickRateText() {
        tickRateText_ = std::to_string(tickRateHz_) + " Hz";
    }

    void syncSnapshotRateText() {
        snapshotRateText_ = std::to_string(snapshotRateHz_) + " Hz";
    }

    void ensureHumanPlayerCapAtLeastLocalParticipants() {
        std::uint16_t parsedCap = 0u;
        if (!net::parseSessionHumanPlayerCap(maxHumanPlayersText_, &parsedCap) ||
            parsedCap < localParticipantCount_) {
            maxHumanPlayersText_ = std::to_string(
                std::max<std::uint16_t>(localParticipantCount_, 1u));
        }
    }

    void syncPreferredTeamText() {
        preferredTeamText_ = teamChoiceText(preferredTeam_);
    }

    static bool isChoiceField(Field field) {
        return field == Field::TickRate ||
               field == Field::SnapshotRate ||
               field == Field::ShotEvaluationMode;
    }

    Field defaultActiveField() const {
        if (step_ != Step::Setup) {
            return Field::None;
        }
        if (mode_ == net::SessionLaunchMode::Host) {
            return Field::ServerPort;
        }
        if (joinSubview_ == JoinSubview::DirectConnect) {
            return Field::HostAddress;
        }
        return Field::PlayerName;
    }

    bool joinBrowserActive() const {
        return mode_ == net::SessionLaunchMode::Join && joinSubview_ == JoinSubview::Browser;
    }

    bool joinDirectConnectActive() const {
        return mode_ == net::SessionLaunchMode::Join && joinSubview_ == JoinSubview::DirectConnect;
    }

    void setStepInternal(Step step) {
        step_ = step;
        activeField_ = defaultActiveField();
        clickReady_ = false;
        validationError_.clear();
        statusMessage_.clear();
    }

    void setJoinSubviewInternal(JoinSubview subview) {
        joinSubview_ = subview;
        if (mode_ == net::SessionLaunchMode::Join &&
            joinSubview_ == JoinSubview::Browser) {
            discoveryAutoScanPending_ = true;
        }
        activeField_ = defaultActiveField();
        validationError_.clear();
        statusMessage_.clear();
    }

    void setHostAdvancedExpandedInternal(bool expanded) {
        hostAdvancedExpanded_ = expanded;
        activeField_ = defaultActiveField();
        validationError_.clear();
        statusMessage_.clear();
    }

    void resetDiscoveryBrowserState() {
        discoveryCache_ = net::SessionBrowserCache{};
        discoveryClockUs_ = 0u;
        nextDiscoveryRefreshUs_ = 0u;
        discoveryScanCount_ = 0u;
        selectedDiscoveryIndex_ = -1;
        selectedDiscoveryHost_.clear();
        selectedDiscoveryPort_ = 0u;
        discoveryAutoScanPending_ = mode_ == net::SessionLaunchMode::Join;
        if (mode_ == net::SessionLaunchMode::Join) {
            discoverySocket_ = net::UdpSocket{};
            discoverySocket_.bind({"127.0.0.1", 0u});
            return;
        }
        discoverySocket_ = net::UdpSocket{};
    }

    void pollDiscoveryResponses() {
        if (!discoverySocket_.isOpen() || mode_ != net::SessionLaunchMode::Join) {
            return;
        }

        net::ReceivedDatagram datagram;
        while (discoverySocket_.receive(&datagram) == net::ReceiveStatus::Received) {
            const net::SessionAdvertisementParseResult parsed =
                net::deserializeSessionAdvertisement(datagram.payload);
            if (!parsed.ok) {
                continue;
            }

            net::SessionAdvertisement advertisement = parsed.advertisement;
            if (advertisement.joinHost.empty()) {
                advertisement.joinHost = datagram.sender.host;
            }
            discoveryCache_.upsert(advertisement, discoveryClockUs_);
        }
    }

    void tickDiscoveryBrowser(std::uint64_t dtUs = 16'667u) {
        if (mode_ != net::SessionLaunchMode::Join) {
            return;
        }

        discoveryClockUs_ += dtUs;
        pollDiscoveryResponses();
        if (joinBrowserActive() &&
            (discoveryAutoScanPending_ || discoveryClockUs_ >= nextDiscoveryRefreshUs_)) {
            refreshDiscoveryBrowser();
            discoveryAutoScanPending_ = false;
        }
        discoveryCache_.expireStale(discoveryClockUs_);
        reconcileSelectedDiscoveryEntry();
    }

    void refreshDiscoveryBrowser() {
        if (!discoverySocket_.isOpen() || mode_ != net::SessionLaunchMode::Join) {
            return;
        }
        ++discoveryScanCount_;
        nextDiscoveryRefreshUs_ = discoveryClockUs_ + kDiscoveryRefreshIntervalUs;
        discoveryCache_.expireStale(discoveryClockUs_);
        discoverySocket_.sendTo({"127.0.0.1", discoveryPort_},
                                net::serializeSessionDiscoveryQuery(net::SessionDiscoveryQuery{}));
        pollDiscoveryResponses();
    }

    bool selectDiscoveryEntry(std::size_t index) {
        const auto visibleEntries = visibleDiscoveryEntries();
        if (index >= visibleEntries.size()) {
            return false;
        }

        const auto& advertisement = visibleEntries[index].advertisement;
        hostAddressText_ = advertisement.joinHost;
        serverPortText_ = std::to_string(advertisement.joinPort);
        selectedDiscoveryIndex_ = static_cast<int>(index);
        selectedDiscoveryHost_ = advertisement.joinHost;
        selectedDiscoveryPort_ = advertisement.joinPort;
        validationError_.clear();
        statusMessage_.clear();
        return true;
    }

    bool hasValidSelectedDiscoveryEntry() const {
        const auto visibleEntries = visibleDiscoveryEntries();
        return selectedDiscoveryIndex_ >= 0 &&
               selectedDiscoveryIndex_ < static_cast<int>(visibleEntries.size());
    }

    bool selectedDiscoveryAdvertisement(net::SessionAdvertisement* advertisementOut) const {
        const auto visibleEntries = visibleDiscoveryEntries();
        if (!hasValidSelectedDiscoveryEntry()) {
            return false;
        }
        if (advertisementOut != nullptr) {
            *advertisementOut =
                visibleEntries[static_cast<std::size_t>(selectedDiscoveryIndex_)].advertisement;
        }
        return true;
    }

    void toggleShotEvaluationMode() {
        shotEvaluationMode_ =
            shotEvaluationMode_ == net::ShotEvaluationMode::SeenPosition
                ? net::ShotEvaluationMode::LivePosition
                : net::ShotEvaluationMode::SeenPosition;
        syncShotEvaluationModeText();
    }

    void setTickRateChoice(std::uint16_t tickRateHz) {
        if (!net::isSupportedSessionTickRateChoice(tickRateHz)) {
            return;
        }
        tickRateHz_ = tickRateHz;
        if (snapshotRateHz_ > tickRateHz_) {
            snapshotRateHz_ = tickRateHz_;
            syncSnapshotRateText();
        }
        syncTickRateText();
    }

    void cycleTickRateChoice(int delta) {
        auto it = std::find(net::kSessionTickRateChoicesHz.begin(),
                            net::kSessionTickRateChoicesHz.end(),
                            tickRateHz_);
        std::size_t currentIndex = it != net::kSessionTickRateChoicesHz.end()
            ? static_cast<std::size_t>(std::distance(net::kSessionTickRateChoicesHz.begin(), it))
            : 0u;
        const int rawIndex = static_cast<int>(currentIndex) + delta;
        const int wrappedIndex = (rawIndex % static_cast<int>(net::kSessionTickRateChoicesHz.size()) +
                                  static_cast<int>(net::kSessionTickRateChoicesHz.size())) %
                                 static_cast<int>(net::kSessionTickRateChoicesHz.size());
        setTickRateChoice(net::kSessionTickRateChoicesHz[static_cast<std::size_t>(wrappedIndex)]);
    }

    void setSnapshotRateChoice(std::uint16_t snapshotRateHz) {
        if (!net::isSupportedSessionTickRateChoice(snapshotRateHz)) {
            return;
        }
        snapshotRateHz_ = std::min(snapshotRateHz, tickRateHz_);
        syncSnapshotRateText();
    }

    void cycleSnapshotRateChoice(int delta) {
        std::array<std::uint16_t, net::kSessionTickRateChoicesHz.size()> supportedChoices{};
        std::size_t supportedCount = 0u;
        for (const std::uint16_t choice : net::kSessionTickRateChoicesHz) {
            if (choice <= tickRateHz_) {
                supportedChoices[supportedCount++] = choice;
            }
        }
        if (supportedCount == 0u) {
            setSnapshotRateChoice(tickRateHz_);
            return;
        }

        std::size_t currentIndex = 0u;
        for (std::size_t index = 0; index < supportedCount; ++index) {
            if (supportedChoices[index] == snapshotRateHz_) {
                currentIndex = index;
                break;
            }
        }

        const int rawIndex = static_cast<int>(currentIndex) + delta;
        const int wrappedIndex = (rawIndex % static_cast<int>(supportedCount) +
                                  static_cast<int>(supportedCount)) %
                                 static_cast<int>(supportedCount);
        setSnapshotRateChoice(supportedChoices[static_cast<std::size_t>(wrappedIndex)]);
    }

    void cycleChoiceField(Field field, int delta) {
        switch (field) {
            case Field::TickRate:
                cycleTickRateChoice(delta);
                return;
            case Field::SnapshotRate:
                cycleSnapshotRateChoice(delta);
                return;
            case Field::ShotEvaluationMode:
                toggleShotEvaluationMode();
                return;
            default:
                return;
        }
    }

    bool studySurfaceActive() const {
        return surface_ == net::SessionProductSurface::LabStudy;
    }

    void cyclePreferredTeamChoice(int delta) {
        static constexpr std::array<sim::TeamId, 3> kOptions{
            sim::TeamId::Attacker,
            sim::TeamId::Defender,
            sim::TeamId::Spectator
        };
        std::size_t currentIndex = 0u;
        for (std::size_t index = 0; index < kOptions.size(); ++index) {
            if (kOptions[index] == preferredTeam_) {
                currentIndex = index;
                break;
            }
        }
        const int rawIndex = static_cast<int>(currentIndex) + delta;
        const int wrappedIndex = (rawIndex % static_cast<int>(kOptions.size()) +
                                  static_cast<int>(kOptions.size())) %
                                 static_cast<int>(kOptions.size());
        preferredTeam_ = kOptions[static_cast<std::size_t>(wrappedIndex)];
        syncPreferredTeamText();
    }

    bool resolveJoinEndpoint(std::string* hostOut,
                             std::uint16_t* portOut,
                             std::string* errorOut = nullptr) const {
        const std::string combinedHost = trim(hostAddressText_);
        if (combinedHost.empty()) {
            if (errorOut != nullptr) {
                *errorOut = "Enter a valid host or LAN address.";
            }
            return false;
        }

        std::string parsedHost = combinedHost;
        std::uint16_t parsedPort = 0u;

        const std::size_t firstColon = combinedHost.find(':');
        if (firstColon != std::string::npos) {
            const std::size_t lastColon = combinedHost.rfind(':');
            if (firstColon != lastColon) {
                if (errorOut != nullptr) {
                    *errorOut = "Use an IPv4 host or hostname. IPv6 join addresses are not supported yet.";
                }
                return false;
            }

            const std::string inlineHost = trim(combinedHost.substr(0, firstColon));
            const std::string inlinePort = trim(combinedHost.substr(firstColon + 1));
            if (inlineHost.empty() || !net::parseSessionPort(inlinePort, &parsedPort)) {
                if (errorOut != nullptr) {
                    *errorOut = "Enter a valid port between 1 and 65535.";
                }
                return false;
            }
            parsedHost = inlineHost;
        } else if (!net::parseSessionPort(serverPortText_, &parsedPort)) {
            if (errorOut != nullptr) {
                *errorOut = "Enter a valid port between 1 and 65535.";
            }
            return false;
        }

        if (!net::isValidSessionHost(parsedHost)) {
            if (errorOut != nullptr) {
                *errorOut = "Enter a valid host or LAN address.";
            }
            return false;
        }

        if (hostOut != nullptr) {
            *hostOut = parsedHost;
        }
        if (portOut != nullptr) {
            *portOut = parsedPort;
        }
        return true;
    }

    void normalizeJoinEndpointFields() {
        if (!joinDirectConnectActive()) {
            return;
        }

        std::string resolvedHost;
        std::uint16_t resolvedPort = 0u;
        if (!resolveJoinEndpoint(&resolvedHost, &resolvedPort, nullptr)) {
            return;
        }

        hostAddressText_ = resolvedHost;
        serverPortText_ = std::to_string(resolvedPort);
    }

    bool validateInternal(std::string* errorOut) const {
        const std::string playerName = trim(playerNameText_);
        if (playerName.empty()) {
            if (errorOut != nullptr) {
                *errorOut = "Enter a player name.";
            }
            return false;
        }

        if (mode_ == net::SessionLaunchMode::Host) {
            std::uint16_t parsedPort = 0u;
            if (!net::parseSessionPort(serverPortText_, &parsedPort)) {
                if (errorOut != nullptr) {
                    *errorOut = "Enter a valid port between 1 and 65535.";
                }
                return false;
            }
            if (selectedLevelSlot_ <= 0) {
                if (errorOut != nullptr) {
                    *errorOut = "Select a level before hosting.";
                }
                return false;
            }
            std::uint16_t totalBots = 0u;
            if (!net::parseSessionBotCount(totalBotCountText_, &totalBots)) {
                if (errorOut != nullptr) {
                    *errorOut = "Enter a bot count between 0 and " +
                                std::to_string(net::kMaxHostedBotCount) + ".";
                }
                return false;
            }
            std::uint16_t maxHumanPlayers = 0u;
            if (!net::parseSessionHumanPlayerCap(maxHumanPlayersText_, &maxHumanPlayers)) {
                if (errorOut != nullptr) {
                    *errorOut = "Enter a human-player cap between 1 and " +
                                std::to_string(net::kMaxHostedHumanPlayerCap) + ".";
                }
                return false;
            }
            if (maxHumanPlayers < localParticipantCount_) {
                if (errorOut != nullptr) {
                    *errorOut = "Human-player cap must be at least the local participant count.";
                }
                return false;
            }
            return true;
        }

        if (joinBrowserActive()) {
            if (!hasValidSelectedDiscoveryEntry()) {
                if (errorOut != nullptr) {
                    *errorOut = "Select a LAN session or switch to Direct Connect.";
                }
                return false;
            }
            return true;
        }

        return resolveJoinEndpoint(nullptr, nullptr, errorOut);
    }

    Rectangle setupFieldRect(Field field) const {
        if (!shouldRenderField(field)) {
            return Rectangle{};
        }

        if (joinBrowserActive()) {
            return field == Field::PlayerName
                ? Rectangle{
                    kSetupFieldPanelX,
                    kJoinBrowserPlayerFieldY,
                    kSetupFieldWidth,
                    kSetupFieldHeight,
                }
                : Rectangle{};
        }

        const bool hostAdvancedGrid =
            mode_ == net::SessionLaunchMode::Host && hostAdvancedExpanded_;
        const float startY = mode_ == net::SessionLaunchMode::Host
            ? kHostSetupFieldStartY
            : kJoinDirectFieldStartY;
        std::size_t visibleIndex = 0u;
        for (const auto& candidate : fields_) {
            if (!shouldRenderField(candidate.field)) {
                continue;
            }
            if (candidate.field == field) {
                if (hostAdvancedGrid) {
                    const float columnWidth = (kSetupFieldWidth - kSetupFieldColumnGapX) * 0.5f;
                    const std::size_t row = visibleIndex / 2u;
                    const std::size_t column = visibleIndex % 2u;
                    return Rectangle{
                        kSetupFieldPanelX +
                            static_cast<float>(column) * (columnWidth + kSetupFieldColumnGapX),
                        startY + static_cast<float>(row) * setupHostAdvancedRowPitch(),
                        columnWidth,
                        kSetupFieldHeight,
                    };
                }
                return Rectangle{
                    kSetupFieldPanelX,
                    startY + static_cast<float>(visibleIndex) * setupSingleColumnRowPitch(),
                    kSetupFieldWidth,
                    kSetupFieldHeight,
                };
            }
            ++visibleIndex;
        }

        return Rectangle{};
    }

    float setupPreviewY() const {
        float lowestFieldBottom = 0.0f;
        for (const auto& field : fields_) {
            if (!shouldRenderField(field.field)) {
                continue;
            }
            const Rectangle rect = setupFieldRect(field.field);
            lowestFieldBottom = std::max(lowestFieldBottom, rect.y + rect.height);
        }

        if (lowestFieldBottom <= 0.0f) {
            return 820.0f;
        }
        return lowestFieldBottom + kSetupPreviewGapY;
    }

    bool shouldRenderField(Field field) const {
        if (step_ != Step::Setup) {
            return false;
        }

        if (mode_ == net::SessionLaunchMode::Host) {
            switch (field) {
                case Field::ServerPort:
                case Field::PlayerName:
                case Field::TotalBots:
                    return true;
                case Field::SessionLabel:
                case Field::TickRate:
                case Field::SnapshotRate:
                case Field::MaxHumanPlayers:
                case Field::ShotEvaluationMode:
                    return hostAdvancedExpanded_;
                default:
                    return false;
            }
        }

        if (joinDirectConnectActive()) {
            return field == Field::HostAddress ||
                   field == Field::ServerPort ||
                   field == Field::PlayerName;
        }

        return field == Field::PlayerName;
    }

    net::SessionLaunchConfig buildLaunchConfigInternal() const {
        std::uint16_t parsedPort = net::kDefaultServerPort;
        net::parseSessionPort(serverPortText_, &parsedPort);

        if (mode_ == net::SessionLaunchMode::Host) {
            std::uint16_t totalBots = net::kDefaultHostedBotCount;
            std::uint16_t maxHumanPlayers = net::kDefaultHostedHumanPlayerCap;
            net::parseSessionBotCount(totalBotCountText_, &totalBots);
            net::parseSessionHumanPlayerCap(maxHumanPlayersText_, &maxHumanPlayers);
            const auto [attackerBots, defenderBots] =
                balancedBotTeamCounts(totalBots, preferredTeam_);
            net::SessionLaunchConfig config = studySurfaceActive()
                ? net::makeStudySessionLaunchConfig(selectedLevelSlot_,
                                                    trim(playerNameText_),
                                                    parsedPort,
                                                    attackerBots,
                                                    defenderBots,
                                                    shotEvaluationMode_,
                                                    localParticipantCount_)
                : net::makeHostSessionLaunchConfig(selectedLevelSlot_,
                                                   trim(playerNameText_),
                                                   parsedPort,
                                                   attackerBots,
                                                   defenderBots,
                                                   0u,
                                                   net::kDefaultProxyServerPort,
                                                   net::kProtocolVersion,
                                                   preferredTeam_,
                                                   localParticipantCount_,
                                                   shotEvaluationMode_);
            config.preferredTeam = preferredTeam_;
            if (!studySurfaceActive() || !trim(sessionLabelText_).empty()) {
                config.sessionLabel = trim(sessionLabelText_);
            }
            config.localParticipantCount = localParticipantCount_ == 0u
                ? net::kDefaultLocalParticipantCount
                : localParticipantCount_;
            config.tickRateHz = tickRateHz_;
            config.snapshotRateHz = snapshotRateHz_;
            config.maxHumanPlayers = maxHumanPlayers;
            config.shotEvaluationMode = shotEvaluationMode_;
            config.discoveryPort = discoveryPort_;
            net::normalizeSessionLaunchConfig(&config);
            return config;
        }

        if (joinBrowserActive()) {
            const std::string host = trim(hostAddressText_);
            const std::uint16_t port = parsedPort;
            net::SessionLaunchConfig config =
                net::makeJoinSessionLaunchConfig(host,
                                                 port,
                                                 trim(playerNameText_),
                                                 net::kProtocolVersion,
                                                 preferredTeam_,
                                                 localParticipantCount_,
                                                 shotEvaluationMode_);
            config.preferredTeam = preferredTeam_;
            return config;
        }

        std::string resolvedHost = trim(hostAddressText_);
        std::uint16_t resolvedPort = parsedPort;
        resolveJoinEndpoint(&resolvedHost, &resolvedPort, nullptr);
        net::SessionLaunchConfig config =
            net::makeJoinSessionLaunchConfig(resolvedHost,
                                             resolvedPort,
                                             trim(playerNameText_),
                                             net::kProtocolVersion,
                                             preferredTeam_,
                                             localParticipantCount_,
                                             shotEvaluationMode_);
        config.preferredTeam = preferredTeam_;
        return config;
    }

    void beginEditing(Field field) {
        if (busy_ || step_ != Step::Setup) {
            return;
        }
        activeField_ = field;
        validationError_.clear();
        statusMessage_.clear();
    }

    void cycleField() {
        if (mode_ == net::SessionLaunchMode::Host) {
            if (hostAdvancedExpanded_) {
                switch (activeField_) {
                    case Field::ServerPort: activeField_ = Field::PlayerName; break;
                    case Field::PlayerName: activeField_ = Field::TotalBots; break;
                    case Field::TotalBots: activeField_ = Field::SessionLabel; break;
                    case Field::SessionLabel: activeField_ = Field::TickRate; break;
                    case Field::TickRate: activeField_ = Field::SnapshotRate; break;
                    case Field::SnapshotRate: activeField_ = Field::MaxHumanPlayers; break;
                    case Field::MaxHumanPlayers: activeField_ = Field::ShotEvaluationMode; break;
                    default: activeField_ = Field::ServerPort; break;
                }
                return;
            }
            switch (activeField_) {
                case Field::ServerPort: activeField_ = Field::PlayerName; break;
                case Field::PlayerName: activeField_ = Field::TotalBots; break;
                default: activeField_ = Field::ServerPort; break;
            }
            return;
        }

        if (joinDirectConnectActive()) {
            switch (activeField_) {
                case Field::HostAddress: activeField_ = Field::ServerPort; break;
                case Field::ServerPort: activeField_ = Field::PlayerName; break;
                default: activeField_ = Field::HostAddress; break;
            }
            return;
        }

        activeField_ = Field::PlayerName;
    }

    void appendCharacterToActiveField(int ch) {
        std::string* activeValue = nullptr;
        switch (activeField_) {
            case Field::HostAddress: activeValue = &hostAddressText_; break;
            case Field::ServerPort: activeValue = &serverPortText_; break;
            case Field::PlayerName: activeValue = &playerNameText_; break;
            case Field::SessionLabel: activeValue = &sessionLabelText_; break;
            case Field::MaxHumanPlayers: activeValue = &maxHumanPlayersText_; break;
            case Field::TotalBots: activeValue = &totalBotCountText_; break;
            default: return;
        }

        if (activeField_ == Field::ServerPort ||
            activeField_ == Field::MaxHumanPlayers ||
            activeField_ == Field::TotalBots) {
            const std::size_t maxDigits = activeField_ == Field::ServerPort ? 5u : 2u;
            if (ch < '0' || ch > '9' || activeValue->size() >= maxDigits) {
                return;
            }
        } else if (activeField_ == Field::HostAddress) {
            const bool isAlphaNum = (ch >= 'a' && ch <= 'z') ||
                                    (ch >= 'A' && ch <= 'Z') ||
                                    (ch >= '0' && ch <= '9');
            const bool isAllowedPunct = ch == '.' || ch == '-' || ch == '_' || ch == ':';
            if ((!isAlphaNum && !isAllowedPunct) || activeValue->size() >= 127u) {
                return;
            }
        } else if (activeField_ == Field::PlayerName) {
            if (ch < 32 || ch > 126 || activeValue->size() >= 24u) {
                return;
            }
        } else if (activeField_ == Field::SessionLabel) {
            if (ch < 32 || ch > 126 || activeValue->size() >= 32u) {
                return;
            }
        }

        activeValue->push_back(static_cast<char>(ch));
    }

    bool handleTextInput() {
        int ch = GetCharPressed();
        while (ch > 0) {
            appendCharacterToActiveField(ch);
            ch = GetCharPressed();
        }

        if (IsKeyPressed(KEY_BACKSPACE)) {
            std::string* activeValue = nullptr;
            switch (activeField_) {
                case Field::HostAddress: activeValue = &hostAddressText_; break;
                case Field::ServerPort: activeValue = &serverPortText_; break;
                case Field::PlayerName: activeValue = &playerNameText_; break;
                case Field::SessionLabel: activeValue = &sessionLabelText_; break;
                case Field::MaxHumanPlayers: activeValue = &maxHumanPlayersText_; break;
                case Field::TotalBots: activeValue = &totalBotCountText_; break;
                default: break;
            }
            if (activeValue != nullptr && !activeValue->empty()) {
                activeValue->pop_back();
            }
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            activeField_ = Field::None;
            validationError_.clear();
            statusMessage_.clear();
            return true;
        }
        return false;
    }

    Result handleBackShortcutPressed() {
        validationError_.clear();
        statusMessage_.clear();
        busy_ = false;
        activeField_ = Field::None;
        if (step_ == Step::TeamChoice) {
            setStepInternal(Step::Setup);
            return Result{};
        }
        return Result{GameMode::MAIN_MENU, false, {}};
    }

    Result trySubmit() {
        if (busy_) {
            return Result{};
        }

        validationError_.clear();
        if (step_ == Step::Setup) {
            normalizeJoinEndpointFields();
            if (!validateInternal(&validationError_)) {
                return Result{};
            }
            setStepInternal(Step::TeamChoice);
            return Result{};
        }

        Result result;
        result.mode = GameMode::MULTIPLAYER_SESSION;
        result.submitted = true;
        result.launchConfig = buildLaunchConfigInternal();
        return result;
    }

    void selectTeamChoice(sim::TeamId team) {
        preferredTeam_ = (team == sim::TeamId::Attacker ||
                          team == sim::TeamId::Defender ||
                          team == sim::TeamId::Spectator)
            ? team
            : sim::TeamId::None;
        syncPreferredTeamText();
    }

    std::string currentSetupSummaryLine() const {
        if (mode_ == net::SessionLaunchMode::Host) {
            const net::SessionLaunchConfig preview = buildLaunchConfigInternal();
            return "Level " + std::to_string(preview.levelSlot) +
                   " | Port " + std::to_string(preview.clientConnectPort) +
                   " | Host " + sessionLabelFallback(sessionLabelText_, playerNameText_) +
                   " | Bots " + std::to_string(preview.attackerBotCount + preview.defenderBotCount) +
                   " | " + std::to_string(preview.tickRateHz) + " / " +
                   std::to_string(preview.snapshotRateHz) + " Hz";
        }

        if (joinBrowserActive()) {
            net::SessionAdvertisement advertisement;
            if (!selectedDiscoveryAdvertisement(&advertisement)) {
                return "Select a LAN session or switch to Direct Connect.";
            }
            return net::displayLabelForSessionAdvertisement(advertisement) +
                   " | " + net::detailLineForSessionAdvertisement(advertisement);
        }

        return "Direct Connect | " + hostAddressText_ + ":" + serverPortText_;
    }

    const char* submitButtonLabel() const {
        if (busy_) {
            return "Starting...";
        }
        return mode_ == net::SessionLaunchMode::Host ? "Start Game" : "Join Game";
    }

    const char* stepTitle() const {
        if (step_ == Step::TeamChoice) {
            return "CHOOSE TEAM";
        }
        return mode_ == net::SessionLaunchMode::Host ? "HOST GAME" : "JOIN GAME";
    }

    const char* stepSubtitle() const {
        if (step_ == Step::TeamChoice) {
            return "";
        }
        if (mode_ == net::SessionLaunchMode::Host) {
            return "";
        }
        return joinBrowserActive()
            ? "Find a LAN session or connect directly."
            : "Enter host details, then choose a team.";
    }

    TypographyService& typography() const {
        return TypographyService::shared();
    }

    void drawTextLine(TypographyStyleId styleId,
                      std::string_view text,
                      float x,
                      float y,
                      Color color) const {
        typography().draw(styleId, text, Vector2{x, y}, color);
    }

    void drawCenteredText(TypographyStyleId styleId,
                          std::string_view text,
                          float centerX,
                          float y,
                          Color color) const {
        typography().drawCentered(styleId, text, centerX, y, color);
    }

    void drawTextField(const TextField& field, const Rectangle& rect) const {
        const TypographyStyle& labelStyle = typography().style(TypographyStyleId::FieldLabel);
        const TypographyStyle& valueStyle = typography().style(TypographyStyleId::FieldValue);
        drawTextLine(TypographyStyleId::FieldLabel,
                     field.label,
                     rect.x,
                     rect.y - labelStyle.lineHeight - 6.0f,
                     Color{166, 176, 194, 255});

        const bool active = field.field == activeField_;
        const Color fieldColor = active ? Color{38, 58, 78, 255} : Color{20, 25, 34, 255};
        const Color borderColor = active ? Color{72, 197, 255, 255} : Color{58, 70, 94, 255};
        DrawRectangleRounded(rect, 0.08f, 8, fieldColor);
        DrawRectangleRoundedLines(rect, 0.08f, 8, borderColor);

        drawTextLine(TypographyStyleId::FieldValue,
                     *field.value,
                     rect.x + 18.0f,
                     rect.y + (rect.height - valueStyle.lineHeight) * 0.5f,
                     WHITE);
    }

    void drawButton(const Rectangle& rect,
                    const char* label,
                    bool active,
                    bool enabled = true) const {
        const Color fill = !enabled
            ? Color{36, 58, 78, 255}
            : active
                ? Color{40, 148, 204, 255}
                : Color{30, 126, 184, 255};
        DrawRectangleRounded(rect, 0.1f, 8, fill);
        const TypographyStyle& buttonStyle = typography().style(TypographyStyleId::SectionTitle);
        drawCenteredText(TypographyStyleId::SectionTitle,
                         label,
                         rect.x + rect.width * 0.5f,
                         rect.y + rect.height * 0.5f - buttonStyle.lineHeight * 0.5f,
                         WHITE);
    }

    std::vector<std::string> wrappedSummaryLines(std::string_view summary, float maxWidth) const {
        std::vector<std::string> lines;
        std::size_t cursor = 0u;
        std::string currentLine;
        while (cursor < summary.size()) {
            const std::size_t delimiter = summary.find(" | ", cursor);
            const std::string segment = std::string(summary.substr(
                cursor,
                delimiter == std::string_view::npos ? std::string_view::npos : delimiter - cursor));
            const std::string candidate = currentLine.empty() ? segment : currentLine + " | " + segment;
            if (!currentLine.empty() &&
                typography().measureWidth(TypographyStyleId::Caption, candidate) > maxWidth) {
                lines.push_back(currentLine);
                currentLine = segment;
            } else {
                currentLine = candidate;
            }

            if (delimiter == std::string_view::npos) {
                break;
            }
            cursor = delimiter + 3u;
        }

        if (!currentLine.empty()) {
            lines.push_back(currentLine);
        }
        return lines;
    }

    void renderSetupPanel(const Rectangle& panel) const {
        if (mode_ == net::SessionLaunchMode::Host) {
            const std::string levelTitle = selectedLevelSlot_ > 0
                ? "Level Slot " + std::to_string(selectedLevelSlot_)
                : "No Level Selected";
            DrawRectangleRounded(Rectangle{panel.x + 40.0f, kHostHeaderY, 6.0f, 52.0f},
                                 0.8f,
                                 8,
                                 Color{72, 197, 255, 255});
            drawTextLine(TypographyStyleId::Caption,
                         "HOSTING",
                         panel.x + 62.0f,
                         kHostHeaderY,
                         Color{152, 163, 183, 255});
            drawTextLine(TypographyStyleId::SectionTitle,
                         levelTitle,
                         panel.x + 62.0f,
                         kHostHeaderY + 26.0f,
                         WHITE);
            DrawLineEx(Vector2{panel.x + 40.0f, kHostHeaderY + 78.0f},
                       Vector2{panel.x + panel.width - 40.0f, kHostHeaderY + 78.0f},
                       1.0f,
                       Color{48, 60, 82, 255});

            drawButton(advancedButton_,
                       hostAdvancedExpanded_ ? "Essentials" : "Advanced",
                       hostAdvancedExpanded_ ||
                           CheckCollisionPointRec(display::mousePosition(), advancedButton_));
        } else {
            drawButton(browserSubviewButton_,
                       "LAN Browser",
                       joinSubview_ == JoinSubview::Browser);
            drawButton(directConnectSubviewButton_,
                       "Direct Connect",
                       joinSubview_ == JoinSubview::DirectConnect);

            if (joinBrowserActive()) {
                drawTextLine(TypographyStyleId::SectionTitle,
                             "Compatible LAN sessions",
                             panel.x + 40.0f,
                             340.0f,
                             WHITE);
                const std::string browserLine = discoveryBrowserSummaryLine();
                drawTextLine(TypographyStyleId::Caption,
                             browserLine,
                             panel.x + 40.0f,
                             370.0f,
                             GRAY);
                drawButton(refreshButton_,
                           "Refresh",
                           CheckCollisionPointRec(display::mousePosition(), refreshButton_));

                const auto visibleEntries = visibleDiscoveryEntries();
                for (std::size_t index = 0;
                     index < std::min<std::size_t>(visibleEntries.size(), discoveryRowRects_.size());
                     ++index) {
                    const bool selected = static_cast<int>(index) == selectedDiscoveryIndex_;
                    const Rectangle row = discoveryRowRects_[index];
                    const Color rowColor = selected
                        ? Color{58, 82, 120, 255}
                        : Color{30, 36, 48, 255};
                    DrawRectangleRounded(row, 0.08f, 8, rowColor);
                    DrawRectangleRoundedLines(row,
                                              0.08f,
                                              8,
                                              selected ? SKYBLUE : Color{80, 92, 120, 255});

                    const auto& advertisement = visibleEntries[index].advertisement;
                    const std::string title =
                        std::to_string(index + 1u) + ". " +
                        net::displayLabelForSessionAdvertisement(advertisement);
                    const std::string detail =
                        net::detailLineForSessionAdvertisement(advertisement);
                    drawTextLine(TypographyStyleId::FieldLabel,
                                 title,
                                 row.x + 16.0f,
                                 row.y + 14.0f,
                                 WHITE);
                    drawTextLine(TypographyStyleId::Caption,
                                 detail,
                                 row.x + 16.0f,
                                 row.y + 42.0f,
                                 LIGHTGRAY);
                }
            } else {
                drawTextLine(TypographyStyleId::SectionTitle,
                             "Direct Connect",
                             panel.x + 40.0f,
                             340.0f,
                             WHITE);
                drawTextLine(TypographyStyleId::Caption,
                             "Enter a host and port.",
                             panel.x + 40.0f,
                             370.0f,
                             GRAY);
            }
        }

        for (const auto& field : fields_) {
            if (shouldRenderField(field.field)) {
                drawTextField(field, setupFieldRect(field.field));
            }
        }
    }

    void renderTeamChoicePanel(const Rectangle& panel) const {
        drawTextLine(TypographyStyleId::SectionTitle,
                     "Setup Summary",
                     panel.x + 40.0f,
                     280.0f,
                     WHITE);
        const std::vector<std::string> summaryLines =
            wrappedSummaryLines(currentSetupSummaryLine(), panel.width - 80.0f);
        for (std::size_t lineIndex = 0; lineIndex < summaryLines.size(); ++lineIndex) {
            drawTextLine(TypographyStyleId::Caption,
                         summaryLines[lineIndex],
                         panel.x + 40.0f,
                         314.0f + static_cast<float>(lineIndex) * 22.0f,
                         lineIndex == 0u ? LIGHTGRAY : GRAY);
        }

        static constexpr std::array<sim::TeamId, 3> kChoices{
            sim::TeamId::Attacker,
            sim::TeamId::Defender,
            sim::TeamId::Spectator
        };
        for (std::size_t index = 0; index < kChoices.size(); ++index) {
            const sim::TeamId choice = kChoices[index];
            const Rectangle card = teamChoiceRects_[index];
            const bool selected = preferredTeam_ == choice;
            const Color fill = selected ? Color{58, 82, 120, 255} : Color{30, 36, 48, 255};
            const Color border = selected ? SKYBLUE : Color{80, 92, 120, 255};
            DrawRectangleRounded(card, 0.08f, 8, fill);
            DrawRectangleRoundedLines(card, 0.08f, 8, border);
            const Rectangle accentBand{card.x, card.y, card.width, 12.0f};
            DrawRectangleRounded(accentBand, 0.2f, 8, Fade(teamChoiceAccentColor(choice), selected ? 0.95f : 0.72f));

            const Rectangle iconBounds = centeredRect(
                Rectangle{card.x, card.y + 30.0f, card.width, 112.0f},
                88.0f,
                88.0f);
            drawTeamChoiceGlyph(choice, iconBounds);
            drawCenteredText(TypographyStyleId::FieldValue,
                             teamChoiceText(choice),
                             card.x + card.width * 0.5f,
                             card.y + 164.0f,
                             WHITE);
            if (selected) {
                const Rectangle selectedPill = centeredRect(
                    Rectangle{card.x, card.y + 198.0f, card.width, 34.0f},
                    114.0f,
                    34.0f);
                DrawRectangleRounded(selectedPill, 0.48f, 8, Fade(GOLD, 0.18f));
                DrawRectangleRoundedLines(selectedPill, 0.48f, 8, Fade(GOLD, 0.92f));
                const TypographyStyle& selectedStyle = typography().style(TypographyStyleId::OverlayAccent);
                drawCenteredText(TypographyStyleId::OverlayAccent,
                                 "Selected",
                                 selectedPill.x + selectedPill.width * 0.5f,
                                 selectedPill.y + selectedPill.height * 0.5f -
                                     selectedStyle.lineHeight * 0.5f,
                                 GOLD);
            }
        }
    }

public:
    explicit MultiplayerSessionMenu(
        net::SessionLaunchMode mode = net::SessionLaunchMode::Join,
        net::SessionProductSurface surface = net::SessionProductSurface::Multiplayer,
        std::uint16_t discoveryPort = net::kDefaultSessionDiscoveryPort)
        : mode_(mode),
          surface_(surface == net::SessionProductSurface::LabStudy
                       ? net::SessionProductSurface::LabStudy
                       : net::SessionProductSurface::Multiplayer),
          discoveryPort_(discoveryPort),
          preferredTeam_(mode_ == net::SessionLaunchMode::Host
                             ? sim::TeamId::Attacker
                             : sim::TeamId::Defender) {
        initializeLayout();
        syncPreferredTeamText();
        syncTickRateText();
        syncSnapshotRateText();
        syncShotEvaluationModeText();
        ensureHumanPlayerCapAtLeastLocalParticipants();
        resetDiscoveryBrowserState();
        activeField_ = defaultActiveField();
        clickReady_ = false;
    }

    void setMode(net::SessionLaunchMode mode) {
        mode_ = mode;
        step_ = Step::Setup;
        joinSubview_ = JoinSubview::Browser;
        hostAdvancedExpanded_ = false;
        validationError_.clear();
        preferredTeam_ = mode_ == net::SessionLaunchMode::Host
            ? sim::TeamId::Attacker
            : sim::TeamId::Defender;
        syncPreferredTeamText();
        ensureHumanPlayerCapAtLeastLocalParticipants();
        resetDiscoveryBrowserState();
        activeField_ = defaultActiveField();
        clickReady_ = false;
    }

    net::SessionLaunchMode mode() const {
        return mode_;
    }

    net::SessionProductSurface surface() const {
        return surface_;
    }

    void setSurfaceForTest(net::SessionProductSurface surface) {
        surface_ = surface == net::SessionProductSurface::LabStudy
            ? net::SessionProductSurface::LabStudy
            : net::SessionProductSurface::Multiplayer;
    }

    Step step() const {
        return step_;
    }

    JoinSubview joinSubview() const {
        return joinSubview_;
    }

    void setJoinSubviewForTest(JoinSubview subview) {
        setJoinSubviewInternal(subview);
    }

    bool hostAdvancedExpanded() const {
        return hostAdvancedExpanded_;
    }

    void setHostAdvancedExpandedForTest(bool expanded) {
        setHostAdvancedExpandedInternal(expanded);
    }

    void setSelectedLevelSlot(int levelSlot) {
        selectedLevelSlot_ = levelSlot;
    }

    int selectedLevelSlot() const {
        return selectedLevelSlot_;
    }

    void setLocalParticipantCountForTest(std::uint8_t localParticipantCount) {
        localParticipantCount_ =
            localParticipantCount == 0u ? net::kDefaultLocalParticipantCount : localParticipantCount;
        ensureHumanPlayerCapAtLeastLocalParticipants();
    }

    std::uint8_t localParticipantCount() const {
        return localParticipantCount_;
    }

    void setHostAddress(const std::string& hostAddress) {
        hostAddressText_ = hostAddress;
    }

    const std::string& hostAddress() const {
        return hostAddressText_;
    }

    void setServerPortText(const std::string& portText) {
        serverPortText_ = portText;
    }

    const std::string& serverPortText() const {
        return serverPortText_;
    }

    void setPlayerName(const std::string& playerName) {
        playerNameText_ = playerName;
    }

    const std::string& playerName() const {
        return playerNameText_;
    }

    void setPreferredTeam(sim::TeamId preferredTeam) {
        preferredTeam_ = (preferredTeam == sim::TeamId::Attacker ||
                          preferredTeam == sim::TeamId::Defender ||
                          preferredTeam == sim::TeamId::Spectator)
            ? preferredTeam
            : sim::TeamId::None;
        syncPreferredTeamText();
    }

    sim::TeamId preferredTeam() const {
        return preferredTeam_;
    }

    void setSessionLabel(const std::string& sessionLabel) {
        sessionLabelText_ = sessionLabel;
    }

    const std::string& sessionLabel() const {
        return sessionLabelText_;
    }

    void setTickRateHz(std::uint16_t tickRateHz) {
        setTickRateChoice(tickRateHz);
    }

    std::uint16_t tickRateHz() const {
        return tickRateHz_;
    }

    void setSnapshotRateHz(std::uint16_t snapshotRateHz) {
        setSnapshotRateChoice(snapshotRateHz);
    }

    std::uint16_t snapshotRateHz() const {
        return snapshotRateHz_;
    }

    void setMaxHumanPlayersText(const std::string& humanPlayerCapText) {
        maxHumanPlayersText_ = humanPlayerCapText;
    }

    const std::string& maxHumanPlayersText() const {
        return maxHumanPlayersText_;
    }

    void setShotEvaluationMode(net::ShotEvaluationMode mode) {
        shotEvaluationMode_ = mode;
        syncShotEvaluationModeText();
    }

    net::ShotEvaluationMode shotEvaluationMode() const {
        return shotEvaluationMode_;
    }

    void setDiscoveryPortForTest(std::uint16_t discoveryPort) {
        discoveryPort_ = discoveryPort;
    }

    void setTotalBotCountText(const std::string& botCountText) {
        totalBotCountText_ = botCountText;
    }

    const std::string& totalBotCountText() const {
        return totalBotCountText_;
    }

    bool validate(std::string* errorOut = nullptr) const {
        return validateInternal(errorOut);
    }

    const std::string& validationError() const {
        return validationError_;
    }

    bool hasValidationError() const {
        return !validationError_.empty();
    }

    net::SessionLaunchConfig buildLaunchConfig() const {
        return buildLaunchConfigInternal();
    }

    std::string previewLine() const {
        if (mode_ != net::SessionLaunchMode::Host) {
            return {};
        }

        const net::SessionLaunchConfig preview = buildLaunchConfigInternal();
        const std::string previewLabel = sessionLabelFallback(sessionLabelText_, playerNameText_);
        return std::string(studySurfaceActive() ? "Study" : "Join") +
               " on 127.0.0.1:" + std::to_string(preview.clientConnectPort) +
               " | Session " + previewLabel +
               " | Team " + std::string(teamChoiceText(preview.preferredTeam)) +
               " | Tick " + std::to_string(preview.tickRateHz) +
               " | Snap " + std::to_string(preview.snapshotRateHz) +
               " | Humans " + std::to_string(preview.maxHumanPlayers) +
               " | " + net::shotEvaluationModeSummary(preview.shotEvaluationMode) +
               " | Locals " + std::to_string(preview.localParticipantCount) +
               " (local test) | LAN peers use this machine's IPv4 + the same port" +
               " | Bots " + std::to_string(preview.attackerBotCount + preview.defenderBotCount) +
               " (" + std::to_string(preview.attackerBotCount) + " attackers / " +
               std::to_string(preview.defenderBotCount) + " defenders)" +
               " | internal server/proxy ports assigned automatically";
    }

    void setStatusMessage(const std::string& message, bool busy) {
        validationError_.clear();
        statusMessage_ = message;
        busy_ = busy;
        if (busy_) {
            activeField_ = Field::None;
        }
    }

    const std::string& statusMessage() const {
        return statusMessage_;
    }

    bool busy() const {
        return busy_;
    }

    const char* submitButtonLabelForTest() const {
        return submitButtonLabel();
    }

    std::uint32_t discoveryScanCount() const {
        return discoveryScanCount_;
    }

    std::vector<net::SessionBrowserEntry> visibleDiscoveryEntries() const {
        std::vector<net::SessionBrowserEntry> entries;
        for (const auto& entry : discoveryCache_.entries()) {
            if (entry.compatibility == net::BrowserCompatibilityState::Compatible) {
                entries.push_back(entry);
            }
        }
        std::sort(entries.begin(),
                  entries.end(),
                  [](const net::SessionBrowserEntry& lhs,
                     const net::SessionBrowserEntry& rhs) {
                      const std::string lhsLabel =
                          net::displayLabelForSessionAdvertisement(lhs.advertisement);
                      const std::string rhsLabel =
                          net::displayLabelForSessionAdvertisement(rhs.advertisement);
                      if (lhsLabel != rhsLabel) {
                          return lhsLabel < rhsLabel;
                      }
                      if (lhs.advertisement.joinHost != rhs.advertisement.joinHost) {
                          return lhs.advertisement.joinHost < rhs.advertisement.joinHost;
                      }
                      return lhs.advertisement.joinPort < rhs.advertisement.joinPort;
                  });
        return entries;
    }

    std::vector<net::SessionBrowserEntry> unavailableDiscoveryEntries() const {
        std::vector<net::SessionBrowserEntry> entries;
        for (const auto& entry : discoveryCache_.entries()) {
            if (entry.compatibility != net::BrowserCompatibilityState::Compatible) {
                entries.push_back(entry);
            }
        }
        std::sort(entries.begin(),
                  entries.end(),
                  [](const net::SessionBrowserEntry& lhs,
                     const net::SessionBrowserEntry& rhs) {
                      if (lhs.compatibility != rhs.compatibility) {
                          return static_cast<int>(lhs.compatibility) <
                                 static_cast<int>(rhs.compatibility);
                      }
                      if (lhs.advertisement.joinHost != rhs.advertisement.joinHost) {
                          return lhs.advertisement.joinHost < rhs.advertisement.joinHost;
                      }
                      return lhs.advertisement.joinPort < rhs.advertisement.joinPort;
                  });
        return entries;
    }

private:
    void reconcileSelectedDiscoveryEntry() {
        if (selectedDiscoveryHost_.empty() || selectedDiscoveryPort_ == 0u) {
            const auto visibleEntries = visibleDiscoveryEntries();
            if (selectedDiscoveryIndex_ >= static_cast<int>(visibleEntries.size())) {
                selectedDiscoveryIndex_ = -1;
            }
            return;
        }

        const auto visibleEntries = visibleDiscoveryEntries();
        for (std::size_t index = 0; index < visibleEntries.size(); ++index) {
            const auto& advertisement = visibleEntries[index].advertisement;
            if (advertisement.joinHost == selectedDiscoveryHost_ &&
                advertisement.joinPort == selectedDiscoveryPort_) {
                selectedDiscoveryIndex_ = static_cast<int>(index);
                return;
            }
        }

        selectedDiscoveryIndex_ = -1;
        selectedDiscoveryHost_.clear();
        selectedDiscoveryPort_ = 0u;
    }

public:
    std::string discoveryBrowserSummaryLine() const {
        const std::size_t compatibleCount = visibleDiscoveryEntries().size();
        const std::size_t unavailableCount = unavailableDiscoveryEntries().size();

        if (compatibleCount == 0u) {
            if (unavailableCount == 0u) {
                return "No compatible sessions found.";
            }
            return "No compatible sessions found. " +
                   std::to_string(unavailableCount) +
                   " hidden.";
        }

        std::string line = std::to_string(compatibleCount) + " compatible session(s) found.";
        if (unavailableCount > 0u) {
            line += " " + std::to_string(unavailableCount) +
                    " hidden.";
        }
        return line;
    }

    Rectangle fieldRectForTest(std::string_view label) const {
        for (const auto& field : fields_) {
            if (field.label == label) {
                return setupFieldRect(field.field);
            }
        }
        return Rectangle{};
    }

    Rectangle cornerBackButtonRectForTest() const {
        return cornerBackButton_;
    }

    Rectangle launchButtonRectForTest() const {
        return launchButton_;
    }

    Rectangle teamChoiceRectForTest(sim::TeamId team) const {
        switch (team) {
            case sim::TeamId::Attacker:
                return teamChoiceRects_[0];
            case sim::TeamId::Defender:
                return teamChoiceRects_[1];
            case sim::TeamId::Spectator:
                return teamChoiceRects_[2];
            default:
                return Rectangle{};
        }
    }

    Color teamChoiceAccentColorForTest(sim::TeamId team) const {
        return teamChoiceAccentColor(team);
    }

    const char* stepSubtitleForTest() const {
        return stepSubtitle();
    }

    Result triggerBackForTest() {
        return handleBackShortcutPressed();
    }

    Rectangle discoveryRowRectForTest(std::size_t index) const {
        if (index >= discoveryRowRects_.size()) {
            return Rectangle{};
        }
        return discoveryRowRects_[index];
    }

    float setupPreviewYForTest() const {
        return setupPreviewY();
    }

    void injectDiscoveryAdvertisementForTest(const net::SessionAdvertisement& advertisement,
                                             std::uint64_t nowUs = 0u) {
        const std::uint64_t insertionTime = nowUs == 0u ? discoveryClockUs_ : nowUs;
        discoveryClockUs_ = std::max(discoveryClockUs_, insertionTime);
        discoveryCache_.upsert(advertisement, insertionTime);
    }

    void tickDiscoveryForTest(std::uint64_t dtUs = 16'667u) {
        tickDiscoveryBrowser(dtUs);
    }

    void refreshDiscoveryForTest() {
        refreshDiscoveryBrowser();
    }

    bool selectDiscoveryEntryForTest(std::size_t index) {
        return selectDiscoveryEntry(index);
    }

    Result submitForTest() {
        return trySubmit();
    }

    Result chooseTeamForTest(sim::TeamId team) {
        if (step_ != Step::TeamChoice) {
            return Result{};
        }
        selectTeamChoice(team);
        return Result{};
    }

    void setActiveFieldForTest(std::string_view label) {
        for (const auto& field : fields_) {
            if (field.label == label) {
                activeField_ = field.field;
                return;
            }
        }
        activeField_ = Field::None;
    }

    bool hasActiveFieldForTest() const {
        return activeField_ != Field::None;
    }

    Result handleBackShortcutForTest(int key, bool allowBackShortcut = true) {
        const bool escapePressed = key == KEY_ESCAPE;
        const bool qPressed = key == KEY_Q;
        const bool backShortcutPressed = allowBackShortcut && (escapePressed || qPressed);

        if (mode_ == net::SessionLaunchMode::Host && step_ == Step::Setup && backShortcutPressed) {
            return handleBackShortcutPressed();
        }

        if (step_ == Step::Setup && activeField_ != Field::None) {
            if (escapePressed) {
                activeField_ = Field::None;
                validationError_.clear();
                statusMessage_.clear();
            }
            return Result{};
        }

        if (backShortcutPressed) {
            return handleBackShortcutPressed();
        }
        return Result{};
    }

    Result update(bool allowBackShortcut = true) {
        const Vector2 mousePos = display::mousePosition();
        const bool confirmPressed = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
        const bool escapePressed = IsKeyPressed(KEY_ESCAPE);
        const bool qPressed = IsKeyPressed(KEY_Q);
        const bool backShortcutPressed = allowBackShortcut && (escapePressed || qPressed);

        tickDiscoveryBrowser();
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            clickReady_ = true;
        }
        const bool leftClickPressed = clickReady_ && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

        if (CheckCollisionPointRec(mousePos, cornerBackButton_) &&
            leftClickPressed) {
            return handleBackShortcutPressed();
        }

        if (!busy_ && step_ == Step::Setup) {
            if (mode_ == net::SessionLaunchMode::Host &&
                CheckCollisionPointRec(mousePos, advancedButton_) &&
                leftClickPressed) {
                setHostAdvancedExpandedInternal(!hostAdvancedExpanded_);
            }

            if (mode_ == net::SessionLaunchMode::Join) {
                if (CheckCollisionPointRec(mousePos, browserSubviewButton_) &&
                    leftClickPressed) {
                    setJoinSubviewInternal(JoinSubview::Browser);
                }
                if (CheckCollisionPointRec(mousePos, directConnectSubviewButton_) &&
                    leftClickPressed) {
                    setJoinSubviewInternal(JoinSubview::DirectConnect);
                }
                if (joinBrowserActive() &&
                    CheckCollisionPointRec(mousePos, refreshButton_) &&
                    leftClickPressed) {
                    refreshDiscoveryBrowser();
                }
                if (joinBrowserActive()) {
                    const auto visibleEntries = visibleDiscoveryEntries();
                    for (std::size_t index = 0;
                         index < std::min<std::size_t>(visibleEntries.size(), discoveryRowRects_.size());
                         ++index) {
                        if (CheckCollisionPointRec(mousePos, discoveryRowRects_[index]) &&
                            leftClickPressed) {
                            selectDiscoveryEntry(index);
                        }
                    }
                }
            }

            for (const auto& field : fields_) {
                if (!shouldRenderField(field.field)) {
                    continue;
                }
                const Rectangle rect = setupFieldRect(field.field);
                if (CheckCollisionPointRec(mousePos, rect) && leftClickPressed) {
                    beginEditing(field.field);
                    if (isChoiceField(field.field)) {
                        cycleChoiceField(field.field, 1);
                    }
                }
            }
        }

        if (!busy_ && step_ == Step::TeamChoice) {
            static constexpr std::array<sim::TeamId, 3> kChoices{
                sim::TeamId::Attacker,
                sim::TeamId::Defender,
                sim::TeamId::Spectator
            };
            for (std::size_t index = 0; index < teamChoiceRects_.size(); ++index) {
                if (CheckCollisionPointRec(mousePos, teamChoiceRects_[index]) &&
                    leftClickPressed) {
                    selectTeamChoice(kChoices[index]);
                    return Result{};
                }
            }
        }

        if (!busy_ && CheckCollisionPointRec(mousePos, launchButton_) && leftClickPressed) {
            return trySubmit();
        }

        if (!busy_ && step_ == Step::Setup && IsKeyPressed(KEY_TAB)) {
            cycleField();
        }
        bool activeFieldClearedThisFrame = false;
        if (!busy_ && step_ == Step::Setup && activeField_ != Field::None) {
            activeFieldClearedThisFrame = handleTextInput();
            if (isChoiceField(activeField_) &&
                (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_SPACE))) {
                if (IsKeyPressed(KEY_LEFT)) {
                    cycleChoiceField(activeField_, -1);
                } else {
                    cycleChoiceField(activeField_, 1);
                }
            }
        }
        if (!busy_ && step_ == Step::TeamChoice &&
            (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_RIGHT))) {
            cyclePreferredTeamChoice(IsKeyPressed(KEY_LEFT) ? -1 : 1);
        }

        if (!busy_ && confirmPressed) {
            return trySubmit();
        }
        if (activeFieldClearedThisFrame) {
            return Result{};
        }
        if (mode_ == net::SessionLaunchMode::Host && step_ == Step::Setup && backShortcutPressed) {
            return handleBackShortcutPressed();
        }
        if (backShortcutPressed && activeField_ == Field::None) {
            return handleBackShortcutPressed();
        }

        return Result{};
    }

    void render() const {
        ClearBackground(Color{11, 12, 18, 255});
        DrawRectangleGradientV(0,
                               0,
                               1920,
                               1080,
                               Color{16, 18, 27, 255},
                               Color{8, 10, 15, 255});

        const char* title = stepTitle();
        const char* subtitle = stepSubtitle();

        drawCenteredText(TypographyStyleId::ScreenTitle,
                         title,
                         1920.0f * 0.5f,
                         110.0f,
                         SKYBLUE);
        if (subtitle[0] != '\0') {
            drawCenteredText(TypographyStyleId::FieldLabel,
                             subtitle,
                             1920.0f * 0.5f,
                             190.0f,
                             LIGHTGRAY);
        }

        const bool cornerBackHovered = CheckCollisionPointRec(display::mousePosition(), cornerBackButton_);
        DrawRectangleRounded(cornerBackButton_,
                             0.18f,
                             8,
                             cornerBackHovered ? Color{56, 72, 96, 255} : Color{34, 40, 56, 255});
        DrawRectangleRoundedLines(cornerBackButton_,
                                  0.18f,
                                  8,
                                  cornerBackHovered ? SKYBLUE : Color{88, 100, 126, 255});
        const TypographyStyle& backStyle = typography().style(TypographyStyleId::ButtonLabel);
        drawCenteredText(TypographyStyleId::ButtonLabel,
                         "Back",
                         cornerBackButton_.x + cornerBackButton_.width * 0.5f,
                         cornerBackButton_.y + cornerBackButton_.height * 0.5f -
                             backStyle.lineHeight * 0.5f,
                         LIGHTGRAY);

        const Color panelColor{21, 26, 36, 255};
        const Rectangle panel{1920.0f / 2.0f - 380.0f, 250.0f, 760.0f, 700.0f};
        DrawRectangleRounded(panel, 0.08f, 12, panelColor);
        DrawRectangleRoundedLines(panel, 0.08f, 12, Color{58, 72, 98, 255});

        if (step_ == Step::Setup) {
            renderSetupPanel(panel);
        } else {
            renderTeamChoicePanel(panel);
        }

        const bool launchHover = CheckCollisionPointRec(display::mousePosition(), launchButton_);
        drawButton(launchButton_, submitButtonLabel(), launchHover, !busy_);

        if (!validationError_.empty()) {
            drawTextLine(TypographyStyleId::OverlayAccent,
                         validationError_,
                         panel.x + 40.0f,
                         955.0f,
                         Color{255, 110, 110, 255});
        } else if (!statusMessage_.empty()) {
            drawTextLine(TypographyStyleId::OverlayAccent,
                         statusMessage_,
                         panel.x + 40.0f,
                         955.0f,
                         busy_ ? SKYBLUE : LIGHTGRAY);
        }
    }
};
