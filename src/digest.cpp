// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/digest.hpp"

#include "fabric_epoch/detail/sha256.hpp"
#include "fabric_epoch/identity.hpp"

namespace fabric_epoch {

std::string SnapshotDigest::to_string() const {
  return detail::to_hex(bytes_.data(), bytes_.size());
}

}  // namespace fabric_epoch
