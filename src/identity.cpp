// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/identity.hpp"

#include <cstdio>

#include "fabric_epoch/error.hpp"

#if defined(_WIN32)
#include <windows.h>

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fstream>
#endif

namespace fabric_epoch::detail {
namespace {

constexpr std::string_view kIdentifierExtras = "._:-";

[[nodiscard]] bool is_ascii_alnum(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] bool is_separator(char c) noexcept {
  return kIdentifierExtras.find(c) != std::string_view::npos;
}

[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

}  // namespace

bool is_valid_identifier_text(std::string_view text, std::size_t max_length) noexcept {
  if (text.empty() || text.size() > max_length) {
    return false;
  }
  if (is_separator(text.front()) || is_separator(text.back())) {
    return false;
  }
  for (const char c : text) {
    if (!is_ascii_alnum(c) && !is_separator(c)) {
      return false;
    }
  }
  return true;
}

void fill_random_16(std::byte* out) {
#if defined(_WIN32)
  const NTSTATUS status =
      ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out), 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status < 0) {
    throw EpochError(ErrorCategory::IoFailure, "BCryptGenRandom failed while generating an identifier");
  }
#else
  std::ifstream source("/dev/urandom", std::ios::binary);
  if (!source) {
    throw EpochError(ErrorCategory::IoFailure, "cannot open /dev/urandom while generating an identifier");
  }
  source.read(reinterpret_cast<char*>(out), 16);
  if (source.gcount() != 16) {
    throw EpochError(ErrorCategory::IoFailure, "short read from /dev/urandom while generating an identifier");
  }
#endif
}

std::string to_hex(const std::byte* data, std::size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    const auto value = static_cast<unsigned>(data[i]);
    out[i * 2] = kDigits[(value >> 4) & 0xFu];
    out[i * 2 + 1] = kDigits[value & 0xFu];
  }
  return out;
}

std::optional<std::array<std::byte, 16>> parse_hex_16(std::string_view text) noexcept {
  if (text.size() != 32) {
    return std::nullopt;
  }
  std::array<std::byte, 16> out{};
  for (std::size_t i = 0; i < 16; ++i) {
    const int hi = hex_value(text[i * 2]);
    const int lo = hex_value(text[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out[i] = static_cast<std::byte>((hi << 4) | lo);
  }
  return out;
}

}  // namespace fabric_epoch::detail
