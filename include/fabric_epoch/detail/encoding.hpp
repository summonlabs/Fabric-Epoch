// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fabric_epoch {

struct Limits;

namespace detail {

/// Deterministic little-endian writer. Lengths are explicit; no raw object
/// layout is ever serialized.
class ByteWriter {
 public:
  ByteWriter() = default;

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void raw(std::span<const std::byte> bytes);
  void fixed16(const std::array<std::byte, 16>& bytes);
  void fixed32(const std::array<std::byte, 32>& bytes);
  /// Length-prefixed byte string (u32 length then raw bytes).
  void string(std::string_view text);

  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return buffer_; }
  [[nodiscard]] std::span<const std::byte> span() const noexcept {
    return std::span<const std::byte>(buffer_.data(), buffer_.size());
  }
  [[nodiscard]] std::vector<std::byte> take() { return std::move(buffer_); }

 private:
  std::vector<std::byte> buffer_;
};

/// Bounded, strict reader. Every read is range-checked; declared lengths are
/// validated against the remaining input *and* the configured limit before any
/// allocation proportional to the declared length occurs.
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

  [[nodiscard]] std::uint8_t u8();
  [[nodiscard]] std::uint16_t u16();
  [[nodiscard]] std::uint32_t u32();
  [[nodiscard]] std::uint64_t u64();
  [[nodiscard]] std::array<std::byte, 16> fixed16();
  [[nodiscard]] std::array<std::byte, 32> fixed32();
  [[nodiscard]] std::string string(const Limits& limits, std::size_t max_length);
  [[nodiscard]] std::vector<std::byte> raw_bytes(std::size_t count);

  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

  /// Strict trailing-byte rejection.
  void require_end() const;

 private:
  void require(std::size_t count) const;

  std::span<const std::byte> data_;
  std::size_t offset_ = 0;
};

}  // namespace detail
}  // namespace fabric_epoch
