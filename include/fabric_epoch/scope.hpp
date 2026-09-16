// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fabric_epoch {

/// Dimensions along which authority can be granted.
enum class ScopeKind : std::uint8_t {
  Fabric = 0,
  Site = 1,
  Domain = 2,
  EntityClass = 3,
  Entity = 4,
  OperationFamily = 5,
};

[[nodiscard]] std::string_view to_string(ScopeKind kind) noexcept;
[[nodiscard]] std::optional<ScopeKind> scope_kind_from_wire(std::uint8_t value) noexcept;

/// One exact authority scope: a dimension plus a concrete subject.
///
/// Fabric Epoch 1.0.0 performs *exact* scope matching. It does not resolve
/// containment between scopes (Fabric does not implicitly cover Site) because
/// containment is topology truth owned by other runtimes. There is no wildcard
/// scope kind and an empty subject is not representable.
class AuthorityScope {
 public:
  static constexpr std::size_t max_subject_length = 128;

  constexpr AuthorityScope() noexcept = default;

  [[nodiscard]] static std::optional<AuthorityScope> parse(ScopeKind kind, std::string_view subject);

  [[nodiscard]] ScopeKind kind() const noexcept { return kind_; }
  [[nodiscard]] const std::string& subject() const noexcept { return subject_; }

  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const AuthorityScope&, const AuthorityScope&) noexcept = default;
  friend std::strong_ordering operator<=>(const AuthorityScope& lhs, const AuthorityScope& rhs) noexcept;

 private:
  ScopeKind kind_ = ScopeKind::Fabric;
  std::string subject_;
};

/// A canonical (sorted, de-duplicated) set of scopes. Bounded in size.
class ScopeSet {
 public:
  constexpr ScopeSet() noexcept = default;

  /// Inserts a scope. Returns false when the set is full.
  bool insert(const AuthorityScope& scope, std::size_t max_scopes);

  [[nodiscard]] bool contains(const AuthorityScope& scope) const noexcept;
  [[nodiscard]] bool empty() const noexcept { return scopes_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return scopes_.size(); }
  [[nodiscard]] const std::vector<AuthorityScope>& scopes() const noexcept { return scopes_; }

  /// True when every scope in p other is present in this set.
  [[nodiscard]] bool contains_all(const ScopeSet& other) const noexcept;

  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const ScopeSet&, const ScopeSet&) noexcept = default;

 private:
  std::vector<AuthorityScope> scopes_;
};

}  // namespace fabric_epoch
