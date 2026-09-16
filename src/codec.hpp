// Fabric Epoch 1.0.0 - Summon Software Labs
// Internal canonical codecs shared by durable persistence, semantic digesting
// and the framed protocol. Not installed.
#pragma once

#include <span>
#include <vector>

#include "fabric_epoch/authority.hpp"
#include "fabric_epoch/detail/encoding.hpp"
#include "fabric_epoch/durable.hpp"
#include "fabric_epoch/limits.hpp"
#include "fabric_epoch/participant.hpp"
#include "fabric_epoch/snapshot.hpp"

namespace fabric_epoch::detail {

void encode_scope_set_canonical(ByteWriter& writer, const ScopeSet& scopes);
[[nodiscard]] ScopeSet decode_scope_set_canonical(ByteReader& reader, const Limits& limits);

void encode_participant_view(ByteWriter& writer, const ParticipantView& view, const Limits& limits);
[[nodiscard]] ParticipantView decode_participant_view(ByteReader& reader, const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_durable_payload(const DurableState& state);
[[nodiscard]] DurableState decode_durable_payload(std::span<const std::byte> payload, const Limits& limits);

/// Deterministic digest over canonicalized semantic authority state.
[[nodiscard]] SnapshotDigest compute_snapshot_digest(const AuthoritySnapshot& snapshot);
[[nodiscard]] SnapshotDigest compute_semantic_digest(std::span<const std::byte> canonical);

}  // namespace fabric_epoch::detail
