// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/snapshot.hpp"

namespace fabric_epoch {

std::string AuthoritySnapshot::render() const {
  std::string out;
  out.append("snapshot=");
  out.append(id.to_string());
  out.append(" epoch=");
  out.append(epoch.to_string());
  out.append(" coordinator_boot=");
  out.append(coordinator_boot.to_string());
  out.append(" generation=");
  out.append(generation.to_string());
  out.append(" registration_floor=");
  out.append(registration_floor.to_string());
  out.append(" fence_floor=");
  out.append(fence_floor.to_string());
  out.append(" participants=");
  out.append(std::to_string(participants.size()));
  out.append(" fences=");
  out.append(std::to_string(fences.size()));
  out.append(" grants=");
  out.append(std::to_string(grants.size()));
  out.append(" digest=");
  out.append(digest.to_string());
  return out;
}

}  // namespace fabric_epoch
