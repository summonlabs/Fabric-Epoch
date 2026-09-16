// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/version.hpp"

namespace fabric_epoch {

std::string version_full() {
  std::string out;
  out.reserve(version_string.size() + 24);
  out.append(version_string);
  out.append(" (persistence ");
  out.append(std::to_string(persistence_format_version));
  out.append(", wire ");
  out.append(std::to_string(wire_protocol_version));
  out.append(")");
  return out;
}

}  // namespace fabric_epoch
