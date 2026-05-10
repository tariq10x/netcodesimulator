#pragma once

#include <cstdint>
#include <string>

#include "net/Protocol.hpp"

namespace net {

struct UdpEndpoint {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
};

struct ReceivedDatagram {
    ByteBuffer payload{};
    UdpEndpoint sender{};
};

enum class ReceiveStatus { Received, WouldBlock, Error };

class UdpSocket {
  public:
    // Platform adapter for non-blocking UDP datagram I/O only.
    // It must not own gameplay, study-policy, or session semantics.
    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    bool bind(const UdpEndpoint& endpoint);
    bool sendTo(const UdpEndpoint& endpoint, const ByteBuffer& payload);
    ReceiveStatus receive(ReceivedDatagram* datagram);

    bool isOpen() const;
    std::uint16_t localPort() const;
    const std::string& lastError() const;

  private:
#if defined(_WIN32)
    using SocketHandle = std::uintptr_t;
    static constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(~SocketHandle{0});
#else
    using SocketHandle = int;
    static constexpr SocketHandle kInvalidSocket = -1;
#endif

    void close();
    bool openSocket();
    void setError(const std::string& message);

    SocketHandle socketFd_{kInvalidSocket};
    std::uint16_t localPort_{0};
    std::string lastError_{};
};

} // namespace net
