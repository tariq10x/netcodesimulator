#include "net/TransportArtifactAdapter.hpp"
#include "net/ProxyRuntime.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <type_traits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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
        expect(status == net::ReceiveStatus::Received, "socket receive should not error during proxy test");
        const auto parsed = net::deserializePacket(datagram.payload);
        expect(parsed.ok, "proxy tests should receive valid protocol packets");
        packets.push_back(parsed.packet);
    }
    return packets;
}

std::vector<net::Packet> filterPacketsByKind(const std::vector<net::Packet>& packets,
                                             net::PacketKind kind) {
    std::vector<net::Packet> filtered;
    for (const auto& packet : packets) {
        if (packet.header.kind == kind) {
            filtered.push_back(packet);
        }
    }
    return filtered;
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

std::vector<net::Packet> waitForPackets(net::UdpSocket* socket,
                                        std::size_t expectedCount,
                                        std::chrono::milliseconds timeout) {
    std::vector<net::Packet> packets;
    const bool received = waitForPredicate([&]() {
        auto next = drainPackets(socket);
        if (!next.empty()) {
            packets.insert(packets.end(), next.begin(), next.end());
        }
        return packets.size() >= expectedCount;
    }, timeout);
    expect(received, "proxy test timed out waiting for forwarded packets");
    return packets;
}

template <typename Predicate>
std::vector<net::Packet> waitForPacketsUntil(net::UdpSocket* socket,
                                             Predicate predicate,
                                             std::chrono::milliseconds timeout) {
    std::vector<net::Packet> packets;
    const bool received = waitForPredicate([&]() {
        auto next = drainPackets(socket);
        if (!next.empty()) {
            packets.insert(packets.end(), next.begin(), next.end());
        }
        return predicate(packets);
    }, timeout);
    expect(received, "proxy test timed out waiting for forwarded packets");
    return packets;
}

void waitForProxyReceived(net::TransportArtifactAdapter* transport,
                          bool upstream,
                          std::uint32_t expectedCount,
                          std::uint64_t tickUs,
                          std::chrono::milliseconds timeout) {
    const bool received = waitForPredicate([&]() {
        transport->tick(tickUs);
        return transport->aggregateStats(upstream).receivedPackets >= expectedCount;
    }, timeout);
    expect(received, "proxy test timed out waiting for proxy ingress");
}

void testFixedDelayDefersForwardingUntilScheduledTime() {
    net::UdpSocket client;
    net::UdpSocket server;
    expect(client.bind(net::UdpEndpoint{"127.0.0.1", 0}), "client socket should bind");
    expect(server.bind(net::UdpEndpoint{"127.0.0.1", 0}), "server socket should bind");

    net::ProxyConfig config;
    config.serverEndpoint = net::UdpEndpoint{"127.0.0.1", server.localPort()};
    config.defaultUpstream.baseDelayMs = 100.0f;
    net::ProxyRuntime proxy(config);
    net::TransportArtifactAdapter& transport = proxy;
    expect(transport.start(), "proxy should start");

    expect(client.sendTo(net::UdpEndpoint{"127.0.0.1", transport.clientListenPort()},
                         net::serializePacket(makeCommandPacket(1u))),
           "client should send to proxy");

    waitForProxyReceived(&transport, true, 1u, 1'000'000u, std::chrono::milliseconds(250));
    const auto immediatePackets = waitForPackets(&server, 1u, std::chrono::milliseconds(250));
    expect(immediatePackets.size() == 1u &&
               immediatePackets.front().header.kind == net::PacketKind::ControlCommandBundle,
           "proxy should fork a clean control command upstream immediately");

    transport.tick(1'099'000u);
    expect(drainPackets(&server).empty(),
           "packet should still be queued before the delay threshold");

    transport.tick(1'100'000u);
    const auto packets = waitForPackets(&server, 1u, std::chrono::milliseconds(250));
    expect(packets.size() == 1u &&
               packets.front().header.kind == net::PacketKind::CommandBundle,
           "the manipulated gameplay command should still respect the configured upstream delay");
    expect(packets.front().header.seq == 1u, "forwarded packet should preserve the original payload");
}

struct ProxyDeterminismResult {
    std::vector<std::uint32_t> forwardedSeqs{};
    net::ProxyStats aggregate{};
};

ProxyDeterminismResult runDeterministicProxyCase() {
    net::UdpSocket client;
    net::UdpSocket server;
    expect(client.bind(net::UdpEndpoint{"127.0.0.1", 0}), "client socket should bind");
    expect(server.bind(net::UdpEndpoint{"127.0.0.1", 0}), "server socket should bind");

    net::ProxyConfig config;
    config.serverEndpoint = net::UdpEndpoint{"127.0.0.1", server.localPort()};
    config.defaultUpstream.lossPct = 35.0f;
    config.defaultUpstream.duplicatePct = 40.0f;
    config.defaultUpstream.seed = 0x12345678u;
    net::ProxyRuntime proxy(config);
    net::TransportArtifactAdapter& transport = proxy;
    expect(transport.start(), "proxy should start");

    for (std::uint32_t seq = 1u; seq <= 8u; ++seq) {
        expect(client.sendTo(net::UdpEndpoint{"127.0.0.1", transport.clientListenPort()},
                             net::serializePacket(makeCommandPacket(seq))),
               "client should send deterministic case packet");
    }

    waitForProxyReceived(&transport, true, 8u, 2'000'000u, std::chrono::milliseconds(250));
    std::vector<net::Packet> packets;
    const bool settled = waitForPredicate([&]() {
        transport.tick(2'000'001u);
        auto next = drainPackets(&server);
        if (!next.empty()) {
            packets.insert(packets.end(), next.begin(), next.end());
        }
        const auto stats = transport.aggregateStats(true);
        return stats.receivedPackets >= 8u &&
               stats.queuedPackets == 0u &&
               stats.forwardedPackets + stats.droppedPackets >= 8u &&
               packets.size() >= stats.forwardedPackets;
    }, std::chrono::milliseconds(250));
    expect(settled, "deterministic proxy case should settle after ingress and duplicate delivery");

    ProxyDeterminismResult result;
    result.aggregate = transport.aggregateStats(true);
    result.forwardedSeqs.reserve(packets.size());
    for (const auto& packet : packets) {
        if (packet.header.kind == net::PacketKind::CommandBundle) {
            result.forwardedSeqs.push_back(packet.header.seq);
        }
    }
    std::sort(result.forwardedSeqs.begin(), result.forwardedSeqs.end());
    return result;
}

void testFixedSeedLossAndDuplicationAreDeterministic() {
    const ProxyDeterminismResult first = runDeterministicProxyCase();
    const ProxyDeterminismResult second = runDeterministicProxyCase();

    expect(first.forwardedSeqs == second.forwardedSeqs,
           "fixed-seed proxy loss and duplication should replay the same forwarded sequence pattern");
    expect(first.aggregate.forwardedPackets == second.aggregate.forwardedPackets &&
           first.aggregate.droppedPackets == second.aggregate.droppedPackets &&
           first.aggregate.duplicatedPackets == second.aggregate.duplicatedPackets,
           "fixed-seed proxy stats should be reproducible across runs");
}

void testZeroImpairmentRelayPreservesOrderAndDelivery() {
    net::UdpSocket client;
    net::UdpSocket server;
    expect(client.bind(net::UdpEndpoint{"127.0.0.1", 0}), "client socket should bind");
    expect(server.bind(net::UdpEndpoint{"127.0.0.1", 0}), "server socket should bind");

    net::ProxyConfig config;
    config.serverEndpoint = net::UdpEndpoint{"127.0.0.1", server.localPort()};
    net::ProxyRuntime proxy(config);
    net::TransportArtifactAdapter& transport = proxy;
    expect(transport.start(), "proxy should start");

    expect(client.sendTo(net::UdpEndpoint{"127.0.0.1", transport.clientListenPort()},
                         net::serializePacket(makeCommandPacket(10u))),
           "first packet should send");
    expect(client.sendTo(net::UdpEndpoint{"127.0.0.1", transport.clientListenPort()},
                         net::serializePacket(makeCommandPacket(11u))),
           "second packet should send");

    waitForProxyReceived(&transport, true, 2u, 3'000'000u, std::chrono::milliseconds(250));
    const auto packets = waitForPacketsUntil(
        &server,
        [](const std::vector<net::Packet>& packets) {
            return filterPacketsByKind(packets, net::PacketKind::CommandBundle).size() >= 2u &&
                   filterPacketsByKind(packets, net::PacketKind::ControlCommandBundle).size() >= 2u;
        },
        std::chrono::milliseconds(250));
    const auto commandPackets = filterPacketsByKind(packets, net::PacketKind::CommandBundle);
    const auto controlPackets = filterPacketsByKind(packets, net::PacketKind::ControlCommandBundle);

    expect(commandPackets.size() == 2u && controlPackets.size() == 2u,
           "zero impairment proxy should deliver both the clean control fork and the manipulated gameplay packets");
    expect(commandPackets[0].header.seq == 10u && commandPackets[1].header.seq == 11u,
           "zero impairment proxy should preserve packet order");
    const auto stats = transport.aggregateStats(true);
    expect(stats.droppedPackets == 0u && stats.duplicatedPackets == 0u,
           "zero impairment proxy should not mutate delivery statistics");
}

void testPeerSpecificOverridesRemainIsolated() {
    net::UdpSocket client;
    net::UdpSocket server;
    expect(client.bind(net::UdpEndpoint{"127.0.0.1", 0}), "client socket should bind");
    expect(server.bind(net::UdpEndpoint{"127.0.0.1", 0}), "server socket should bind");

    net::ProxyConfig config;
    config.serverEndpoint = net::UdpEndpoint{"127.0.0.1", server.localPort()};
    net::ProxyRuntime proxy(config);
    net::TransportArtifactAdapter& transport = proxy;
    expect(transport.start(), "proxy should start");

    net::ProxyLinkConfig delayedLink;
    delayedLink.baseDelayMs = 60.0f;
    transport.setPeerLinkConfig(1u, true, delayedLink);

    expect(client.sendTo(net::UdpEndpoint{"127.0.0.1", transport.clientListenPort()},
                         net::serializePacket(makeCommandPacket(21u, 1u))),
           "peer-one packet should send");
    expect(client.sendTo(net::UdpEndpoint{"127.0.0.1", transport.clientListenPort()},
                         net::serializePacket(makeCommandPacket(22u, 2u))),
           "peer-two packet should send");

    waitForProxyReceived(&transport, true, 2u, 4'000'000u, std::chrono::milliseconds(250));
    const auto firstPackets = waitForPacketsUntil(
        &server,
        [](const std::vector<net::Packet>& packets) {
            const auto commandPackets = filterPacketsByKind(packets, net::PacketKind::CommandBundle);
            const auto controlPackets = filterPacketsByKind(packets, net::PacketKind::ControlCommandBundle);
            return !commandPackets.empty() && controlPackets.size() >= 2u;
        },
        std::chrono::milliseconds(250));
    const auto firstCommandPackets =
        filterPacketsByKind(firstPackets, net::PacketKind::CommandBundle);
    const auto firstControlPackets =
        filterPacketsByKind(firstPackets, net::PacketKind::ControlCommandBundle);
    expect(firstCommandPackets.size() == 1u &&
               firstCommandPackets.front().header.seq == 22u &&
               firstControlPackets.size() == 2u,
           "peer-specific overrides should still fork clean control traffic immediately while only delaying the targeted gameplay command");

    transport.tick(4'060'000u);
    const auto delayedPackets = waitForPackets(&server, 1u, std::chrono::milliseconds(250));
    expect(delayedPackets.size() == 1u &&
               delayedPackets.front().header.kind == net::PacketKind::CommandBundle &&
               delayedPackets.front().header.seq == 21u,
           "the targeted participant should still receive the configured delay");
}

void testUpstreamCommandsForkImmediateControlStream() {
    net::UdpSocket client;
    net::UdpSocket server;
    expect(client.bind(net::UdpEndpoint{"127.0.0.1", 0}), "client socket should bind");
    expect(server.bind(net::UdpEndpoint{"127.0.0.1", 0}), "server socket should bind");

    net::ProxyConfig config;
    config.serverEndpoint = net::UdpEndpoint{"127.0.0.1", server.localPort()};
    config.defaultUpstream.baseDelayMs = 100.0f;
    net::ProxyRuntime proxy(config);
    net::TransportArtifactAdapter& transport = proxy;
    expect(transport.start(), "proxy should start");

    expect(client.sendTo(net::UdpEndpoint{"127.0.0.1", transport.clientListenPort()},
                         net::serializePacket(makeCommandPacket(1u))),
           "client should send a command packet through the proxy");
    waitForProxyReceived(&transport, true, 1u, 1'000'000u, std::chrono::milliseconds(250));

    const auto immediatePackets = waitForPackets(&server, 1u, std::chrono::milliseconds(250));
    expect(immediatePackets.size() == 1u &&
               immediatePackets.front().header.kind == net::PacketKind::ControlCommandBundle,
           "proxy should fork a clean control command bundle immediately before the manipulated relay");

    transport.tick(1'100'001u);
    const auto delayedPackets = waitForPackets(&server, 1u, std::chrono::milliseconds(250));
    expect(delayedPackets.size() == 1u &&
               delayedPackets.front().header.kind == net::PacketKind::CommandBundle,
           "proxy should still deliver the manipulated gameplay command through the configured upstream delay");
}

}  // namespace

int main() {
    try {
        static_assert(std::is_base_of_v<net::TransportArtifactAdapter, net::ProxyRuntime>,
                      "ProxyRuntime should remain consumable through the TransportArtifactAdapter boundary");
        testFixedDelayDefersForwardingUntilScheduledTime();
        testFixedSeedLossAndDuplicationAreDeterministic();
        testZeroImpairmentRelayPreservesOrderAndDelivery();
        testPeerSpecificOverridesRemainIsolated();
        testUpstreamCommandsForkImmediateControlStream();
        std::cout << "ProxyRuntimeTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ProxyRuntimeTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
