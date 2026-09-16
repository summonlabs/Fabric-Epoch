# Fabric Epoch 1.0.0 - Summon Software Labs
# Test suites. No CTest TIMEOUT property is set anywhere: a hanging test is a
# defect and must surface as a hang rather than be masked by a timer.

set(FABRIC_EPOCH_TEST_SUITES
  identity
  scope
  explanation
  epoch_arithmetic
  persistence
  corruption
  registration
  validation
  fencing
  grants
  reincarnation
  snapshot_digest
  protocol
  limits
  concurrency
  property
  adversarial
  scale)

add_library(fabric_epoch_test_support STATIC test_harness.cpp)
target_include_directories(fabric_epoch_test_support PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  # White-box access to the canonical codec lets the corruption suites craft
  # adversarial durable images that the validating encoder would refuse to
  # produce. Internal headers are never installed.
  "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(fabric_epoch_test_support PUBLIC SummonSoftwareLabs::FabricEpoch)
fabric_epoch_apply_warnings(fabric_epoch_test_support)

set(FABRIC_EPOCH_TEST_SOURCES test_main.cpp)
foreach(suite ${FABRIC_EPOCH_TEST_SUITES})
  list(APPEND FABRIC_EPOCH_TEST_SOURCES test_${suite}.cpp)
endforeach()

if(FABRIC_EPOCH_BUILD_DISTRIBUTED_TESTS AND FABRIC_EPOCH_BUILD_APPS)
  list(APPEND FABRIC_EPOCH_TEST_SOURCES test_distributed.cpp)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/test_executables.hpp" CONTENT
"#pragma once
#define FABRIC_EPOCH_COORDINATOR_EXE \"$<TARGET_FILE:fabric-epoch-coordinator>\"
#define FABRIC_EPOCH_WORKER_EXE \"$<TARGET_FILE:fabric-epoch-worker>\"
#define FABRIC_EPOCH_CLI_EXE \"$<TARGET_FILE:fabric-epoch-cli>\"
")
  set_source_files_properties(test_distributed.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
endif()

add_executable(fabric_epoch_tests ${FABRIC_EPOCH_TEST_SOURCES})
target_link_libraries(fabric_epoch_tests PRIVATE fabric_epoch_test_support)
fabric_epoch_apply_warnings(fabric_epoch_tests)
if(FABRIC_EPOCH_BUILD_DISTRIBUTED_TESTS AND FABRIC_EPOCH_BUILD_APPS)
  add_dependencies(fabric_epoch_tests fabric-epoch-coordinator fabric-epoch-worker fabric-epoch-cli)
endif()

add_test(NAME fabric_epoch_tests COMMAND fabric_epoch_tests)
