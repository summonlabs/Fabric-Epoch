// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fabric_epoch {

namespace detail {

/// Identifier text rules: 1..max_length characters, ASCII alphanumeric plus
/// '.', '_', ':', '-'. No leading or trailing separator, no whitespace, no
/// control characters, no non-ASCII bytes.
[[nodiscard]] bool is_valid_identifier_text(std::string_view text, std::size_t max_length) noexcept;

/// Fills 16 bytes from the operating-system CSPRNG. Throws EpochError when no
/// entropy source is usable. These identifiers are uniqueness tokens, not
/// authentication secrets.
void fill_random_16(std::byte* out);

[[nodiscard]] std::string to_hex(const std::byte* data, std::size_t size);

/// Canonical 128-bit identifier text: exactly 32 lowercase hex characters.
[[nodiscard]] std::optional<std::array<std::byte, 16>> parse_hex_16(std::string_view text) noexcept;

}  // namespace detail

/// Tag type used to keep 128-bit identifiers from different domains distinct.
struct CoordinatorBootIdTag {};
struct WorkerBootIdTag {};
struct SessionIdTag {};
struct AuthorityGrantIdTag {};
struct EpochTransitionIdTag {};
struct EpochSnapshotIdTag {};
struct MutationAttemptIdTag {};
struct IntegrityTagTag {};

template <typename Tag>
class Fixed128Id {
 public:
  using storage_type = std::array<std::byte, 16>;

  constexpr Fixed128Id() noexcept = default;

  [[nodiscard]] static Fixed128Id from_bytes(const storage_type& bytes) noexcept {
    Fixed128Id id;
    id.bytes_ = bytes;
    return id;
  }

  /// Parses exactly 32 hexadecimal characters, upper or lower case.
  [[nodiscard]] static std::optional<Fixed128Id> parse(std::string_view text) noexcept {
    const auto parsed = detail::parse_hex_16(text);
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    return from_bytes(*parsed);
  }

  /// Generates a fresh identifier from the operating-system CSPRNG.
  [[nodiscard]] static Fixed128Id generate() {
    Fixed128Id id;
    detail::fill_random_16(id.bytes_.data());
    return id;
  }

  [[nodiscard]] const storage_type& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::span<const std::byte, 16> span() const noexcept {
    return std::span<const std::byte, 16>(bytes_.data(), bytes_.size());
  }

