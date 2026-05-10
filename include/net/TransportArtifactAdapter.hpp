#pragma once

#include <cstdint>
#include <string>

#include "net/Protocol.hpp"

namespace net {

constexpr std::uint16_t kFirstBotTransportTargetId = 1000u;

inline bool isBotTransportTargetId(std::uint16_t targetId) {
    return targetId >= kFirstBotTransportTargetId;
}

inline RuntimeParamScope runtimeParamScopeForTargetId(std::uint16_t targetId) {
    return isBotTransportTargetId(targetId)
        ? RuntimeParamScope::Bot
        : RuntimeParamScope::Player;
}

inline bool transportTargetUsesProxyLink(std::uint16_t targetId) {
    return targetId != 0u && !isBotTransportTargetId(targetId);
}

inline std::string runtimeParamKeyForTarget(std::uint16_t targetId, const char* suffix) {
    const char* prefix = isBotTransportTargetId(targetId) ? "net.bot[" : "net.player[";
    return std::string(prefix) + std::to_string(targetId) + "]." + suffix;
}

class TransportArtifactAdapter {
public:
    TransportArtifactAdapter() = default;
    virtual ~TransportArtifactAdapter() = default;

    TransportArtifactAdapter(const TransportArtifactAdapter&) = delete;
    TransportArtifactAdapter& operator=(const TransportArtifactAdapter&) = delete;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void tick(std::uint64_t nowUs) = 0;

    virtual void setDefaultLinkConfig(bool upstream, const ProxyLinkConfig& config) = 0;
    virtual void setPeerLinkConfig(std::uint16_t peerId, bool upstream, const ProxyLinkConfig& config) = 0;

    virtual std::uint16_t clientListenPort() const = 0;
    virtual std::uint16_t serverListenPort() const = 0;
    virtual ProxyStats aggregateStats(bool upstream) const = 0;
    virtual ProxyStats peerStats(std::uint16_t peerId, bool upstream) const = 0;
    virtual const std::string& lastError() const = 0;
};

}  // namespace net
