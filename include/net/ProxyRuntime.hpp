#pragma once

#include <cstdint>
#include <string>

#include "net/Protocol.hpp"
#include "net/TransportArtifactAdapter.hpp"
#include "net/UdpSocket.hpp"

namespace net {

struct ProxyConfig {
    UdpEndpoint clientListenEndpoint{"127.0.0.1", 0};
    UdpEndpoint serverEndpoint{"127.0.0.1", 41000};
    UdpEndpoint serverListenEndpoint{"127.0.0.1", 0};
    ProxyLinkConfig defaultUpstream{};
    ProxyLinkConfig defaultDownstream{};
};

class ProxyRuntime final : public TransportArtifactAdapter {
public:
    explicit ProxyRuntime(ProxyConfig config = {});
    ~ProxyRuntime();

    ProxyRuntime(const ProxyRuntime&) = delete;
    ProxyRuntime& operator=(const ProxyRuntime&) = delete;

    bool start() override;
    void stop() override;
    void tick(std::uint64_t nowUs) override;

    void setDefaultLinkConfig(bool upstream, const ProxyLinkConfig& config) override;
    void setPeerLinkConfig(std::uint16_t peerId, bool upstream, const ProxyLinkConfig& config) override;

    std::uint16_t clientListenPort() const override;
    std::uint16_t serverListenPort() const override;
    ProxyStats aggregateStats(bool upstream) const override;
    ProxyStats peerStats(std::uint16_t peerId, bool upstream) const override;
    const std::string& lastError() const override;

    struct Impl;

private:
    ProxyConfig config_{};
    UdpSocket clientSocket_{};
    UdpSocket serverSocket_{};
    std::string lastError_{};
    Impl* impl_{nullptr};
};

}  // namespace net
