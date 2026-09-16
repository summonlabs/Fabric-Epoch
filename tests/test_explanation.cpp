// Fabric Epoch 1.0.0 - Summon Software Labs
#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

FE_TEST(explanation, rendering_is_order_independent) {
  fe::Explanation first(fe::ExplanationCode::WorkerBootFenced);
  first.with("boot", "abc").with("epoch", "9").with("reason", "PROCESS_LOSS");

  fe::Explanation second(fe::ExplanationCode::WorkerBootFenced);
  second.with("reason", "PROCESS_LOSS").with("epoch", "9").with("boot", "abc");

  FE_REQUIRE_EQ(first.render(), second.render());
  FE_REQUIRE_EQ(first.summary(), std::string("WORKER_BOOT_FENCED"));
  FE_REQUIRE_EQ(first.render(),
                std::string("WORKER_BOOT_FENCED (boot=abc, epoch=9, reason=PROCESS_LOSS)"));
}

FE_TEST(explanation, rendering_without_details_is_the_code) {
  const fe::Explanation explanation(fe::ExplanationCode::AuthorityCurrent);
  FE_REQUIRE_EQ(explanation.render(), std::string("AUTHORITY_CURRENT"));
}

FE_TEST(explanation, every_code_renders_a_distinct_stable_name) {
  std::vector<std::string> names;
  for (std::uint16_t value = 1; value <= 22; ++value) {
    const auto name = fe::to_string(static_cast<fe::ExplanationCode>(value));
    FE_REQUIRE_MSG(name != std::string_view("UNKNOWN"), std::to_string(value));
    names.emplace_back(name);
  }
  for (std::size_t i = 0; i < names.size(); ++i) {
    for (std::size_t j = i + 1; j < names.size(); ++j) {
      FE_REQUIRE(names[i] != names[j]);
    }
  }
}

FE_TEST(explanation, currentness_and_validation_codes_render_stably) {
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityCurrentness::Current), std::string_view("CURRENT"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityCurrentness::RevalidationRequired),
                std::string_view("REVALIDATION_REQUIRED"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityCurrentness::Fenced), std::string_view("FENCED"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityCurrentness::StaleEpoch), std::string_view("STALE_EPOCH"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityCurrentness::Retired), std::string_view("RETIRED"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityCurrentness::Unknown), std::string_view("UNKNOWN"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityCurrentness::Revoked), std::string_view("REVOKED"));

  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityValidationCode::Current), std::string_view("CURRENT"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityValidationCode::StaleEpoch), std::string_view("STALE_EPOCH"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityValidationCode::StaleWorkerBoot),
                std::string_view("STALE_WORKER_BOOT"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityValidationCode::UnknownParticipant),
                std::string_view("UNKNOWN_PARTICIPANT"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityValidationCode::UnauthorizedScope),
                std::string_view("UNAUTHORIZED_SCOPE"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityValidationCode::RevalidationRequired),
                std::string_view("REVALIDATION_REQUIRED"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityValidationCode::Malformed), std::string_view("MALFORMED"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityValidationCode::Retired), std::string_view("RETIRED"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityValidationCode::UnknownGrant),
                std::string_view("UNKNOWN_GRANT"));
  FE_REQUIRE_EQ(fe::to_string(fe::AuthorityValidationCode::Revoked), std::string_view("REVOKED"));
}

FE_TEST(explanation, structured_enums_never_collapse_to_a_bool) {
  FE_REQUIRE_EQ(fe::to_string(fe::FenceReason::ProcessLoss), std::string_view("PROCESS_LOSS"));
  FE_REQUIRE_EQ(fe::to_string(fe::FenceReason::Reincarnation), std::string_view("REINCARNATION"));
  FE_REQUIRE_EQ(fe::to_string(fe::FenceReason::CoordinatorEpochAdvance),
                std::string_view("COORDINATOR_EPOCH_ADVANCE"));
  FE_REQUIRE_EQ(fe::to_string(fe::FenceReason::AdministrativeRevoke),
                std::string_view("ADMINISTRATIVE_REVOKE"));
  FE_REQUIRE_EQ(fe::to_string(fe::FenceReason::SessionProtocolViolation),
                std::string_view("SESSION_PROTOCOL_VIOLATION"));
  FE_REQUIRE_EQ(fe::to_string(fe::FenceReason::ExplicitOperatorFence),
                std::string_view("EXPLICIT_OPERATOR_FENCE"));
  FE_REQUIRE_EQ(fe::to_string(fe::FenceReason::AuthorityReplaced),
                std::string_view("AUTHORITY_REPLACED"));
  FE_REQUIRE_EQ(fe::to_string(fe::FenceReason::Shutdown), std::string_view("SHUTDOWN"));

  FE_REQUIRE_EQ(fe::to_string(fe::RegistrationOutcome::Registered), std::string_view("REGISTERED"));
  FE_REQUIRE_EQ(fe::to_string(fe::RegistrationOutcome::Idempotent), std::string_view("IDEMPOTENT"));
  FE_REQUIRE_EQ(fe::to_string(fe::RegistrationOutcome::StaleEpoch), std::string_view("STALE_EPOCH"));
  FE_REQUIRE_EQ(fe::to_string(fe::RegistrationOutcome::StaleWorkerBoot),
                std::string_view("STALE_WORKER_BOOT"));
  FE_REQUIRE_EQ(fe::to_string(fe::RegistrationOutcome::AlreadyActive),
                std::string_view("ALREADY_ACTIVE"));
  FE_REQUIRE_EQ(fe::to_string(fe::RegistrationOutcome::ConflictingIncarnation),
                std::string_view("CONFLICTING_INCARNATION"));
  FE_REQUIRE_EQ(fe::to_string(fe::RegistrationOutcome::UnauthorizedScope),
                std::string_view("UNAUTHORIZED_SCOPE"));
  FE_REQUIRE_EQ(fe::to_string(fe::RegistrationOutcome::MalformedRequest),
                std::string_view("MALFORMED_REQUEST"));
  FE_REQUIRE_EQ(fe::to_string(fe::RegistrationOutcome::ResourceLimit),
                std::string_view("RESOURCE_LIMIT"));
}
