// Fabric Epoch 1.0.0 - Summon Software Labs
// Internal blocking socket helpers with deterministic shutdown behaviour.
// Not installed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace fabric_epoch::net {

/// Initializes the platform socket layer once per process.
void ensure_initialized();

/// RAII socket handle. Move-only; closing is idempotent.
class Socket {
 public:
  Socket() = default;
  explicit Socket(std::intptr_t handle) noexcept : handle_(handle) {}
  ~Socket();

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::intptr_t native() const noexcept { return handle_; }

  /// Idempotent close. Shuts the connection down in both directions first so
  /// that a blocked receive on another thread is released, then closes.
  void close() noexcept;

  /// Releases ownership of the handle without closing it.
  [[nodiscard]] std::intptr_t release() noexcept;

  /// Applies a receive budget to this socket. A peer that stops in the middle
  /// of a frame is then released within the budget instead of pinning the
  /// session thread indefinitely.
  void set_receive_timeout(std::uint32_t timeout_milliseconds);

  void send_all(std::span<const std::byte> data);

  /// Reads exactly the requested number of bytes. Returns false when the peer
  /// closed cleanly before any byte of this read arrived. Throws EpochError on
  /// a truncated read or a genuine socket error.
  [[nodiscard]] bool recv_exact(std::span<std::byte> data);

  /// Waits until the socket is readable or the budget expires. Returns true
  /// when readable (data or close), false on timeout. Never blocks beyond the
  /// budget, which is what lets an owning thread observe a stop request.
  [[nodiscard]] bool wait_readable(std::uint32_t timeout_milliseconds);

 private:
  std::intptr_t handle_ = -1;
};

/// Listening socket bound to a loopback or explicitly supplied address.
class Listener {
 public:
  Listener() = default;
  ~Listener();

  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  /// Binds and starts listening. Throws EpochError(IoFailure) on failure.
  void bind_and_listen(const std::string& address, std::uint16_t port, std::size_t backlog);

  /// Waits up to the supplied budget for an inbound connection. Returns an
  /// invalid Socket on timeout. The caller observes its own stop flag between
  /// calls, so the listener handle is only ever touched by its owning thread.
  [[nodiscard]] Socket accept_one(std::uint32_t wait_milliseconds);

  void shutdown() noexcept;
  void close() noexcept;

  [[nodiscard]] std::uint16_t bound_port() const noexcept { return bound_port_; }
  [[nodiscard]] bool valid() const noexcept { return handle_ != -1; }

 private:
  std::intptr_t handle_ = -1;
  std::uint16_t bound_port_ = 0;
};

/// Connects to host:port within the supplied budget. Throws EpochError on
/// timeout or refusal. The budget is product logic: exceeding it produces an
/// explicit structured failure, never a silent hang.
[[nodiscard]] Socket connect_to(const std::string& host, std::uint16_t port,
                                std::uint32_t timeout_milliseconds);

}  // namespace fabric_epoch::net
