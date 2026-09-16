// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace fabric_epoch {

/// Deterministic 256-bit semantic digest. Computed over canonicalized semantic
/// authority state only: process-local timestamps, sockets, thread identifiers,
/// addresses, arrival order and non-semantic diagnostics are excluded.
class SnapshotDigest {
 public:
  using storage_type = std::array<std::byte, 32>;

  constexpr SnapshotDigest() noexcept = default;

  [[nodiscard]] static SnapshotDigest from_bytes(const storage_type& bytes) noexcept {
    SnapshotDigest digest;
    digest.bytes_ = bytes;
    return digest;
  }

  [[nodiscard]] const storage_type& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const SnapshotDigest&, const SnapshotDigest&) noexcept = default;

 private:
  storage_type bytes_{};
};

}  // namespace fabric_epoch
