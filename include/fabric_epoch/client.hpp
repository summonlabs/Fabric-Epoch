// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "fabric_epoch/authority.hpp"
#include "fabric_epoch/epoch.hpp"
#include "fabric_epoch/fence.hpp"
#include "fabric_epoch/identity.hpp"
#include "fabric_epoch/limits.hpp"
#include "fabric_epoch/participant.hpp"
#include "fabric_epoch/protocol.hpp"
#include "fabric_epoch/snapshot.hpp"

namespace fabric_epoch {

struct ClientOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  Limits limits;
  std::uint32_t connect_timeout_milliseconds = 5000;
};

/// Blocking request/response client for the Fabric Epoch protocol. One client
/// owns one connection; calls are serialized internally so a client may be
/// shared between threads.
class CoordinatorClient {
 public:
  explicit CoordinatorClient(ClientOptions options);
  ~CoordinatorClient();

  CoordinatorClient(const CoordinatorClient&) = delete;
  CoordinatorClient& operator=(const CoordinatorClient&) = delete;

  void connect();
  void close();
  [[nodiscard]] bool connected() const;

  [[nodiscard]] HelloAckPayload hello(std::string_view role);

  [[nodiscard]] RegistrationResult register_participant(const RegisterParticipantRequest& request,
                                                        const SessionId& session);
  [[nodiscard]] AuthorityValidation validate_authority(const ValidateAuthorityRequest& request);
  [[nodiscard]] FenceResult fence_worker(const FenceWorkerRequest& request);
  [[nodiscard]] RevokeResult revoke_authority(const RevokeAuthorityRequest& request);
  [[nodiscard]] RetireResult retire_participant(const RetireParticipantRequest& request);
  [[nodiscard]] EpochAdvanceResult advance_epoch(const EpochAdvanceRequest& request);
  [[nodiscard]] EpochState epoch_state();
  [[nodiscard]] std::optional<ParticipantView> query_participant(const ParticipantId& participant);
  [[nodiscard]] AuthoritySnapshot snapshot();
  [[nodiscard]] Explanation explain(const ParticipantId& participant, const WorkerBootId& boot);
  void request_shutdown();

  /// Blocks until the peer closes the connection. Used by long-lived worker
  /// processes that must remain attached until their session ends. The call
  /// never gives up on its own: an idle but healthy session is not a
  /// disconnect. close() and request_shutdown() release it from another
  /// thread; the same thread must not call close() while waiting.
  void wait_for_disconnect();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  mutable std::mutex mutex_;
};

}  // namespace fabric_epoch
