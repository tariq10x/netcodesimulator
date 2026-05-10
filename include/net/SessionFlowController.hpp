#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "InputHandler3D.hpp"
#include "app/SplitScreenSessionController.hpp"
#include "net/ClientRuntime.hpp"
#include "net/SessionDiscovery.hpp"
#include "net/ServerRuntime.hpp"
#include "net/SessionLaunchConfig.hpp"
#include "net/TransportArtifactAdapter.hpp"
#include "net/UdpSocket.hpp"

namespace net {

enum class SessionFlowState : std::uint8_t {
    Idle = 0,
    StartingServer = 1,
    StartingProxy = 2,
    StartingClient = 3,
    Connecting = 4,
    Running = 5,
    Failed = 6,
    Ended = 7
};

const char* toString(SessionFlowState state);

struct SessionRuntimeComposition {
    SessionLaunchConfig config{};
    std::vector<std::string> startupSequence_{};
    UdpSocket discoverySocket_{};
    UdpSocket hostedServerSocket_{};
    std::unique_ptr<ServerRuntime> hostedServer_{};
    std::unique_ptr<TransportArtifactAdapter> proxy_{};
    std::unique_ptr<ClientRuntime> client_{};
};

class SessionFlowController {
public:
    explicit SessionFlowController(SessionLaunchConfig config = {});
    explicit SessionFlowController(SessionRuntimeComposition composition);
    ~SessionFlowController();

    SessionFlowController(const SessionFlowController&) = delete;
    SessionFlowController& operator=(const SessionFlowController&) = delete;

    bool start();
    void update(float dtSeconds, const InputHandler3D::InputState* input = nullptr);
    void render() const;
    void shutdown(const std::string& reason = "session ended");

    SessionFlowState state() const;
    const std::string& statusMessage() const;
    const SessionLaunchConfig& config() const;
    HostedSessionMetadata hostedSessionMetadata() const;
    const std::vector<std::string>& startupSequence() const;
    bool shouldReturnToMainMenu() const;
    ProxyStats aggregateProxyStats(bool upstream) const;
    std::size_t hostedSessionCount() const;
    void bindPrimaryLocalParticipant(std::uint16_t participantId,
                                     int actorId,
                                     const std::string& label);
    void requestRightLocalParticipant();
    bool bindRightLocalParticipant(std::uint16_t participantId,
                                   int actorId,
                                   const std::string& label);
    void bindRightObservation(const client::PaneBinding& binding);
    void setFocusedPane(sim::PaneSlot slot);
    void disableSplitScreen();
    const app::SplitScreenSessionController& splitScreenController() const;

    ClientRuntime* clientRuntime();
    const ClientRuntime* clientRuntime() const;
    const ServerRuntime* hostedServer() const;

private:
    void adoptComposition(SessionRuntimeComposition composition);
    void fail(const std::string& reason);
    void syncSplitScreenConfig();
    void advanceHostedNetworking(std::uint64_t dtUs);
    void pumpHostedDiscoveryResponder();
    void pumpHostedNetworkingAtCurrentTime();
    void pumpHostedServerIngress();
    void flushHostedServerPackets();
    void tickHostedServer(std::uint64_t dtUs, std::uint64_t nowUs);
    void sendHostedServerPacket(const Packet& packet);
    void updateStateFromClient(bool sessionShouldEnd);
    std::uint64_t serverTickIntervalUs() const;

    SessionLaunchConfig config_{};
    SessionFlowState state_{SessionFlowState::Idle};
    std::string statusMessage_{"idle"};
    std::vector<std::string> startupSequence_{};
    std::uint64_t clockUs_{0u};
    std::uint64_t hostedNetworkAccumulatorUs_{0u};
    std::uint64_t serverAccumulatorUs_{0u};
    UdpSocket discoverySocket_{};
    UdpSocket hostedServerSocket_{};
    std::unique_ptr<ServerRuntime> hostedServer_{};
    std::unique_ptr<TransportArtifactAdapter> proxy_{};
    std::unique_ptr<ClientRuntime> client_{};
    app::SplitScreenSessionController splitScreenController_{};
};

}  // namespace net
