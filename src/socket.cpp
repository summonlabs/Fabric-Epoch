// Fabric Epoch 1.0.0 - Summon Software Labs
#include "socket.hpp"

#include <array>
#include <mutex>
#include <string>

#include "fabric_epoch/error.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#if defined(_MSC_VER)
// ws2tcpip.h defines an inline WSAGetIPUserMtu() whose error path leaves its
// _Out_ parameter unset. The static analyzer reports C6101 against that
// third-party inline body; Fabric Epoch never calls it and the finding is not
// reachable from first-party code. The suppression is scoped to this one system
// header include and covers no first-party translation unit content.
#pragma warning(push)
#pragma warning(disable : 6101)
#endif
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace fabric_epoch::net {
namespace {

#if defined(_WIN32)
using native_handle = SOCKET;
constexpr native_handle kInvalid = INVALID_SOCKET;
#else
using native_handle = int;
constexpr native_handle kInvalid = -1;
#endif

std::once_flag g_init_flag;

void initialize_once() {
#if defined(_WIN32)
  WSADATA data{};
  if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    throw io_failure("WSAStartup failed");
  }
#endif
}

[[nodiscard]] native_handle to_native(std::intptr_t handle) noexcept {
  return static_cast<native_handle>(handle);
}

void close_native(std::intptr_t handle) noexcept {
#if defined(_WIN32)
  ::closesocket(to_native(handle));
#else
  ::close(to_native(handle));
#endif
}

/// Classifies a socket error as "peer went away" so that it is reported as a
/// clean close rather than an infrastructure failure.
/// True when the last socket error was a receive timeout rather than a peer
/// disconnect or a genuine transport failure.
[[nodiscard]] bool is_timeout_error() noexcept {
#if defined(_WIN32)
  const int code = ::WSAGetLastError();
  return code == WSAETIMEDOUT;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT;
#endif
}

[[nodiscard]] bool is_disconnect_error() noexcept {
#if defined(_WIN32)
  const int code = ::WSAGetLastError();
  return code == WSAECONNRESET || code == WSAECONNABORTED || code == WSAESHUTDOWN ||
         code == WSAENOTCONN || code == WSAEINTR;
#else
  return errno == ECONNRESET || errno == EPIPE || errno == EINTR || errno == ENOTCONN;
#endif
}

}  // namespace

void ensure_initialized() { std::call_once(g_init_flag, initialize_once); }

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = -1; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = -1;
  }
  return *this;
}

bool Socket::valid() const noexcept { return handle_ != -1; }

std::intptr_t Socket::release() noexcept {
  const std::intptr_t handle = handle_;
  handle_ = -1;
  return handle;
}

void Socket::close() noexcept {
  if (handle_ == -1) {
    return;
  }
  const std::intptr_t handle = handle_;
  handle_ = -1;
  // Shutting down in both directions before closing is what releases a thread
  // blocked in recv on another thread. Relying on close() alone is not a
  // portable way to wake a blocked receive.
  ::shutdown(to_native(handle), 2 /* SD_BOTH */);
  close_native(handle);
}

void Socket::set_receive_timeout(std::uint32_t timeout_milliseconds) {
#if defined(_WIN32)
  const DWORD budget = timeout_milliseconds;
  ::setsockopt(to_native(handle_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&budget),
               sizeof(budget));
#else
  timeval budget{};
  budget.tv_sec = static_cast<long>(timeout_milliseconds / 1000u);
  budget.tv_usec = static_cast<long>((timeout_milliseconds % 1000u) * 1000u);
  ::setsockopt(to_native(handle_), SOL_SOCKET, SO_RCVTIMEO, &budget, sizeof(budget));
#endif
}

void Socket::send_all(std::span<const std::byte> data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t remaining = data.size() - offset;
    const int chunk = static_cast<int>(remaining > (1u << 20) ? (1u << 20) : remaining);
    const int sent = ::send(to_native(handle_), reinterpret_cast<const char*>(data.data() + offset), chunk, 0);
    if (sent <= 0) {
      if (is_disconnect_error()) {
        throw io_failure("peer closed the connection while sending");
      }
      throw io_failure("socket send failed");
    }
    offset += static_cast<std::size_t>(sent);
  }
}

bool Socket::recv_exact(std::span<std::byte> data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t remaining = data.size() - offset;
    const int chunk = static_cast<int>(remaining > (1u << 20) ? (1u << 20) : remaining);
    const int got = ::recv(to_native(handle_), reinterpret_cast<char*>(data.data() + offset), chunk, 0);
    if (got == 0) {
      if (offset == 0) {
        return false;
      }
      throw io_failure("peer closed the connection in the middle of a frame");
    }
    if (got < 0) {
      if (is_timeout_error()) {
        if (offset == 0) {
          return false;
        }
        throw protocol_violation("peer stopped in the middle of a frame within the receive budget");
      }
      if (is_disconnect_error()) {
        if (offset == 0) {
          return false;
        }
        throw io_failure("connection was reset in the middle of a frame");
      }
      throw io_failure("socket receive failed");
    }
    offset += static_cast<std::size_t>(got);
  }
  return true;
}

