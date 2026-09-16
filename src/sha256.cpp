// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/detail/sha256.hpp"

#include <algorithm>
#include <cstring>

#if defined(_MSC_VER)
#include <sal.h>
#endif

namespace fabric_epoch::detail {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline std::uint32_t rotr(std::uint32_t value, std::uint32_t count) noexcept {
  return (value >> count) | (value << (32u - count));
}

inline std::uint8_t byte_at(const std::uint8_t (&block)[64], std::size_t index) noexcept {
  return block[index];
}

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

void Sha256::compress(const std::uint8_t (&block)[64]) noexcept {
  std::uint32_t schedule[64];
  for (std::size_t i = 0; i < 16; ++i) {
    schedule[i] = (static_cast<std::uint32_t>(byte_at(block, i * 4)) << 24) |
                  (static_cast<std::uint32_t>(byte_at(block, i * 4 + 1)) << 16) |
                  (static_cast<std::uint32_t>(byte_at(block, i * 4 + 2)) << 8) |
                  (static_cast<std::uint32_t>(byte_at(block, i * 4 + 3)));
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(schedule[i - 15], 7) ^ rotr(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
    const std::uint32_t s1 = rotr(schedule[i - 2], 17) ^ rotr(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + schedule[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::byte> data) noexcept {
  total_bytes_ += static_cast<std::uint64_t>(data.size());
  std::size_t offset = 0;

  // Top up a partially filled staging buffer first.
  if (buffer_size_ != 0) {
    while (offset < data.size() && buffer_size_ < block_size) {
      buffer_[buffer_size_] = std::to_integer<std::uint8_t>(data[offset]);
      ++buffer_size_;
      ++offset;
    }
    if (buffer_size_ == block_size) {
      compress(buffer_);
      buffer_size_ = 0;
    } else {
      // The input was consumed before the block was complete.
      return;
    }
  }

  // Whole blocks are staged through a fixed-size local so the 64-byte contract
  // is expressed in the code rather than asserted in a comment. The copy costs
  // a fraction of the compression it feeds.
  std::uint8_t block[block_size]{};
  while (data.size() - offset >= block_size) {
    for (std::size_t index = 0; index < block_size; ++index) {
      block[index] = std::to_integer<std::uint8_t>(data[offset + index]);
    }
    compress(block);
    offset += block_size;
  }

  // Post-condition of the loop above: fewer than block_size bytes remain, and
  // the staging buffer is empty because it was either empty on entry or just
  // flushed.
  const std::size_t remaining = data.size() - offset;
  if (remaining == 0) {
    return;
  }
  _Analysis_assume_(remaining < block_size);
  for (std::size_t index = 0; index < remaining; ++index) {
    buffer_[index] = std::to_integer<std::uint8_t>(data[offset + index]);
  }
  buffer_size_ = remaining;
}

void Sha256::update(std::string_view text) noexcept {
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

void Sha256::update_byte(std::uint8_t value) noexcept {
  const std::byte single = static_cast<std::byte>(value);
  update(std::span<const std::byte>(&single, 1));
}

Sha256::digest_type Sha256::finish() noexcept {
  const std::uint64_t bit_length = total_bytes_ * 8u;
  const std::byte padding = static_cast<std::byte>(0x80u);
  update(std::span<const std::byte>(&padding, 1));
  const std::byte zero{0};
  while (buffer_size_ != 56) {
    update(std::span<const std::byte>(&zero, 1));
  }
  std::uint8_t length_bytes[8]{};
  for (std::size_t i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<std::uint8_t>((bit_length >> (56u - 8u * i)) & 0xFFu);
  }
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(length_bytes), sizeof(length_bytes)));

  digest_type out{};
  for (std::size_t i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::byte>((state_[i] >> 24) & 0xFFu);
    out[i * 4 + 1] = static_cast<std::byte>((state_[i] >> 16) & 0xFFu);
    out[i * 4 + 2] = static_cast<std::byte>((state_[i] >> 8) & 0xFFu);
    out[i * 4 + 3] = static_cast<std::byte>(state_[i] & 0xFFu);
  }
  return out;
}

Sha256::digest_type Sha256::hash(std::span<const std::byte> data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

Sha256::digest_type Sha256::hash(std::string_view text) noexcept {
  Sha256 hasher;
  hasher.update(text);
  return hasher.finish();
}

}  // namespace fabric_epoch::detail