  [[nodiscard]] bool is_nil() const noexcept {
    for (const std::byte b : bytes_) {
      if (b != std::byte{0}) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::string to_string() const { return detail::to_hex(bytes_.data(), bytes_.size()); }

  friend bool operator==(const Fixed128Id&, const Fixed128Id&) noexcept = default;
  friend std::strong_ordering operator<=>(const Fixed128Id& lhs, const Fixed128Id& rhs) noexcept {
    return lhs.bytes_ <=> rhs.bytes_;
  }

 private:
  storage_type bytes_{};
};

using CoordinatorBootId = Fixed128Id<CoordinatorBootIdTag>;
using WorkerBootId = Fixed128Id<WorkerBootIdTag>;
using SessionId = Fixed128Id<SessionIdTag>;
using AuthorityGrantId = Fixed128Id<AuthorityGrantIdTag>;
using EpochTransitionId = Fixed128Id<EpochTransitionIdTag>;
using EpochSnapshotId = Fixed128Id<EpochSnapshotIdTag>;
using MutationAttemptId = Fixed128Id<MutationAttemptIdTag>;

/// Stable participant identity. Owned by Fabric Registry at the stack level;
/// Fabric Epoch consumes it and never invents a competing identity.
class ParticipantId {
 public:
  static constexpr std::size_t max_length = 128;

  constexpr ParticipantId() noexcept = default;

  [[nodiscard]] static std::optional<ParticipantId> parse(std::string_view text) {
    if (!detail::is_valid_identifier_text(text, max_length)) {
      return std::nullopt;
    }
    ParticipantId id;
    id.text_.assign(text);
    return id;
  }

  [[nodiscard]] static ParticipantId from_validated(std::string text) {
    ParticipantId id;
    id.text_ = std::move(text);
    return id;
  }

  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
  [[nodiscard]] const std::string& value() const noexcept { return text_; }

  [[nodiscard]] std::string to_string() const { return text_; }

  friend bool operator==(const ParticipantId&, const ParticipantId&) noexcept = default;
  friend std::strong_ordering operator<=>(const ParticipantId& lhs, const ParticipantId& rhs) noexcept {
    return lhs.text_ <=> rhs.text_;
  }

 private:
  std::string text_;
};

/// Publisher identity as used by the publication path. Deliberately a distinct
/// type from ParticipantId so that the two cannot be interchanged implicitly;
/// conversion is explicit and single-directional.
class PublisherId {
 public:
  static constexpr std::size_t max_length = 128;

  constexpr PublisherId() noexcept = default;

  [[nodiscard]] static std::optional<PublisherId> parse(std::string_view text) {
    if (!detail::is_valid_identifier_text(text, max_length)) {
      return std::nullopt;
    }
    PublisherId id;
    id.text_.assign(text);
    return id;
  }

  [[nodiscard]] static PublisherId from_participant(const ParticipantId& participant) {
    PublisherId id;
    id.text_ = participant.value();
    return id;
  }

  [[nodiscard]] ParticipantId as_participant() const {
    return ParticipantId::from_validated(text_);
  }

  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
  [[nodiscard]] const std::string& value() const noexcept { return text_; }
  [[nodiscard]] std::string to_string() const { return text_; }

  friend bool operator==(const PublisherId&, const PublisherId&) noexcept = default;
  friend std::strong_ordering operator<=>(const PublisherId& lhs, const PublisherId& rhs) noexcept {
    return lhs.text_ <=> rhs.text_;
  }

 private:
  std::string text_;
};

/// Tag for monotonic 64-bit generation counters.
struct CoordinatorEpochTag {};
struct EpochGenerationTag {};
struct RegistrationGenerationTag {};
struct FencingGenerationTag {};
struct GrantGenerationTag {};
struct IncarnationSequenceTag {};

/// Monotonic 64-bit counter with checked arithmetic. Wrapping is never
/// permitted: incrementing the maximum representable value fails explicitly.
template <typename Tag>
class MonotonicCounter {
 public:
  using value_type = std::uint64_t;
  static constexpr value_type max_value = UINT64_MAX;

  constexpr MonotonicCounter() noexcept = default;
  explicit constexpr MonotonicCounter(value_type value) noexcept : value_(value) {}

  [[nodiscard]] constexpr value_type value() const noexcept { return value_; }

  /// Returns std::nullopt on overflow instead of wrapping.
  [[nodiscard]] constexpr std::optional<MonotonicCounter> next() const noexcept {
    if (value_ == max_value) {
      return std::nullopt;
    }
    return MonotonicCounter(value_ + 1);
  }

  [[nodiscard]] constexpr bool is_max() const noexcept { return value_ == max_value; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

  /// Checked addition; returns std::nullopt on overflow.
  [[nodiscard]] constexpr std::optional<MonotonicCounter> plus(value_type delta) const noexcept {
    if (delta > max_value - value_) {
      return std::nullopt;
    }
    return MonotonicCounter(value_ + delta);
  }

  [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

  friend constexpr bool operator==(const MonotonicCounter&, const MonotonicCounter&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const MonotonicCounter& lhs,
                                                    const MonotonicCounter& rhs) noexcept {
    return lhs.value_ <=> rhs.value_;
  }

 private:
  value_type value_ = 0;
};

/// Authoritative generation of the control plane.
using CoordinatorEpoch = MonotonicCounter<CoordinatorEpochTag>;
/// Generation of an epoch snapshot.
using EpochGeneration = MonotonicCounter<EpochGenerationTag>;
/// Generation of a participant registration.
using RegistrationGeneration = MonotonicCounter<RegistrationGenerationTag>;
/// Generation of fencing activity for a participant.
using FencingGeneration = MonotonicCounter<FencingGenerationTag>;
/// Generation of an authority grant.
using GrantGeneration = MonotonicCounter<GrantGenerationTag>;
/// Monotonic ordinal of a worker incarnation within one participant.
using IncarnationSequence = MonotonicCounter<IncarnationSequenceTag>;

/// A bounded, validated free-form string (provenance, operator note).
class BoundedText {
 public:
  constexpr BoundedText() noexcept = default;

  [[nodiscard]] static bool is_valid(std::string_view text, std::size_t max_length) noexcept {
    if (text.size() > max_length) {
      return false;
    }
    for (const char c : text) {
      const unsigned char uc = static_cast<unsigned char>(c);
      if (uc < 0x20u && uc != '\t') {
        return false;
      }
      if (uc == 0x7Fu) {
        return false;
      }
      if (uc > 0x7Eu) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] static BoundedText from_validated(std::string text) {
    BoundedText out;
    out.text_ = std::move(text);
    return out;
  }

  [[nodiscard]] const std::string& value() const noexcept { return text_; }
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

  friend bool operator==(const BoundedText&, const BoundedText&) noexcept = default;

 private:
  std::string text_;
};

}  // namespace fabric_epoch
