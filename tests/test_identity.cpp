// Fabric Epoch 1.0.0 - Summon Software Labs
#include <string>
#include <vector>

#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

FE_TEST(identity, participant_accepts_valid_text) {
  for (const std::string text : {"p", "publisher.alpha", "site-1:node_7", "A1.b-2_c:3"}) {
    const auto parsed = fe::ParticipantId::parse(text);
    FE_REQUIRE_MSG(parsed.has_value(), text);
    FE_REQUIRE_EQ(parsed->value(), text);
    FE_REQUIRE_EQ(parsed->to_string(), text);
  }
}

FE_TEST(identity, participant_rejects_malformed_text) {
  const std::vector<std::string> rejected = {
      "", ".leading", "trailing.", "-leading", "trailing-", ":colon", "colon:", "with space",
      "with\ttab", "with\nnewline", "sla/sh", "back\\slash", "\x01control",
      std::string(129, 'a')};
  for (const std::string& text : rejected) {
    FE_REQUIRE_MSG(!fe::ParticipantId::parse(text).has_value(), text);
  }
}

FE_TEST(identity, fixed_identifier_hex_round_trip) {
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  FE_REQUIRE(!boot.is_nil());
  const std::string text = boot.to_string();
  FE_REQUIRE_EQ(text.size(), std::size_t{32});
  const auto parsed = fe::WorkerBootId::parse(text);
  FE_REQUIRE(parsed.has_value());
  FE_REQUIRE_EQ(*parsed, boot);
  std::string upper = text;
  for (char& c : upper) {
    if (c >= 'a' && c <= 'f') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  FE_REQUIRE_EQ(*fe::WorkerBootId::parse(upper), boot);
}

FE_TEST(identity, fixed_identifier_rejects_bad_encoding) {
  const std::vector<std::string> rejected = {
      "", "0", std::string(31, 'a'), std::string(33, 'a'), std::string(32, 'z'),
      std::string(32, 'g'), "473f62c063740e7472cae51f5f514bc", "473f62c063740e7472cae51f5f514bc66",
      " 73f62c063740e7472cae51f5f514bc6a"};
  for (const std::string& text : rejected) {
    FE_REQUIRE_MSG(!fe::WorkerBootId::parse(text).has_value(), text);
  }
}

FE_TEST(identity, distinct_identifier_domains_do_not_compare) {
  const auto raw = fe::detail::parse_hex_16("00112233445566778899aabbccddeeff");
  FE_REQUIRE(raw.has_value());
  const fe::WorkerBootId boot = fe::WorkerBootId::from_bytes(*raw);
  const fe::SessionId session = fe::SessionId::from_bytes(*raw);
  const fe::AuthorityGrantId grant = fe::AuthorityGrantId::from_bytes(*raw);
  // Same bytes, different domains: the values are equal byte-wise but the
  // types are unrelated, so they can never be passed for one another.
  FE_REQUIRE_EQ(boot.bytes(), session.bytes());
  FE_REQUIRE_EQ(session.bytes(), grant.bytes());
  FE_REQUIRE(boot == fe::WorkerBootId::from_bytes(*raw));
}

FE_TEST(identity, nil_identifier_is_recognisable) {
  FE_REQUIRE(fe::WorkerBootId{}.is_nil());
  FE_REQUIRE(!fe::WorkerBootId::generate().is_nil());
}

FE_TEST(identity, generated_identifiers_are_distinct) {
  std::vector<fe::WorkerBootId> boots;
  for (int i = 0; i < 256; ++i) {
    boots.push_back(fe::WorkerBootId::generate());
  }
  for (std::size_t i = 0; i < boots.size(); ++i) {
    FE_REQUIRE(!boots[i].is_nil());
    for (std::size_t j = i + 1; j < boots.size(); ++j) {
      FE_REQUIRE(boots[i] != boots[j]);
    }
  }
}

FE_TEST(identity, publisher_conversion_is_explicit) {
  const fe::ParticipantId stable_identity = participant("publisher.gamma");
  const fe::PublisherId publisher = fe::PublisherId::from_participant(stable_identity);
  FE_REQUIRE_EQ(publisher.value(), stable_identity.value());
  FE_REQUIRE_EQ(publisher.as_participant(), stable_identity);
}

FE_TEST(identity, monotonic_counter_rejects_overflow) {
  fe::CoordinatorEpoch epoch(0);
  FE_REQUIRE(epoch.is_zero());
  for (int i = 0; i < 8; ++i) {
    const auto next = epoch.next();
    FE_REQUIRE(next.has_value());
    epoch = *next;
  }
  FE_REQUIRE_EQ(epoch.value(), std::uint64_t{8});

  const fe::CoordinatorEpoch maximum(fe::CoordinatorEpoch::max_value);
  FE_REQUIRE(maximum.is_max());
  FE_REQUIRE(!maximum.next().has_value());

  FE_REQUIRE_EQ(fe::CoordinatorEpoch(4).plus(6).value().value(), std::uint64_t{10});
  FE_REQUIRE(!fe::CoordinatorEpoch(fe::CoordinatorEpoch::max_value).plus(1).has_value());
  FE_REQUIRE_EQ(fe::CoordinatorEpoch(fe::CoordinatorEpoch::max_value).plus(0).value().value(),
                fe::CoordinatorEpoch::max_value);
}

FE_TEST(identity, monotonic_counter_orders_explicitly) {
  FE_REQUIRE(fe::CoordinatorEpoch(3) < fe::CoordinatorEpoch(4));
  FE_REQUIRE(fe::CoordinatorEpoch(4) > fe::CoordinatorEpoch(3));
  FE_REQUIRE(fe::CoordinatorEpoch(4) == fe::CoordinatorEpoch(4));
  FE_REQUIRE_EQ(fe::CoordinatorEpoch(42).to_string(), std::string("42"));
}

FE_TEST(identity, bounded_text_rejects_control_and_non_ascii) {
  FE_REQUIRE(fe::BoundedText::is_valid("coordinator restart", 256));
  FE_REQUIRE(fe::BoundedText::is_valid("", 256));
  FE_REQUIRE(fe::BoundedText::is_valid("tab\there", 256));
  FE_REQUIRE(!fe::BoundedText::is_valid("bell\x07", 256));
  FE_REQUIRE(!fe::BoundedText::is_valid("del\x7f", 256));
  FE_REQUIRE(!fe::BoundedText::is_valid("\xc3\xa9", 256));
  FE_REQUIRE(!fe::BoundedText::is_valid(std::string(257, 'a'), 256));
}
