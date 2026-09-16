cmake_minimum_required(VERSION 3.21)

project(FabricEpoch
  VERSION 1.0.0
  DESCRIPTION "Authoritative control-plane epoch and stale-authority fencing runtime"
  HOMEPAGE_URL "https://github.com/pngen/Fabric-Epoch"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Version consistency. The C++ header is the authoritative source; the CMake
# project version must agree with it or configuration fails fast.
# ---------------------------------------------------------------------------
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/fabric_epoch/version.hpp" FABRIC_EPOCH_VERSION_HEADER)

function(fabric_epoch_header_constant NAME OUT_VAR)
  string(REGEX MATCH "inline constexpr int ${NAME} = ([0-9]+);" MATCH "${FABRIC_EPOCH_VERSION_HEADER}")
  if(NOT MATCH)
    message(FATAL_ERROR "include/fabric_epoch/version.hpp does not define ${NAME}")
  endif()
  set(${OUT_VAR} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

fabric_epoch_header_constant(version_major FABRIC_EPOCH_VERSION_MAJOR)
fabric_epoch_header_constant(version_minor FABRIC_EPOCH_VERSION_MINOR)
fabric_epoch_header_constant(version_patch FABRIC_EPOCH_VERSION_PATCH)

set(FABRIC_EPOCH_HEADER_VERSION
  "${FABRIC_EPOCH_VERSION_MAJOR}.${FABRIC_EPOCH_VERSION_MINOR}.${FABRIC_EPOCH_VERSION_PATCH}")

if(NOT FABRIC_EPOCH_HEADER_VERSION STREQUAL PROJECT_VERSION)
  message(FATAL_ERROR
    "version mismatch: project() declares ${PROJECT_VERSION} but version.hpp declares "
    "${FABRIC_EPOCH_HEADER_VERSION}")
endif()

message(STATUS "Fabric Epoch ${PROJECT_VERSION}")

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FABRIC_EPOCH_BUILD_TESTS "Build the Fabric Epoch test suites" ON)
option(FABRIC_EPOCH_BUILD_EXAMPLES "Build the Fabric Epoch examples" ON)
option(FABRIC_EPOCH_BUILD_BENCHMARKS "Build the Fabric Epoch benchmarks" ON)
option(FABRIC_EPOCH_BUILD_APPS "Build the Fabric Epoch coordinator, worker and CLI" ON)
option(FABRIC_EPOCH_ENABLE_ANALYZE "Run the MSVC static analyzer on first-party code" OFF)
option(FABRIC_EPOCH_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(FABRIC_EPOCH_BUILD_DISTRIBUTED_TESTS
  "Build the suites that start real coordinator and worker processes" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warning configuration for first-party targets only.
# ---------------------------------------------------------------------------
function(fabric_epoch_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    if(FABRIC_EPOCH_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(FABRIC_EPOCH_ENABLE_ANALYZE)
      # System and platform headers are third-party code. They are declared
      # external so that analysis reports first-party findings only; no
      # first-party warning is suppressed by this.
      target_compile_options(${target} PRIVATE
        /analyze
        /external:anglebrackets
        /external:env:INCLUDE
        /external:W0)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(FABRIC_EPOCH_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(FabricEpoch
  src/authority.cpp
  src/client.cpp
  src/codec.cpp
  src/digest.cpp
  src/durable.cpp
  src/encoding.cpp
  src/epoch.cpp
  src/error.cpp
  src/explanation.cpp
  src/fence.cpp
  src/identity.cpp
  src/limits.cpp
  src/participant.cpp
  src/protocol.cpp
  src/runtime.cpp
  src/scope.cpp
  src/server.cpp
  src/sha256.cpp
  src/snapshot.cpp
  src/socket.cpp
  src/store.cpp
  src/version.cpp)

add_library(SummonSoftwareLabs::FabricEpoch ALIAS FabricEpoch)

set_target_properties(FabricEpoch PROPERTIES
  EXPORT_NAME FabricEpoch
  POSITION_INDEPENDENT_CODE ON
  WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(FabricEpoch
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

fabric_epoch_apply_warnings(FabricEpoch)

if(WIN32)
  target_link_libraries(FabricEpoch PRIVATE ws2_32 bcrypt)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(FabricEpoch PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
if(FABRIC_EPOCH_BUILD_APPS)
  add_executable(fabric-epoch-coordinator apps/coordinator_main.cpp apps/arguments.cpp)
  add_executable(fabric-epoch-worker apps/worker_main.cpp apps/arguments.cpp)
  add_executable(fabric-epoch-cli apps/cli_main.cpp apps/arguments.cpp)
  foreach(app fabric-epoch-coordinator fabric-epoch-worker fabric-epoch-cli)
    target_link_libraries(${app} PRIVATE SummonSoftwareLabs::FabricEpoch)
    target_include_directories(${app} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps")
    fabric_epoch_apply_warnings(${app})
  endforeach()
  install(TARGETS fabric-epoch-coordinator fabric-epoch-worker fabric-epoch-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(FABRIC_EPOCH_BUILD_EXAMPLES)
  set(FABRIC_EPOCH_EXAMPLES
    basic_registration
    authority_validation
    worker_reincarnation
    fencing
    epoch_advancement
    stale_epoch_rejection
    stale_boot_rejection
    coordinator_restart
    persistence_recovery)
  foreach(example ${FABRIC_EPOCH_EXAMPLES})
    add_executable(fabric_epoch_example_${example} examples/${example}.cpp)
    target_link_libraries(fabric_epoch_example_${example} PRIVATE SummonSoftwareLabs::FabricEpoch)
    fabric_epoch_apply_warnings(fabric_epoch_example_${example})
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(FABRIC_EPOCH_BUILD_BENCHMARKS)
  add_executable(fabric_epoch_benchmarks benchmarks/benchmarks.cpp)
  target_link_libraries(fabric_epoch_benchmarks PRIVATE SummonSoftwareLabs::FabricEpoch)
  fabric_epoch_apply_warnings(fabric_epoch_benchmarks)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(FABRIC_EPOCH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS FabricEpoch
  EXPORT FabricEpochTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(FABRIC_EPOCH_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/FabricEpoch")

install(EXPORT FabricEpochTargets
  NAMESPACE SummonSoftwareLabs::
  FILE FabricEpochTargets.cmake
  DESTINATION ${FABRIC_EPOCH_CMAKE_INSTALL_DIR})

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FabricEpochConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/FabricEpochConfig.cmake"
  INSTALL_DESTINATION ${FABRIC_EPOCH_CMAKE_INSTALL_DIR})

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/FabricEpochConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/FabricEpochConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/FabricEpochConfigVersion.cmake"
  DESTINATION ${FABRIC_EPOCH_CMAKE_INSTALL_DIR})
