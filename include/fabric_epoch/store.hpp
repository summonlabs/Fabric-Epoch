// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "fabric_epoch/durable.hpp"
#include "fabric_epoch/limits.hpp"

namespace fabric_epoch {

struct StoreOptions {
  std::filesystem::path path;
  Limits limits;
  /// When false, loading a missing store is an error rather than a fresh start.
  bool create_if_missing = true;
  /// When true the store additionally maintains a sidecar watermark file used
  /// to detect rollback of the primary image. Documented limitation: a restore
  /// that replaces both files cannot be detected without external monotonic
  /// storage, which Fabric Epoch 1.0.0 does not implement.
  bool enable_rollback_watermark = true;
};

/// Versioned, integrity-checked, atomically replaced durable store.
///
/// Layout: magic | format version | payload length | payload | SHA-256 trailer.
/// The trailer covers magic, version, length and payload, so a corrupted
/// header field is detected exactly like a corrupted payload byte.
class EpochStore {
 public:
  explicit EpochStore(StoreOptions options);

  [[nodiscard]] bool exists() const;
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return options_.path; }
  [[nodiscard]] std::filesystem::path watermark_path() const;

  /// Loads and fully validates the durable image. Throws EpochError with
  /// category CorruptState / IntegrityFailure / UnsupportedVersion on any
  /// rejection; never returns a partially trusted image.
  [[nodiscard]] DurableState load() const;

  /// Returns an empty initial state for a store that does not exist yet.
  [[nodiscard]] DurableState initial_state() const;

  /// Persists atomically: temp file in the same directory, flush, durability
  /// barrier, then replace. Returns only after the replacement is durable.
  void save(const DurableState& state) const;

  /// Highest epoch ever durably persisted, read from the watermark sidecar.
  [[nodiscard]] CoordinatorEpoch highest_persisted_epoch() const;

  /// Encodes and decodes the durable image without touching the filesystem.
  /// Exposed so integrity and corruption tests can attack the representation
  /// directly, including truncation at every byte position.
  [[nodiscard]] std::vector<std::byte> encode(const DurableState& state) const;
  [[nodiscard]] DurableState decode(std::span<const std::byte> image) const;

 private:
  [[nodiscard]] std::uint64_t read_watermark() const;
  void write_watermark(CoordinatorEpoch epoch) const;

  StoreOptions options_;
};

}  // namespace fabric_epoch
