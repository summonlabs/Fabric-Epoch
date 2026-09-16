cmake_minimum_required(VERSION 3.21)

# Independent downstream consumer of the installed Fabric Epoch package.
# This project is deliberately NOT part of the Fabric Epoch build: it exists to
# prove that the installed artifact alone (headers, library, exported target and
# package config) is sufficient to compile, link and run against.
project(FabricEpochConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(FabricEpoch CONFIG REQUIRED)

if(NOT TARGET SummonSoftwareLabs::FabricEpoch)
  message(FATAL_ERROR "the installed package did not export SummonSoftwareLabs::FabricEpoch")
endif()

message(STATUS "consumer resolved FabricEpoch ${FabricEpoch_VERSION}")

add_executable(fabric_epoch_consumer main.cpp)
target_link_libraries(fabric_epoch_consumer PRIVATE SummonSoftwareLabs::FabricEpoch)

if(MSVC)
  target_compile_options(fabric_epoch_consumer PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
else()
  target_compile_options(fabric_epoch_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()
add_test(NAME fabric_epoch_consumer COMMAND fabric_epoch_consumer)
