// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/scope.hpp"

#include <algorithm>

#include "fabric_epoch/error.hpp"
#include "fabric_epoch/identity.hpp"

namespace fabric_epoch {

std::string_view to_string(ScopeKind kind) noexcept {
  switch (kind) {
    case ScopeKind::Fabric:
      return "FABRIC";
    case ScopeKind::Site:
      return "SITE";
    case ScopeKind::Domain:
      return "DOMAIN";
    case ScopeKind::EntityClass:
      return "ENTITY_CLASS";
    case ScopeKind::Entity:
      return "ENTITY";
    case ScopeKind::OperationFamily:
      return "OPERATION_FAMILY";
  }
  return "UNKNOWN";
}

std::optional<ScopeKind> scope_kind_from_wire(std::uint8_t value) noexcept {
  switch (value) {
    case 0:
      return ScopeKind::Fabric;
    case 1:
      return ScopeKind::Site;
    case 2:
      return ScopeKind::Domain;
    case 3:
      return ScopeKind::EntityClass;
    case 4:
      return ScopeKind::Entity;
    case 5:
      return ScopeKind::OperationFamily;
    default:
      return std::nullopt;
  }
}

std::optional<AuthorityScope> AuthorityScope::parse(ScopeKind kind, std::string_view subject) {
  if (!detail::is_valid_identifier_text(subject, max_subject_length)) {
    return std::nullopt;
  }
  AuthorityScope scope;
  scope.kind_ = kind;
  scope.subject_.assign(subject);
  return scope;
}

std::string AuthorityScope::to_string() const {
  std::string out;
  out.append(fabric_epoch::to_string(kind_));
  out.push_back(':');
  out.append(subject_);
  return out;
}

std::strong_ordering operator<=>(const AuthorityScope& lhs, const AuthorityScope& rhs) noexcept {
  if (const auto by_kind = lhs.kind_ <=> rhs.kind_; by_kind != 0) {
    return by_kind;
  }
  return lhs.subject_ <=> rhs.subject_;
}

bool ScopeSet::insert(const AuthorityScope& scope, std::size_t max_scopes) {
  const auto position = std::lower_bound(scopes_.begin(), scopes_.end(), scope);
  if (position != scopes_.end() && *position == scope) {
    return true;
  }
  if (scopes_.size() >= max_scopes) {
    return false;
  }
  scopes_.insert(position, scope);
  return true;
}

bool ScopeSet::contains(const AuthorityScope& scope) const noexcept {
  const auto position = std::lower_bound(scopes_.begin(), scopes_.end(), scope);
  return position != scopes_.end() && *position == scope;
}

bool ScopeSet::contains_all(const ScopeSet& other) const noexcept {
  for (const auto& scope : other.scopes_) {
    if (!contains(scope)) {
      return false;
    }
  }
  return true;
}

std::string ScopeSet::to_string() const {
  std::string out;
  out.push_back('[');
  for (std::size_t i = 0; i < scopes_.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out.append(scopes_[i].to_string());
  }
  out.push_back(']');
  return out;
}

}  // namespace fabric_epoch
