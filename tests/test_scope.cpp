// Fabric Epoch 1.0.0 - Summon Software Labs
#include <string>

#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

FE_TEST(scope, parse_requires_a_concrete_subject) {
  const auto fabric = fe::AuthorityScope::parse(fe::ScopeKind::Fabric, "fabric-main");
  FE_REQUIRE(fabric.has_value());
  FE_REQUIRE_EQ(fabric->to_string(), std::string("FABRIC:fabric-main"));
  FE_REQUIRE(!fe::AuthorityScope::parse(fe::ScopeKind::Fabric, "").has_value());
  FE_REQUIRE(!fe::AuthorityScope::parse(fe::ScopeKind::Entity, "*").has_value());
  FE_REQUIRE(!fe::AuthorityScope::parse(fe::ScopeKind::Entity, "a b").has_value());
  FE_REQUIRE(!fe::AuthorityScope::parse(fe::ScopeKind::Entity, ".").has_value());
}

FE_TEST(scope, no_wildcard_kind_exists) {
  for (std::uint8_t value = 6; value < 16; ++value) {
    FE_REQUIRE(!fe::scope_kind_from_wire(value).has_value());
  }
  FE_REQUIRE(fe::scope_kind_from_wire(0).has_value());
  FE_REQUIRE(fe::scope_kind_from_wire(5).has_value());
}

FE_TEST(scope, set_is_canonical_and_deduplicates) {
  fe::ScopeSet set;
  FE_REQUIRE(set.insert(scope(fe::ScopeKind::Site, "site-b"), 8));
  FE_REQUIRE(set.insert(scope(fe::ScopeKind::Fabric, "fabric-main"), 8));
  FE_REQUIRE(set.insert(scope(fe::ScopeKind::Site, "site-b"), 8));
  FE_REQUIRE_EQ(set.size(), std::size_t{2});
  FE_REQUIRE_EQ(set.scopes()[0].to_string(), std::string("FABRIC:fabric-main"));
  FE_REQUIRE_EQ(set.scopes()[1].to_string(), std::string("SITE:site-b"));
  FE_REQUIRE_EQ(set.to_string(), std::string("[FABRIC:fabric-main,SITE:site-b]"));

  fe::ScopeSet reversed;
  FE_REQUIRE(reversed.insert(scope(fe::ScopeKind::Fabric, "fabric-main"), 8));
  FE_REQUIRE(reversed.insert(scope(fe::ScopeKind::Site, "site-b"), 8));
  FE_REQUIRE_EQ(reversed.to_string(), set.to_string());
}

FE_TEST(scope, set_respects_its_bound) {
  fe::ScopeSet set;
  FE_REQUIRE(set.insert(scope(fe::ScopeKind::Entity, "e1"), 2));
  FE_REQUIRE(set.insert(scope(fe::ScopeKind::Entity, "e2"), 2));
  FE_REQUIRE(!set.insert(scope(fe::ScopeKind::Entity, "e3"), 2));
  FE_REQUIRE_EQ(set.size(), std::size_t{2});
  // Re-inserting an existing element is not a bound violation.
  FE_REQUIRE(set.insert(scope(fe::ScopeKind::Entity, "e1"), 2));
}

FE_TEST(scope, containment_is_exact) {
  const fe::ScopeSet granted = scopes({scope(fe::ScopeKind::Site, "site-b"),
                                           scope(fe::ScopeKind::Entity, "node-7")});
  FE_REQUIRE(granted.contains(scope(fe::ScopeKind::Site, "site-b")));
  FE_REQUIRE(!granted.contains(scope(fe::ScopeKind::Site, "site-c")));
  // Fabric Epoch performs exact matching: a Site scope does not implicitly
  // cover an Entity inside it, because containment is topology truth owned by
  // another runtime.
  FE_REQUIRE(!granted.contains(scope(fe::ScopeKind::Fabric, "site-b")));

  const fe::ScopeSet subset = scopes({scope(fe::ScopeKind::Entity, "node-7")});
  FE_REQUIRE(granted.contains_all(subset));
  FE_REQUIRE(!subset.contains_all(granted));
  FE_REQUIRE(granted.contains_all(fe::ScopeSet{}));
}

FE_TEST(scope, kinds_render_stably) {
  FE_REQUIRE_EQ(fe::to_string(fe::ScopeKind::Fabric), std::string_view("FABRIC"));
  FE_REQUIRE_EQ(fe::to_string(fe::ScopeKind::Site), std::string_view("SITE"));
  FE_REQUIRE_EQ(fe::to_string(fe::ScopeKind::Domain), std::string_view("DOMAIN"));
  FE_REQUIRE_EQ(fe::to_string(fe::ScopeKind::EntityClass), std::string_view("ENTITY_CLASS"));
  FE_REQUIRE_EQ(fe::to_string(fe::ScopeKind::Entity), std::string_view("ENTITY"));
  FE_REQUIRE_EQ(fe::to_string(fe::ScopeKind::OperationFamily), std::string_view("OPERATION_FAMILY"));
}
