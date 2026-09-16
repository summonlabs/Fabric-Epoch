// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "fabric_epoch/limits.hpp"
#include "fabric_epoch/runtime.hpp"

namespace fabric_epoch {

struct CoordinatorServerOptions {
  std::string bind_address = "127.0.0.1";
  /// 0 requests an ephemeral port; the bound port is reported by port().
  std::uint16_t port = 0;
  Limits limits;
  std::string product_version;
  /// Wait budget for the receive path to observe a required condition. This is
  /// not a test timeout: exceeding it produces an explicit structured failure.
  std::uint32_t io_wait_milliseconds = 250;

  /// Budget for a peer to deliver one complete frame once it has started one.
  /// A peer that stops mid-frame is disconnected with a protocol violation;
  /// this is slow-peer defence, not a test timeout.
  std::uint32_t frame_receive_budget_milliseconds = 2000;
};

/// Single-authority coordinator transport. Fabric Epoch 1.0.0 is explicitly
/// single-authority: this server is the only process permitted to mutate the
/// durable epoch, and no distributed consensus is implemented. A standby
/// process can only become authoritative by advancing the durable epoch.
///
/// Transport security: plain TCP, intended for loopback or an already trusted
/// fabric. Integrity checking detects corruption, not forgery. No
/// cryptographic peer authentication is implemented.
class CoordinatorServer {
 public:
  CoordinatorServer(EpochRuntime& runtime, CoordinatorServerOptions options);
  ~CoordinatorServer();

  CoordinatorServer(const CoordinatorServer&) = delete;
  CoordinatorServer& operator=(const CoordinatorServer&) = delete;

  /// Binds and starts accepting. Throws EpochError(IoFailure) if the address
  /// cannot be bound.
  void start();

  /// Idempotent, non-self-joining shutdown. Safe to call from any thread,
  /// including a session thread.
  void stop();

  [[nodiscard]] std::uint16_t port() const;
  [[nodiscard]] std::size_t active_session_count() const;

  /// True once a peer has requested an orderly coordinator shutdown.
  [[nodiscard]] bool shutdown_requested() const;

  /// Blocks until a peer requests an orderly shutdown. The caller then calls
  /// stop() from a non-session thread: stop() never joins the calling thread,
  /// so a session handler must not call it.
  void wait_for_shutdown_request();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fabric_epoch
