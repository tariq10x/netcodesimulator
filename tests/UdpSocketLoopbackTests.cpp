#include "net/UdpSocket.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
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

void testLoopbackExchange() {
    net::UdpSocket sender;
    net::UdpSocket receiver;

    expect(sender.bind({"127.0.0.1", 0}), "sender should bind to an ephemeral loopback port");
    expect(receiver.bind({"127.0.0.1", 0}), "receiver should bind to an ephemeral loopback port");

    const net::ByteBuffer payload{1u, 2u, 3u, 4u};
    expect(sender.sendTo({"127.0.0.1", receiver.localPort()}, payload), "sender should transmit loopback datagram");

    net::ReceivedDatagram datagram;
    const bool received = waitForPredicate([&]() {
        return receiver.receive(&datagram) == net::ReceiveStatus::Received;
    }, std::chrono::milliseconds(250));

    expect(received, "receiver should get the loopback datagram");
    expect(datagram.payload == payload, "receiver payload should match the sent datagram");
    expect(datagram.sender.port == sender.localPort(), "sender port should round-trip through recvfrom");
}

void testDatagramsStayDatagrams() {
    net::UdpSocket sender;
    net::UdpSocket receiver;

    expect(sender.bind({"127.0.0.1", 0}), "sender should bind for datagram test");
    expect(receiver.bind({"127.0.0.1", 0}), "receiver should bind for datagram test");

    const net::ByteBuffer first{10u, 20u, 30u};
    const net::ByteBuffer second{40u, 50u};

    expect(sender.sendTo({"127.0.0.1", receiver.localPort()}, first), "first datagram should send");
    expect(sender.sendTo({"127.0.0.1", receiver.localPort()}, second), "second datagram should send");

    net::ReceivedDatagram firstReceived;
    net::ReceivedDatagram secondReceived;

    const bool gotFirst = waitForPredicate([&]() {
        return receiver.receive(&firstReceived) == net::ReceiveStatus::Received;
    }, std::chrono::milliseconds(250));
    const bool gotSecond = waitForPredicate([&]() {
        return receiver.receive(&secondReceived) == net::ReceiveStatus::Received;
    }, std::chrono::milliseconds(250));

    expect(gotFirst && gotSecond, "receiver should get both datagrams independently");
    expect(firstReceived.payload == first, "first payload should stay intact");
    expect(secondReceived.payload == second, "second payload should stay intact");
}

void testWouldBlockIsNonBlocking() {
    net::UdpSocket socket;
    expect(socket.bind({"127.0.0.1", 0}), "test socket should bind successfully");

    net::ReceivedDatagram datagram;
    const auto start = std::chrono::steady_clock::now();
    const net::ReceiveStatus status = socket.receive(&datagram);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    expect(status == net::ReceiveStatus::WouldBlock, "empty non-blocking socket should report WouldBlock");
    expect(elapsed < std::chrono::milliseconds(50), "WouldBlock receive should return quickly");
}

}  // namespace

int main() {
    try {
        testLoopbackExchange();
        testDatagramsStayDatagrams();
        testWouldBlockIsNonBlocking();
        std::cout << "UdpSocketLoopbackTests: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "UdpSocketLoopbackTests: FAIL - " << ex.what() << '\n';
        return 1;
    }
}
