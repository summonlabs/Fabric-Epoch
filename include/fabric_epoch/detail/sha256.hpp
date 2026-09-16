// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fabric_epoch::detail {

/// Minimal, dependency-free SHA-256 used for durable integrity, wire integrity
/// and deterministic semantic digests. It is an integrity mechanism, not an
/// authentication mechanism: Fabric Epoch 1.0.0 makes no cryptographic
/// peer-authentication claim.
class Sha256 {
 public:
  using digest_type = std::array<std::byte, 32>;
  static constexpr std::size_t block_size = 64;

  Sha256() noexcept;

  void update(std::span<const std::byte> data) noexcept;
  void update(std::string_view text) noexcept;
  void update_byte(std::uint8_t value) noexcept;

  [[nodiscard]] digest_type finish() noexcept;

  [[nodiscard]] static digest_type hash(std::span<const std::byte> data) noexcept;
  [[nodiscard]] static digest_type hash(std::string_view text) noexcept;

 private:
  /// The parameter type is a fixed-size array reference on purpose: it makes the
  /// 64-byte block contract explicit to the compiler and to static analysis
  /// instead of relying on a bare pointer.
  void compress(const std::uint8_t (&block)[block_size]) noexcept;

  std::array<std::uint32_t, 8> state_;
  std::uint8_t buffer_[block_size]{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffer_size_ = 0;
};

}  // namespace fabric_epoch::detail
