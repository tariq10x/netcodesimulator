#include "net/ProxyRuntime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace net {
namespace {

struct LinkState {
    ProxyLinkConfig config{};
    std::mt19937 rng{std::random_device{}()};
    ProxyStats stats{};
};

struct ScheduledRelay {
    bool upstream{true};
    std::uint16_t peerId{0};
    std::uint64_t deliverAtUs{0};
    std::uint64_t order{0};
    UdpEndpoint destination{};
    ByteBuffer payload{};
};

std::uint32_t seedFor(const ProxyLinkConfig& config, std::uint16_t peerId, bool upstream) {
    const std::uint32_t directionSalt = upstream ? 0xA341316Cu : 0xC8013EA4u;
    return config.seed ^ directionSalt ^ static_cast<std::uint32_t>(peerId);
}

std::uint64_t microsFromMillis(double milliseconds) {
    if (milliseconds <= 0.0) {
        return 0u;
    }
    const double micros = milliseconds * 1000.0;
    if (micros >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(std::round(micros));
}

double uniformPercent(std::mt19937& rng) {
    static std::uniform_real_distribution<double> distribution(0.0, 100.0);
    return distribution(rng);
}

double uniformJitter(std::mt19937& rng, float magnitudeMs) {
    if (magnitudeMs <= 0.0f) {
        return 0.0;
    }
    std::uniform_real_distribution<double> distribution(-static_cast<double>(magnitudeMs),
                                                        static_cast<double>(magnitudeMs));
    return distribution(rng);
}

}  // namespace

struct ProxyRuntime::Impl {
    std::unordered_map<std::uint16_t, LinkState> upstreamLinks{};
    std::unordered_map<std::uint16_t, LinkState> downstreamLinks{};
    std::unordered_map<std::uint32_t, UdpEndpoint> sessionRoutes{};
    std::unordered_map<std::uint16_t, UdpEndpoint> peerRoutes{};
    std::vector<ScheduledRelay> scheduled{};
    ProxyStats upstreamAggregate{};
    ProxyStats downstreamAggregate{};
    std::uint64_t orderCounter{0u};
};

ProxyRuntime::ProxyRuntime(ProxyConfig config)
    : config_(std::move(config)),
      impl_(new Impl()) {}

ProxyRuntime::~ProxyRuntime() {
    stop();
}

bool ProxyRuntime::start() {
    stop();
    impl_ = new Impl();

    if (!clientSocket_.bind(config_.clientListenEndpoint)) {
        lastError_ = clientSocket_.lastError();
        stop();
        return false;
    }
    if (!serverSocket_.bind(config_.serverListenEndpoint)) {
        lastError_ = serverSocket_.lastError();
        stop();
        return false;
    }

    lastError_.clear();
    return true;
}

void ProxyRuntime::stop() {
    clientSocket_ = UdpSocket{};
    serverSocket_ = UdpSocket{};
    delete impl_;
    impl_ = nullptr;
}

namespace {

ProxyStats& aggregateStatsFor(ProxyRuntime::Impl* impl, bool upstream) {
    return upstream ? impl->upstreamAggregate : impl->downstreamAggregate;
}

const ProxyStats& aggregateStatsFor(const ProxyRuntime::Impl* impl, bool upstream) {
    return upstream ? impl->upstreamAggregate : impl->downstreamAggregate;
}

std::unordered_map<std::uint16_t, LinkState>& linkMapFor(ProxyRuntime::Impl* impl, bool upstream) {
    return upstream ? impl->upstreamLinks : impl->downstreamLinks;
}

const std::unordered_map<std::uint16_t, LinkState>& linkMapFor(const ProxyRuntime::Impl* impl, bool upstream) {
    return upstream ? impl->upstreamLinks : impl->downstreamLinks;
}

LinkState& ensureLinkState(ProxyRuntime::Impl* impl,
                           const ProxyConfig& config,
                           std::uint16_t peerId,
                           bool upstream) {
    auto& links = linkMapFor(impl, upstream);
    auto it = links.find(peerId);
    if (it != links.end()) {
        return it->second;
    }

    LinkState state;
    state.config = upstream ? config.defaultUpstream : config.defaultDownstream;
    state.rng.seed(seedFor(state.config, peerId, upstream));
    auto insertResult = links.emplace(peerId, state);
    return insertResult.first->second;
}

bool shouldTrigger(float pct, std::mt19937& rng) {
    if (pct <= 0.0f) {
        return false;
    }
    return uniformPercent(rng) < static_cast<double>(pct);
}

void updateQueuedStats(ProxyRuntime::Impl* impl) {
    impl->upstreamAggregate.queuedPackets = 0u;
    impl->downstreamAggregate.queuedPackets = 0u;
    for (auto& [_, link] : impl->upstreamLinks) {
        link.stats.queuedPackets = 0u;
    }
    for (auto& [_, link] : impl->downstreamLinks) {
        link.stats.queuedPackets = 0u;
    }

    for (const auto& relay : impl->scheduled) {
        ProxyStats& aggregate = aggregateStatsFor(impl, relay.upstream);
        ++aggregate.queuedPackets;

        auto& links = linkMapFor(impl, relay.upstream);
        const auto linkIt = links.find(relay.peerId);
        if (linkIt != links.end()) {
            ++linkIt->second.stats.queuedPackets;
        }
    }
}

void scheduleRelay(ProxyRuntime::Impl* impl,
                   const ProxyConfig& config,
                   bool upstream,
                   std::uint16_t peerId,
                   const UdpEndpoint& destination,
                   const ByteBuffer& payload,
                   std::uint64_t nowUs) {
    LinkState& link = ensureLinkState(impl, config, peerId, upstream);
    ProxyStats& aggregate = aggregateStatsFor(impl, upstream);

    ++link.stats.receivedPackets;
    ++aggregate.receivedPackets;

    if (shouldTrigger(link.config.lossPct, link.rng)) {
        ++link.stats.droppedPackets;
        ++aggregate.droppedPackets;
        updateQueuedStats(impl);
        return;
    }

    double delayMs = static_cast<double>(link.config.baseDelayMs) +
                     uniformJitter(link.rng, link.config.jitterMs);
    if (delayMs < 0.0) {
        delayMs = 0.0;
    }

    std::uint64_t deliverAtUs = nowUs + microsFromMillis(delayMs);
    if (shouldTrigger(link.config.reorderPct, link.rng) && !impl->scheduled.empty()) {
        ++link.stats.reorderedPackets;
        ++aggregate.reorderedPackets;
        deliverAtUs = nowUs;
    }

    ScheduledRelay relay;
    relay.upstream = upstream;
    relay.peerId = peerId;
    relay.deliverAtUs = deliverAtUs;
    relay.order = impl->orderCounter++;
    relay.destination = destination;
    relay.payload = payload;
    impl->scheduled.push_back(relay);

    if (shouldTrigger(link.config.duplicatePct, link.rng)) {
        ++link.stats.duplicatedPackets;
        ++aggregate.duplicatedPackets;

        ScheduledRelay duplicate = relay;
        duplicate.order = impl->orderCounter++;
        duplicate.deliverAtUs += 1u;
        impl->scheduled.push_back(std::move(duplicate));
    }

    updateQueuedStats(impl);
}

ParseResult tryParsePacket(const ByteBuffer& payload) {
    return deserializePacket(payload);
}

}  // namespace

void ProxyRuntime::tick(std::uint64_t nowUs) {
    if (impl_ == nullptr) {
        return;
    }

    auto relayClientPackets = [&](UdpSocket& socket) {
        ReceivedDatagram datagram;
        while (true) {
            const ReceiveStatus status = socket.receive(&datagram);
            if (status == ReceiveStatus::WouldBlock) {
                break;
            }
            if (status == ReceiveStatus::Error) {
                lastError_ = socket.lastError();
                return;
            }

            const ParseResult parsed = tryParsePacket(datagram.payload);
            if (!parsed.ok) {
                continue;
            }

            const Packet& packet = parsed.packet;
            const std::uint16_t peerId = packet.header.peerId;
            if (packet.header.kind == PacketKind::Hello) {
                const auto& hello = std::get<HelloMessage>(packet.payload);
                impl_->sessionRoutes[hello.sessionId] = datagram.sender;
            }
            if (peerId != 0u) {
                impl_->peerRoutes[peerId] = datagram.sender;
            }

            if (packet.header.kind == PacketKind::CommandBundle) {
                Packet controlPacket;
                controlPacket.header = packet.header;
                controlPacket.header.kind = PacketKind::ControlCommandBundle;
                controlPacket.payload =
                    ControlCommandBundle{std::get<CommandBundle>(packet.payload).commands};
                if (!serverSocket_.sendTo(config_.serverEndpoint, serializePacket(controlPacket))) {
                    lastError_ = serverSocket_.lastError();
                }
            }

            scheduleRelay(impl_,
                          config_,
                          true,
                          peerId,
                          config_.serverEndpoint,
                          datagram.payload,
                          nowUs);
        }
    };

    auto relayServerPackets = [&](UdpSocket& socket) {
        ReceivedDatagram datagram;
        while (true) {
            const ReceiveStatus status = socket.receive(&datagram);
            if (status == ReceiveStatus::WouldBlock) {
                break;
            }
            if (status == ReceiveStatus::Error) {
                lastError_ = socket.lastError();
                return;
            }

            const ParseResult parsed = tryParsePacket(datagram.payload);
            if (!parsed.ok) {
                continue;
            }

            const Packet& packet = parsed.packet;
            std::uint16_t peerId = packet.header.peerId;
            UdpEndpoint destination{};
            bool haveDestination = false;

            if (packet.header.kind == PacketKind::Welcome) {
                const auto& welcome = std::get<WelcomeMessage>(packet.payload);
                auto routeIt = impl_->sessionRoutes.find(welcome.sessionId);
                if (routeIt != impl_->sessionRoutes.end()) {
                    peerId = welcome.assignedPeerId;
                    impl_->peerRoutes[peerId] = routeIt->second;
                    destination = routeIt->second;
                    haveDestination = true;
                }
            } else {
                auto routeIt = impl_->peerRoutes.find(peerId);
                if (routeIt != impl_->peerRoutes.end()) {
                    destination = routeIt->second;
                    haveDestination = true;
                }
            }

            if (!haveDestination) {
                continue;
            }
            scheduleRelay(impl_,
                          config_,
                          false,
                          peerId,
                          destination,
                          datagram.payload,
                          nowUs);
        }
    };

    relayClientPackets(clientSocket_);
    relayServerPackets(serverSocket_);

    std::stable_sort(impl_->scheduled.begin(),
                     impl_->scheduled.end(),
                     [](const ScheduledRelay& lhs, const ScheduledRelay& rhs) {
                         if (lhs.deliverAtUs != rhs.deliverAtUs) {
                             return lhs.deliverAtUs < rhs.deliverAtUs;
                         }
                         return lhs.order < rhs.order;
                     });

    auto it = impl_->scheduled.begin();
    while (it != impl_->scheduled.end()) {
        if (it->deliverAtUs > nowUs) {
            ++it;
            continue;
        }

        UdpSocket& outboundSocket = it->upstream ? serverSocket_ : clientSocket_;
        if (!outboundSocket.sendTo(it->destination, it->payload)) {
            lastError_ = outboundSocket.lastError();
            ++it;
            continue;
        }

        LinkState& link = ensureLinkState(impl_, config_, it->peerId, it->upstream);
        ProxyStats& aggregate = aggregateStatsFor(impl_, it->upstream);
        ++link.stats.forwardedPackets;
        ++aggregate.forwardedPackets;
        it = impl_->scheduled.erase(it);
    }

    updateQueuedStats(impl_);
}

void ProxyRuntime::setDefaultLinkConfig(bool upstream, const ProxyLinkConfig& config) {
    if (upstream) {
        config_.defaultUpstream = config;
    } else {
        config_.defaultDownstream = config;
    }
}

void ProxyRuntime::setPeerLinkConfig(std::uint16_t peerId, bool upstream, const ProxyLinkConfig& config) {
    if (impl_ == nullptr) {
        return;
    }

    auto& links = linkMapFor(impl_, upstream);
    LinkState state;
    state.config = config;
    state.rng.seed(seedFor(config, peerId, upstream));
    links[peerId] = state;
}

std::uint16_t ProxyRuntime::clientListenPort() const {
    return clientSocket_.localPort();
}

std::uint16_t ProxyRuntime::serverListenPort() const {
    return serverSocket_.localPort();
}

ProxyStats ProxyRuntime::aggregateStats(bool upstream) const {
    if (impl_ == nullptr) {
        return ProxyStats{};
    }
    return aggregateStatsFor(static_cast<const Impl*>(impl_), upstream);
}

ProxyStats ProxyRuntime::peerStats(std::uint16_t peerId, bool upstream) const {
    if (impl_ == nullptr) {
        return ProxyStats{};
    }

    const auto& links = linkMapFor(static_cast<const Impl*>(impl_), upstream);
    const auto it = links.find(peerId);
    return it != links.end() ? it->second.stats : ProxyStats{};
}

const std::string& ProxyRuntime::lastError() const {
    return lastError_;
}

}  // namespace net
