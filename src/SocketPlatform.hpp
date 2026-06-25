#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using native_socket_t = SOCKET;
constexpr native_socket_t kInvalidSocket = INVALID_SOCKET;
inline int closeSocket(native_socket_t s) {
    return closesocket(s);
}
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using native_socket_t = int;
constexpr native_socket_t kInvalidSocket = -1;
inline int closeSocket(native_socket_t s) {
    return close(s);
}
#endif

namespace mcls {
namespace detail {

#ifdef _WIN32
struct WinsockInit {
    WinsockInit() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockInit() {
        WSACleanup();
    }
};

inline void ensureWinsock() {
    static WinsockInit init;
}
#else
inline void ensureWinsock() {}
#endif

} // namespace detail
} // namespace mcls
