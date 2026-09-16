// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/detail/encoding.hpp"

#include <limits>

#include "fabric_epoch/error.hpp"
#include "fabric_epoch/limits.hpp"

namespace fabric_epoch::detail {
namespace {

/// Absolute ceiling for any single decoded allocation, independent of the
/// configured limits. Prevents absurd counts from reaching the allocator even
/// when a caller supplies a permissive Limits value.
constexpr std::uint32_t kAbsoluteMaxElements = 4u * 1024u * 1024u;
constexpr std::uint32_t kAbsoluteMaxBytes = 64u * 1024u * 1024u;

}  // namespace

void ByteWriter::u8(std::uint8_t value) { buffer_.push_back(static_cast<std::byte>(value)); }

void ByteWriter::u16(std::uint16_t value) {
  u8(static_cast<std::uint8_t>(value & 0xFFu));
  u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void ByteWriter::u32(std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void ByteWriter::u64(std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void ByteWriter::raw(std::span<const std::byte> bytes) {
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::fixed16(const std::array<std::byte, 16>& bytes) { raw(std::span<const std::byte>(bytes)); }

void ByteWriter::fixed32(const std::array<std::byte, 32>& bytes) { raw(std::span<const std::byte>(bytes)); }

void ByteWriter::string(std::string_view text) {
  if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw resource_limit("string too large to encode");
  }
  u32(static_cast<std::uint32_t>(text.size()));
  raw(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

void ByteReader::require(std::size_t count) const {
  if (count > remaining()) {
    throw malformed("truncated input while decoding");
  }
}

std::uint8_t ByteReader::u8() {
  require(1);
  return static_cast<std::uint8_t>(data_[offset_++]);
}

std::uint16_t ByteReader::u16() {
  std::uint16_t value = 0;
  for (int shift = 0; shift < 16; shift += 8) {
    value = static_cast<std::uint16_t>(value | (static_cast<std::uint16_t>(u8()) << shift));
  }
  return value;
}

std::uint32_t ByteReader::u32() {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= (static_cast<std::uint32_t>(u8()) << shift);
  }
  return value;
}

std::uint64_t ByteReader::u64() {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= (static_cast<std::uint64_t>(u8()) << shift);
  }
  return value;
}

std::array<std::byte, 16> ByteReader::fixed16() {
  require(16);
  std::array<std::byte, 16> out{};
  for (std::size_t i = 0; i < 16; ++i) {
    out[i] = data_[offset_ + i];
  }
  offset_ += 16;
  return out;
}

std::array<std::byte, 32> ByteReader::fixed32() {
  require(32);
  std::array<std::byte, 32> out{};
  for (std::size_t i = 0; i < 32; ++i) {
    out[i] = data_[offset_ + i];
  }
  offset_ += 32;
  return out;
}

std::string ByteReader::string(const Limits& limits, std::size_t max_length) {
  const std::uint32_t declared = u32();
  if (declared > kAbsoluteMaxBytes) {
    throw resource_limit("declared string length exceeds absolute ceiling");
  }
  if (declared > max_length) {
    throw resource_limit("declared string length exceeds requested maximum");
  }
  if (declared > limits.max_string_size) {
    throw resource_limit("declared string length exceeds limits.max_string_size");
  }
  require(declared);
  std::string out(reinterpret_cast<const char*>(data_.data() + offset_), declared);
  offset_ += declared;
  return out;
}

std::vector<std::byte> ByteReader::raw_bytes(std::size_t count) {
  if (count > kAbsoluteMaxBytes) {
    throw resource_limit("declared byte count exceeds absolute ceiling");
  }
  require(count);
  std::vector<std::byte> out(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
                             data_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
  offset_ += count;
  return out;
}

void ByteReader::require_end() const {
  if (!at_end()) {
    throw malformed("trailing bytes after decoded record");
  }
}

}  // namespace fabric_epoch::detail
