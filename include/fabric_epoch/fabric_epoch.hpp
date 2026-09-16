// Fabric Epoch 1.0.0 - Summon Software Labs
//
// Umbrella header for the Fabric Epoch public library API.
//
// Fabric Epoch is the authoritative control-plane epoch and stale-authority
// fencing runtime of the Distributed Fabric Infrastructure / Fabric OS stack.
// It answers exactly one question: which control-plane epoch is authoritative
// now, which participants and worker incarnations belong to that epoch, which
// prior generations are permanently stale, and when must traffic, mutations,
// evidence, leases, registrations, snapshots or control decisions be rejected
// because they were created under obsolete authority?
#pragma once

#include "fabric_epoch/authority.hpp"
#include "fabric_epoch/client.hpp"
#include "fabric_epoch/digest.hpp"
#include "fabric_epoch/durable.hpp"
#include "fabric_epoch/epoch.hpp"
#include "fabric_epoch/error.hpp"
#include "fabric_epoch/explanation.hpp"
#include "fabric_epoch/fence.hpp"
#include "fabric_epoch/identity.hpp"
#include "fabric_epoch/limits.hpp"
#include "fabric_epoch/participant.hpp"
#include "fabric_epoch/protocol.hpp"
#include "fabric_epoch/runtime.hpp"
#include "fabric_epoch/scope.hpp"
#include "fabric_epoch/server.hpp"
#include "fabric_epoch/snapshot.hpp"
#include "fabric_epoch/store.hpp"
#include "fabric_epoch/version.hpp"