bool Socket::wait_readable(std::uint32_t timeout_milliseconds) {
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(to_native(handle_), &readable);
  timeval budget{};
  budget.tv_sec = static_cast<long>(timeout_milliseconds / 1000u);
  budget.tv_usec = static_cast<long>((timeout_milliseconds % 1000u) * 1000u);
  const int ready = ::select(static_cast<int>(handle_) + 1, &readable, nullptr, nullptr, &budget);
  return ready > 0;
}

Listener::~Listener() { close(); }

void Listener::bind_and_listen(const std::string& address, std::uint16_t port, std::size_t backlog) {
  ensure_initialized();
  close();
  const native_handle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == kInvalid) {
    throw io_failure("cannot create a listening socket");
  }
  handle_ = static_cast<std::intptr_t>(handle);

  int reuse = 1;
  ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = ::htons(port);
  if (::inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
    close();
    throw invalid_argument("bind address is not a valid IPv4 literal");
  }
  if (::bind(handle, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
    close();
    throw io_failure("cannot bind the coordinator listening socket");
  }
  if (::listen(handle, static_cast<int>(backlog)) != 0) {
    close();
    throw io_failure("cannot listen on the coordinator socket");
  }
  sockaddr_in actual{};
#if defined(_WIN32)
  int actual_length = sizeof(actual);
#else
  socklen_t actual_length = sizeof(actual);
#endif
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&actual), &actual_length) != 0) {
    close();
    throw io_failure("cannot resolve the bound coordinator port");
  }
  bound_port_ = ::ntohs(actual.sin_port);
}

Socket Listener::accept_one(std::uint32_t wait_milliseconds) {
  if (handle_ == -1) {
    return Socket{};
  }
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(to_native(handle_), &readable);
  timeval budget{};
  budget.tv_sec = static_cast<long>(wait_milliseconds / 1000u);
  budget.tv_usec = static_cast<long>((wait_milliseconds % 1000u) * 1000u);
  if (::select(static_cast<int>(handle_) + 1, &readable, nullptr, nullptr, &budget) <= 0) {
    return Socket{};
  }
  const native_handle accepted = ::accept(to_native(handle_), nullptr, nullptr);
  if (accepted == kInvalid) {
    return Socket{};
  }
  int no_delay = 1;
  ::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay),
               sizeof(no_delay));
  return Socket(static_cast<std::intptr_t>(accepted));
}

void Listener::shutdown() noexcept {
  if (handle_ == -1) {
    return;
  }
  ::shutdown(to_native(handle_), 2 /* SD_BOTH */);
}

void Listener::close() noexcept {
  if (handle_ == -1) {
    return;
  }
  const std::intptr_t handle = handle_;
  handle_ = -1;
  ::shutdown(to_native(handle), 2 /* SD_BOTH */);
  close_native(handle);
}

Socket connect_to(const std::string& host, std::uint16_t port, std::uint32_t timeout_milliseconds) {
  ensure_initialized();
  const native_handle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == kInvalid) {
    throw io_failure("cannot create a client socket");
  }
  Socket client(static_cast<std::intptr_t>(handle));

  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = ::htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &endpoint.sin_addr) != 1) {
    throw invalid_argument("connect host is not a valid IPv4 literal");
  }

#if defined(_WIN32)
  u_long non_blocking = 1;
  ::ioctlsocket(handle, FIONBIO, &non_blocking);
#else
  const int flags = ::fcntl(handle, F_GETFL, 0);
  ::fcntl(handle, F_SETFL, flags | O_NONBLOCK);
#endif

  const int result = ::connect(handle, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
  bool connected = result == 0;
  if (!connected) {
#if defined(_WIN32)
    const int error = ::WSAGetLastError();
    const bool in_progress = error == WSAEWOULDBLOCK || error == WSAEINPROGRESS ||
                             error == WSAEINVAL || error == WSAENOTCONN;
#else
    const bool in_progress = errno == EINPROGRESS || errno == EWOULDBLOCK;
#endif
    if (!in_progress) {
      throw io_failure("coordinator connection was refused");
    }
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(handle, &writable);
    timeval budget{};
    budget.tv_sec = static_cast<long>(timeout_milliseconds / 1000u);
    budget.tv_usec = static_cast<long>((timeout_milliseconds % 1000u) * 1000u);
    const int ready = ::select(static_cast<int>(handle) + 1, nullptr, &writable, nullptr, &budget);
    if (ready <= 0) {
      throw io_failure("coordinator connection timed out");
    }
    int socket_error = 0;
#if defined(_WIN32)
    int length = sizeof(socket_error);
#else
    socklen_t length = sizeof(socket_error);
#endif
    if (::getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error), &length) != 0) {
      throw io_failure("cannot determine the coordinator connection status");
    }
    if (socket_error != 0) {
      throw io_failure("coordinator connection failed");
    }
  }

#if defined(_WIN32)
  non_blocking = 0;
  ::ioctlsocket(handle, FIONBIO, &non_blocking);
#else
  ::fcntl(handle, F_SETFL, flags);
#endif
  int no_delay = 1;
  ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
  return client;
}

}  // namespace fabric_epoch::net
