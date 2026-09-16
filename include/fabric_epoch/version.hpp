// Fabric Epoch 1.0.0 - Summon Software Labs
// Authoritative control-plane epoch and stale-authority fencing runtime.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace fabric_epoch {

/// Product version. This is the single authoritative source for the library
/// version, the CMake package version, the CLI version report and the version
/// advertised on the wire during HELLO.
inline constexpr int version_major = 1;
inline constexpr int version_minor = 0;
inline constexpr int version_patch = 0;
inline constexpr std::string_view version_string = "1.0.0";

/// Durable representation version. Bumped only when the on-disk representation
/// actually changes; it is deliberately independent of the product version.
inline constexpr std::uint32_t persistence_format_version = 1;

/// Framed protocol version. Bumped only when the wire representation changes.
inline constexpr std::uint16_t wire_protocol_version = 1;

/// Stable protocol magic ("FEP1").
inline constexpr std::uint32_t wire_protocol_magic = 0x46455031u;

[[nodiscard]] std::string version_full();

}  // namespace fabric_epoch
