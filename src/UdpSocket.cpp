#include "net/UdpSocket.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace net {
namespace {

#if defined(_WIN32)
using SocketLength = int;

class WinsockSession {
  public:
    WinsockSession() {
        WSADATA data{};
        initialized_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }

    ~WinsockSession() {
        if (initialized_) {
            WSACleanup();
        }
    }

    bool initialized() const {
        return initialized_;
    }

  private:
    bool initialized_{false};
};

bool ensureWinsockInitialized() {
    static WinsockSession session;
    return session.initialized();
}
#else
using SocketLength = socklen_t;
using SOCKET = int;
#endif

std::string lastSystemErrorString() {
#if defined(_WIN32)
    return "winsock error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

bool fillSockaddr(const UdpEndpoint& endpoint, sockaddr_storage* storage,
                  SocketLength* storageSize) {
    if (storage == nullptr || storageSize == nullptr) {
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* resolved = nullptr;
    const std::string portString = std::to_string(endpoint.port);
    const int result = getaddrinfo(endpoint.host.c_str(), portString.c_str(), &hints, &resolved);
    if (result != 0 || resolved == nullptr) {
        return false;
    }

    std::memcpy(storage, resolved->ai_addr, resolved->ai_addrlen);
    *storageSize = static_cast<SocketLength>(resolved->ai_addrlen);
    freeaddrinfo(resolved);
    return true;
}

UdpEndpoint endpointFromSockaddr(const sockaddr_storage& storage, SocketLength storageSize) {
    char host[NI_MAXHOST] = {0};
    char service[NI_MAXSERV] = {0};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&storage), storageSize, host, sizeof(host),
                    service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return UdpEndpoint{};
    }

    return UdpEndpoint{host, static_cast<std::uint16_t>(std::stoi(service))};
}

} // namespace

UdpSocket::UdpSocket() = default;

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : socketFd_(std::exchange(other.socketFd_, kInvalidSocket)),
      localPort_(std::exchange(other.localPort_, std::uint16_t{0})),
      lastError_(std::move(other.lastError_)) {}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        socketFd_ = std::exchange(other.socketFd_, kInvalidSocket);
        localPort_ = std::exchange(other.localPort_, std::uint16_t{0});
        lastError_ = std::move(other.lastError_);
    }
    return *this;
}

bool UdpSocket::bind(const UdpEndpoint& endpoint) {
    close();
    if (!openSocket()) {
        return false;
    }

    sockaddr_storage storage{};
    SocketLength storageSize = 0;
    if (!fillSockaddr(endpoint, &storage, &storageSize)) {
        setError("failed to resolve bind endpoint");
        close();
        return false;
    }

    const int reuse = 1;
    const SOCKET socket = static_cast<SOCKET>(socketFd_);
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));

    if (::bind(socket, reinterpret_cast<const sockaddr*>(&storage), storageSize) != 0) {
        setError("bind failed: " + lastSystemErrorString());
        close();
        return false;
    }

    sockaddr_storage boundAddress{};
    SocketLength boundSize = sizeof(boundAddress);
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&boundAddress), &boundSize) != 0) {
        setError("getsockname failed: " + lastSystemErrorString());
        close();
        return false;
    }

    localPort_ = endpointFromSockaddr(boundAddress, boundSize).port;
    return true;
}

bool UdpSocket::sendTo(const UdpEndpoint& endpoint, const ByteBuffer& payload) {
    if (!isOpen()) {
        setError("socket not open");
        return false;
    }

    sockaddr_storage storage{};
    SocketLength storageSize = 0;
    if (!fillSockaddr(endpoint, &storage, &storageSize)) {
        setError("failed to resolve destination endpoint");
        return false;
    }
    if (payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        setError("payload is too large for a UDP send");
        return false;
    }

    const SOCKET socket = static_cast<SOCKET>(socketFd_);
    const auto bytesSent = ::sendto(socket, reinterpret_cast<const char*>(payload.data()),
                                    static_cast<int>(payload.size()), 0,
                                    reinterpret_cast<const sockaddr*>(&storage), storageSize);
    if (bytesSent < 0 || static_cast<std::size_t>(bytesSent) != payload.size()) {
        setError("sendto failed: " + lastSystemErrorString());
        return false;
    }

    return true;
}

ReceiveStatus UdpSocket::receive(ReceivedDatagram* datagram) {
    if (!isOpen()) {
        setError("socket not open");
        return ReceiveStatus::Error;
    }
    if (datagram == nullptr) {
        setError("datagram output is null");
        return ReceiveStatus::Error;
    }

    std::array<std::uint8_t, 2048> buffer{};
    sockaddr_storage from{};
    SocketLength fromSize = sizeof(from);
    const SOCKET socket = static_cast<SOCKET>(socketFd_);
    const auto bytesRead =
        ::recvfrom(socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()),
                   0, reinterpret_cast<sockaddr*>(&from), &fromSize);
    if (bytesRead < 0) {
#if defined(_WIN32)
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            return ReceiveStatus::WouldBlock;
        }
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ReceiveStatus::WouldBlock;
        }
#endif
        setError("recvfrom failed: " + lastSystemErrorString());
        return ReceiveStatus::Error;
    }

    datagram->payload.assign(buffer.begin(), buffer.begin() + bytesRead);
    datagram->sender = endpointFromSockaddr(from, fromSize);
    return ReceiveStatus::Received;
}

bool UdpSocket::isOpen() const {
    return socketFd_ != kInvalidSocket;
}

std::uint16_t UdpSocket::localPort() const {
    return localPort_;
}

const std::string& UdpSocket::lastError() const {
    return lastError_;
}

void UdpSocket::close() {
    if (socketFd_ != kInvalidSocket) {
#if defined(_WIN32)
        closesocket(static_cast<SOCKET>(socketFd_));
#else
        ::close(socketFd_);
#endif
        socketFd_ = kInvalidSocket;
    }
    localPort_ = 0;
}

bool UdpSocket::openSocket() {
#if defined(_WIN32)
    if (!ensureWinsockInitialized()) {
        setError("winsock initialization failed");
        return false;
    }
#endif

    socketFd_ = static_cast<SocketHandle>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (socketFd_ == kInvalidSocket) {
        setError("socket creation failed: " + lastSystemErrorString());
        return false;
    }

#if defined(_WIN32)
    u_long nonBlocking = 1;
    if (ioctlsocket(static_cast<SOCKET>(socketFd_), FIONBIO, &nonBlocking) != 0) {
        setError("failed to enable non-blocking mode: " + lastSystemErrorString());
        close();
        return false;
    }

    BOOL udpConnectionReset = FALSE;
    DWORD bytesReturned = 0;
    WSAIoctl(static_cast<SOCKET>(socketFd_),
             SIO_UDP_CONNRESET,
             &udpConnectionReset,
             sizeof(udpConnectionReset),
             nullptr,
             0,
             &bytesReturned,
             nullptr,
             nullptr);
#else
    const int flags = fcntl(socketFd_, F_GETFL, 0);
    if (flags < 0 || fcntl(socketFd_, F_SETFL, flags | O_NONBLOCK) != 0) {
        setError("failed to enable non-blocking mode: " + lastSystemErrorString());
        close();
        return false;
    }
#endif

    return true;
}

void UdpSocket::setError(const std::string& message) {
    lastError_ = message;
}

} // namespace net
