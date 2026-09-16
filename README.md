# Fabric Epoch

**Fabric Epoch is the authoritative control-plane epoch and stale-authority fencing runtime of the
Distributed Fabric Infrastructure / Fabric OS stack.** It answers exactly one question:

> Which control-plane epoch is authoritative now, which participants and worker incarnations belong
> to that epoch, which prior generations are permanently stale, and when must traffic, mutations,
> evidence, leases, registrations, snapshots or control decisions be rejected because they were
> created under obsolete authority?

Fabric Epoch 1.0.0 is a C++20 library, a framed wire protocol, a single-authority coordinator
process and a worker process. It is vendor-neutral, has no third-party dependencies, and builds with
CMake.

The core invariant is:

> **New authority must never be confused with continuation of old authority.**

Stable participant identity, worker incarnation, coordinator incarnation, coordinator epoch, session,
durable state, live authority, generation and evidence currentness are all modelled as separate
things, and Fabric Epoch refuses to let any one of them stand in for another.

---

## 1. Systems boundary

### Fabric Epoch owns

* coordinator epochs and epoch generation;
* epoch identity and epoch transition rules;
* coordinator incarnation (`CoordinatorBootId`);
* publisher/worker incarnation registration and `WorkerBootId` fencing;
* participant session generations;
* stale-authority rejection;
* epoch-bound authority tokens and authority grants;
* monotonic epoch advancement and durable epoch recovery;
* epoch transition provenance and bounded transition history;
* participant reattachment and revalidation;
* epoch currentness;
* epoch snapshots with deterministic semantic digests;
* deterministic epoch and authority explanations;
* versioned, integrity-checked epoch persistence;
* distributed epoch publication over a framed protocol;
* real coordinator-restart and real worker-reincarnation semantics;
* the stale-epoch and stale-boot rejection primitives other Fabric OS runtimes bind to.

### Fabric Epoch does not own

* canonical device identity (Fabric Registry);
* structural relationships (Fabric Topology);
* operational link state (Link State Fabric);
* port configuration (Port Fabric);
* device capability truth (Fabric Capability Registry);
* failure-domain classification (Failure Domain Registry);
* routing, path planning, path authority, traffic engineering;
* scheduling, congestion, queue or buffer allocation;
* distributed consensus across independent administrative clusters (not implemented; see section 11).

### Relationship to Fabric Registry

Fabric Registry owns canonical infrastructure identity. Fabric Epoch **consumes** stable participant
identity - `ParticipantId`, and `PublisherId` on the publication path - and never invents a
competing infrastructure identity. Fabric Epoch owns only epoch-specific identities:

```
CoordinatorEpoch   CoordinatorBootId   WorkerBootId   SessionId
AuthorityGrantId   EpochTransitionId   EpochSnapshotId
EpochGeneration    RegistrationGeneration   FencingGeneration   IncarnationSequence
MutationAttemptId
```

These are strongly typed values, not interchangeable integers or raw strings. They reject malformed
encodings, serialize deterministically, render stably, compare explicitly, and never convert across
domains implicitly.

Fabric Epoch keeps three questions strictly apart:

```
PARTICIPANT IDENTITY EXISTS
PARTICIPANT HAS A CURRENT INCARNATION
PARTICIPANT HOLDS AUTHORITY IN THE CURRENT EPOCH
```

### Relationship to downstream Fabric OS runtimes

A downstream runtime binds to Fabric Epoch by presenting an authority token and asking the cheap
deterministic validation primitive whether the token is still current **for a specific authority
scope**:

```cpp
fabric_epoch::ValidateAuthorityRequest request;
request.token.epoch        = token_epoch;
request.token.participant  = publisher_id;
request.token.boot         = worker_boot_id;
request.token.incarnation  = incarnation_sequence;
request.scope              = authority_scope;

const auto validation = runtime.validate_authority(request);
if (validation.code != fabric_epoch::AuthorityValidationCode::Current) {
  // The reason is exact:
  // STALE_EPOCH, STALE_WORKER_BOOT, REVALIDATION_REQUIRED, UNAUTHORIZED_SCOPE,
  // UNKNOWN_PARTICIPANT, RETIRED, UNKNOWN_GRANT, REVOKED or MALFORMED.
}
```

The same primitive is available over the wire as **VALIDATE_AUTHORITY -> VALIDATE_RESULT**.

---

## 2. CoordinatorEpoch

`CoordinatorEpoch` is the authoritative generation of the control plane.

* It is **monotonic**. It never decreases and is never derived from wall-clock time, a process
  identifier, or a boot timestamp.
* It **survives durable restart**: the current epoch is written to the durable image before it is
  acknowledged.
* It **advances on a genuinely fresh coordinator authority incarnation**. A fresh coordinator process
  over an existing store records a `COORDINATOR_RESTART` transition and advances by exactly one;
  a pristine store records `COORDINATOR_START` and establishes epoch 1.
* **Old epochs remain permanently stale.** A token or request naming any epoch other than the current
  one is rejected with `STALE_EPOCH`.
* **Overflow is terminal.** `CoordinatorEpoch` uses checked arithmetic. At the maximum
  representable value, advancement is refused with `EPOCH_EXHAUSTED` (or an `OVERFLOW`
  infrastructure error when a fresh coordinator tries to open at the maximum). The counter never wraps
  to zero and an older epoch is never reused.

## 3. CoordinatorBootId

`CoordinatorBootId` identifies one actual coordinator process incarnation. It is deliberately a
different type from `CoordinatorEpoch`:

| type | identifies |
|---|---|
| `CoordinatorEpoch` | one **authority generation** |
| `CoordinatorBootId` | one **process incarnation** that established an epoch |

Fabric Epoch 1.0.0 takes the conservative rule: **a fresh coordinator process after loss or restart
advances the epoch.** No exception is implemented and therefore none is claimed.

## 4. WorkerBootId and incarnation

Every worker or publisher process generates a fresh 128-bit `WorkerBootId` from the
operating-system CSPRNG at start-up. A stable `PublisherId` does **not** imply stable worker
authority.

* A reconnect with the same `WorkerBootId` is idempotent only while that incarnation is still
  current and the session semantics permit it.
* A restart must generate a new `WorkerBootId`.
* A fenced `WorkerBootId` never becomes current again while its fence is retained.
* Traffic from an old `WorkerBootId` is rejected permanently.
* A new `WorkerBootId` must establish fresh authority by registering under the current epoch.

Every accepted incarnation receives a **monotonic incarnation sequence** from a per-participant
allocator that never reissues a value - not after fencing, and not after retention pruning.

## 5. Participant registration

Registration binds stable participant identity to a worker incarnation under one epoch:

```cpp
struct RegisterParticipantRequest {
  MutationAttemptId attempt;
  CoordinatorEpoch expected_epoch;
  ParticipantId participant;
  WorkerBootId boot;
  ScopeSet scopes;
  IncarnationPolicy policy;
  std::optional<RegistrationGeneration> expected_registration_generation;
  BoundedText provenance;
};
```

Registration returns a **structured outcome**, never a bool:

```
REGISTERED   IDEMPOTENT   STALE_EPOCH   STALE_WORKER_BOOT   ALREADY_ACTIVE
CONFLICTING_INCARNATION   UNAUTHORIZED_SCOPE   MALFORMED_REQUEST   RESOURCE_LIMIT
RETIRED_PARTICIPANT   CONFLICTING_ATTEMPT   REVALIDATION_REQUIRED
```

### Processing order

The order is fixed and tested, because a careless order leaks protected state through replay
classification:

```
decode and framing validation
  -> structural / malformed validation
  -> epoch currentness
  -> participant retirement
  -> worker-boot fencing
  -> request identity classification (exact replay / attempt conflict)
  -> expected-generation check
  -> incarnation policy and conflict resolution
  -> resource limits
  -> mutation
  -> durable commit
  -> response
```

Epoch currentness is established **before** replay classification, so a stale request can never learn
anything from the idempotency table. Epoch advancement (section 7) is the one documented exception: an
exact retry of a successful advancement necessarily carries the pre-advancement expectation, so for
that control-plane operation replay classification precedes the expected-epoch check. The attempt table
is keyed by a caller-supplied 128-bit identifier, so nothing about another caller's state is
observable.

### Exactly-once semantics

`MutationAttemptId` gives every mutating request an identity. An exact replay returns the recorded
outcome with `replayed = true` and advances no generation. Reusing an attempt identifier with a
different semantic payload is rejected as `CONFLICTING_ATTEMPT`.

The semantic replay identity deliberately **excludes** `expected_epoch`,
`expected_registration_generation` and free-form provenance, so a caller retrying with refreshed
expectation fields is still recognised as an exact retry instead of being mistaken for a new
incarnation.

## 6. One current incarnation

`IncarnationPolicy::SingleIncarnation` is the default: **at most one current `WorkerBootId`
per `ParticipantId`.** Multi-incarnation participation is a separate, explicitly requested policy
that a runtime must enable; it is never entered by accident.

For a single-incarnation participant, when a fresh incarnation appears while an incumbent is still
live, one of two explicitly configured policies applies:

* `RequireExplicitFence` (default) - the registration is refused with
  `CONFLICTING_INCARNATION` until the incumbent is fenced.
* `ReplaceIncumbent` - the incumbent is fenced **atomically with** the acceptance of the new
  incarnation, with fence reason `REINCARNATION`.

Either way the old incarnation stops being current, its sessions become stale, and its authority
grants become unusable.

## 7. Epoch advancement

Advancement is explicit and transactional:

```
load current durable epoch
  -> verify integrity and internal consistency
  -> establish a fresh coordinator authority incarnation
  -> compute the next epoch with checked arithmetic
  -> persist the new epoch durably (temp file, flush, atomic replace)
  -> make it authoritative in memory
  -> invalidate every prior live authority (REVALIDATION_REQUIRED)
  -> expose the new epoch
  -> permit fresh registration
```

**The new epoch is never exposed before durable advancement succeeds.** If the durability barrier
fails, the in-memory mutation is rolled back through an undo log and the caller sees an exception; no
partial authority state is observable. The same rollback discipline applies to every mutation.

## 8. Authority scope and grants

Authority scopes are explicit. A scope is a dimension plus a concrete subject:

```
FABRIC:<fabric>   SITE:<site>   DOMAIN:<domain>
ENTITY_CLASS:<class>   ENTITY:<entity>   OPERATION_FAMILY:<family>
```

* There is **no wildcard scope kind** and an empty subject is not representable.
* Matching is **exact**. Fabric Epoch does not resolve containment between scopes, because containment
  is topology truth owned by another runtime. A participant authorised for one scope cannot mutate
  another merely because it holds a current `WorkerBootId`.
* A participant may only self-register scope kinds the runtime has enabled
  (`EpochRuntimeOptions::registrable_scope_kinds`); anything else is `UNAUTHORIZED_SCOPE`.

An explicit `AuthorityGrant` binds the current epoch, the participant, the worker boot, the
incarnation sequence, a canonical scope set, a generation, and optionally a parent grant. A grant from
an old epoch is invalid. A grant bound to a fenced `WorkerBootId` is invalid. Durable
serialization of a grant does not make it live after a restart: a grant is only exercised through
current-state validation.

**Delegation** is modelled explicitly when used: a delegated grant must bind a live parent grant in the
current epoch, and its scope set may never exceed the parent's - enforced as a set-containment check,
not as a convention.

**Revocation** is generation-bound and idempotent, cascades to derived grants, survives restart, and is
distinct from worker fencing: revoking authority never retires the participant identity.

## 9. Fencing

Fencing is **permanent for the targeted incarnation** and **idempotent**. Repeating a fence returns
`ALREADY_FENCED` and advances no generation, creates no duplicate record, and produces no
semantic churn.

Fence reasons are structured, never free-form strings: `PROCESS_LOSS`, `REINCARNATION`,
`COORDINATOR_EPOCH_ADVANCE`, `ADMINISTRATIVE_REVOKE`, `SESSION_PROTOCOL_VIOLATION`,
`EXPLICIT_OPERATOR_FENCE`, `AUTHORITY_REPLACED`, `SHUTDOWN`. Human text may accompany
a reason code and is never the only representation.

Fencing is durable **before it is acknowledged**. A fenced boot is still rejected after the process
that fenced it is killed.

### Fence retention

Retention is bounded, and bounded retention cannot resurrect an old incarnation:

* Every participant carries an exact, bounded ring of recently fenced `WorkerBootId` literals.
* Every participant carries a monotonic **incarnation floor**: the highest incarnation sequence that
  has been fenced. Sequences are issued by a monotonic allocator that never reuses a value.
* The runtime carries a global fencing floor and a bounded, retained fence-record list for operators.
* Pruning a descriptive record or a ring entry never lowers a floor, and never causes a sequence to be
  reissued. An old *incarnation* - the pair of boot and sequence - therefore can never become current
  again. A pruned boot *identifier* could be registered as a brand-new incarnation, which receives a
  new sequence and carries none of the old authority; this is stated as a limitation in section 25.

## 10. Sessions, revalidation and currentness

Session identity is **not** authority. A session binds a `SessionId` to an epoch, a participant, a
worker boot, an incarnation sequence and a registration generation.

Fabric Epoch 1.0.0 implements the precise model **one session per worker incarnation**:

* A second session may not claim an incarnation that already holds one.
* Losing that session fences exactly that incarnation (`PROCESS_LOSS`, `SHUTDOWN` or
  `SESSION_PROTOCOL_VIOLATION` depending on how it ended) and leaves every other participant and
  every redundant incarnation untouched.
* `EpochRuntimeOptions::fence_on_session_loss = false` selects a durable-incarnation model
  instead, in which session loss leaves the incarnation live. The choice is explicit and tested in both
  directions.

**Revalidation** keeps durable identity and live authority separate. After an epoch advancement or a
coordinator restart:

```
durable participant record      survives
participant identity            survives
live authority                  does not survive
participant state               REVALIDATION_REQUIRED
```

A fresh `WorkerBootId` registration re-establishes authority. Downstream components never have to
delete durable identity in order to regain authority.

**Currentness** is never collapsed into a single boolean:

```
CURRENT   REVALIDATION_REQUIRED   FENCED   STALE_EPOCH   RETIRED   UNKNOWN   REVOKED
```

**Retirement** is an explicit operator action, not deletion. It fences every live incarnation, closes
the incarnation allocator, keeps the durable record in a retired state, and makes every later
registration and validation for that identity report the retirement. A retired identity can never be
revived by any historical incarnation.

## 11. Coordinator restart, split brain and consensus boundary

### What is proven

A coordinator that dies and is replaced by a fresh process:

* advances the durable epoch monotonically, by exactly one per restart, repeatedly;
* rejects all traffic from the previous epoch with `STALE_EPOCH` over the real wire;
* does not reactivate any pre-restart `WorkerBootId`: a hard kill records no fence, so
  `establish_epoch` clears every live incarnation and marks the participant
  `REVALIDATION_REQUIRED` - the incumbent boot is not silently live under the new epoch;
* keeps durable participant identity while discarding live authority;
* lets fresh incarnations re-establish authority without deleting durable records.

### What is explicitly not claimed

**Fabric Epoch 1.0.0 is single-authority. No distributed consensus is implemented.** The coordinator
is the only process permitted to mutate the durable epoch.

Fabric Epoch can make an old epoch stale *once a newer durable epoch exists*. It cannot by itself
guarantee that two isolated coordinators never independently believe they are authoritative, because
that requires a shared exclusion or consensus mechanism that Fabric Epoch does not implement.

Concretely:

* Stale-epoch fencing is proven: coordinator A at epoch N, coordinator B establishing N+1 from the same
  durable store, then A's traffic rejected everywhere that trusts Fabric Epoch.
* **Prevention of two simultaneously acting coordinators working from separate storage is not proven,
  and is not claimed.** Two coordinators pointed at different store directories will each establish
  their own epoch lineage; nothing in this runtime detects that.
* A standby process cannot become authoritative without advancing the durable epoch, but Fabric Epoch
  does not implement leader election between standbys.

## 12. Persistence and recovery

### Format

One versioned, integrity-checked image per store:

```
magic "FBEP" (4 bytes) | format version (4) | payload length (4)
| payload | SHA-256 trailer (32)
```

The trailer covers the magic, the version, the declared length and the whole payload, so a corrupted
header field is detected exactly like a corrupted payload byte. The payload is a deterministic
little-endian encoding with explicit lengths, canonical (sorted) scope sets and canonical participant
ordering; no raw C++ object layout is ever written.

The image persists: the current epoch, the coordinator incarnation, the durable state generation, the
registration and fencing floors, durable participant records, retained fence records, grants and
revocations, bounded transition history, and bounded idempotency records. It never persists live
sockets, live sessions, thread or process state, or process-local liveness.

**Persisted format version and wire protocol version are independent of the product version** and only
change when the representation actually changes.

### Durability ordering

Every acknowledged mutation has passed through: encode -> validate -> write to a uniquely named
temporary file -> flush and durability barrier -> atomic replacement. Acknowledgment happens only after
the replacement is durable. An independent reader observes an acknowledged registration or fence
without any explicit flush.

A crash before, during or after the temporary write can leave a partially written temporary file; such
a file is never authoritative, and the loader ignores it. A failed persistence barrier rolls the
in-memory state back and propagates an error.

### Rollback detection and its limit

The store maintains a sidecar watermark file recording the highest epoch ever persisted. On load, an
image whose epoch is below the watermark is rejected as a rollback. Images that are internally
inconsistent - a current epoch below a recorded transition target, a fencing floor at or below a
recorded fence generation, an active incarnation the allocator never issued, a boot listed as both
current and fenced, duplicated participant records, one worker boot bound to two participants,
delegated grants without a parent, absurd counts, trailing bytes - are rejected as corrupt.

**Genuine limitation:** a restore that replaces *both* the image and the watermark cannot be detected.
True external rollback protection requires externally trusted monotonic storage, which Fabric Epoch
1.0.0 does not implement and does not claim.

## 13. Distributed process model

Three real executables ship with the project:

| executable | role |
|---|---|
| `fabric-epoch-coordinator` | establishes a fresh coordinator incarnation, advances the durable epoch, serves the framed protocol |
| `fabric-epoch-worker` | generates a fresh `WorkerBootId`, registers under the current epoch, holds its session |
| `fabric-epoch-cli` | deterministic, script-friendly inspection and control client |

The distributed proofs use real independent operating-system processes, real loopback TCP sockets,
real `TerminateProcess` terminations, and real durable restarts. The worker process is asserted to
be **alive immediately before** it is terminated, so the observed session loss is caused by the
termination and not by a worker that gave up on its own. Ephemeral ports are requested (`--port 0`)
and learned from the coordinator's readiness line, so no test reuses a fixed port.

### Wire protocol

```
magic "FEP1" | protocol version | message type | flags | payload length
| sequence | epoch | request identifier (16) | integrity digest (32) | payload
```

Message types have stable explicit wire identifiers that are never derived from C++ enumerator
ordinals: HELLO, HELLO_ACK, REGISTER_PARTICIPANT, REGISTER_ACK, FENCE_WORKER, FENCE_ACK,
VALIDATE_AUTHORITY, VALIDATE_RESULT, ADVANCE_EPOCH, EPOCH_STATE_REQUEST, EPOCH_STATE_RESPONSE,
REVOKE_AUTHORITY, REVOKE_ACK, QUERY_PARTICIPANT, PARTICIPANT_STATE, SNAPSHOT_REQUEST,
SNAPSHOT_RESPONSE, EXPLAIN_REQUEST, EXPLAIN_RESPONSE, SHUTDOWN, ERROR, ADVANCE_EPOCH_ACK,
RETIRE_PARTICIPANT, RETIRE_ACK.

The integrity digest covers every semantic header field and the whole payload. Frame length is bounded
before any allocation proportional to the declared length occurs, and application requests are
additionally bounded by a separate request limit. Every payload decoder rejects trailing bytes and
malformed enum values rather than coercing them. A peer that stops in the middle of a frame is
disconnected once its receive budget expires, so a slow or hostile peer cannot pin a session
indefinitely.

## 14. Snapshots and deterministic digests

`AuthoritySnapshot` is an immutable value copy of authority state at one generation: the current
epoch, the coordinator boot, the state generation, the registration and fencing floors, participant
views, retained fences and grants, plus a 256-bit SHA-256 semantic digest.

The digest covers **semantic authority only**: epochs, identities, incarnations, generations, scope
sets, fence reasons and grant structure. It excludes process-local timestamps, sockets, thread
identifiers, addresses, record iteration order, operator notes, provenance text and operational
counters. Permuting the record vectors cannot change the digest.

Snapshot currentness is explicit: `is_current_for(epoch, generation)`. An old snapshot remains
inspectable but can never authorise mutation, because every mutating entry point re-checks live state
and every validation re-reads current state.

## 15. Security model

Fabric Epoch treats protocol input as untrusted and rejects, with tests: forged epochs, forged worker
boots, forged incarnation sequences, namespace confusion between identity domains, unauthorized
scopes, malformed and truncated encodings, oversized and absurd length declarations, integer overflow,
corrupted integrity fields, stale replays, reused request identifiers, resource exhaustion and
mid-frame stalls.

**Transport and authentication are explicit limitations:**

* the transport is **plain TCP**, intended for loopback or an already trusted fabric;
* integrity checking detects **corruption, not forgery** - it is not a MAC and the digest is not keyed;
* **no cryptographic peer authentication is implemented**; any peer that can reach the port can issue
  protocol requests, subject to the authority rules above;
* `WorkerBootId` and `CoordinatorBootId` are operating-system CSPRNG uniqueness tokens, not
  authentication secrets;
* the store path is treated as an opaque filesystem location; Fabric Epoch performs no path
  interpretation of its own.

## 16. Validation classification

### REAL

* real coordinator, worker and CLI processes;
* actual operating-system process termination;
* actual loopback TCP sockets;
* real persisted epoch state and real restart/recovery;
* real durable fences surviving a process kill;
* ephemeral ports, unique temporary workspaces, no shared fixed ports.

### SYNTHETIC

* large participant populations (1000 / 10000 / 100000) generated in-process;
* multi-site authority scopes and generated epoch histories;
* seeded deterministic randomized schedules for property and adversarial testing.

### UNSUPPORTED

* distributed consensus of any kind;
* external monotonic hardware rollback protection;
* multi-host partition testing - the environment available is a single host;
* cryptographic peer authentication;
* availability guarantees under real network partition.

No consensus, authentication, availability or hardware rollback guarantee is claimed anywhere in this
document.

---

## 17. Build

Requirements: a C++20 compiler and CMake 3.21 or newer. MSVC (Visual Studio 2022), GCC and Clang are
supported; the reference validation used MSVC 19.44 with `/W4 /WX /permissive-`.

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

`CMakePresets.json` provides `release`, `debug`, `analyze` and `asan`
configure/build presets.

| option | default | effect |
|---|---|---|
| `FABRIC_EPOCH_BUILD_TESTS` | ON | build the test suites |
| `FABRIC_EPOCH_BUILD_EXAMPLES` | ON | build the shipped examples |
| `FABRIC_EPOCH_BUILD_BENCHMARKS` | ON | build the benchmark executable |
| `FABRIC_EPOCH_BUILD_APPS` | ON | build the coordinator, worker and CLI |
| `FABRIC_EPOCH_BUILD_DISTRIBUTED_TESTS` | ON | build the suites that start real processes |
| `FABRIC_EPOCH_ENABLE_ANALYZE` | OFF | add the MSVC static analyzer |
| `FABRIC_EPOCH_WARNINGS_AS_ERRORS` | ON | treat first-party warnings as errors |

Warning settings are applied to first-party targets only and are never exported from the package.

## 18. Test

```
ctest --test-dir build/release --output-on-failure
# or directly
./build/release/tests/fabric_epoch_tests
./build/release/tests/fabric_epoch_tests --filter=distributed
./build/release/tests/fabric_epoch_tests --skip=scale
```

The suites are: `identity`, `scope`, `explanation`, `epoch_arithmetic`,
`persistence`, `corruption`, `registration`, `validation`, `fencing`,
`grants`, `reincarnation`, `snapshot_digest`, `protocol`, `limits`,
`concurrency`, `property`, `adversarial`, `scale` and `distributed`.

**No watchdog or framework timeout is configured anywhere** - not in CTest, not in the harness, not in
the test binaries. A hanging test is a defect and must surface as a hang. Two bounded waits exist, both
as product logic that produces an explicit structured failure rather than a silent pass:

* the coordinator's frame receive budget, which disconnects a peer that stops mid-frame;
* the distributed tests' observation budget for an external process, which becomes a failed assertion
  carrying a diagnostic.

Temporary workspaces are unique per process and derive their identity from operating-system entropy, so
concurrent test processes never share state and no stale store is ever reused.

## 19. Install and find_package

```
cmake --install build/release --prefix <prefix>
```

The installation contains the headers, the library, the three executables and a CMake package:

```cmake
find_package(FabricEpoch CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE SummonSoftwareLabs::FabricEpoch)
```

The installed package config performs a **fail-fast self-check**: it verifies that the documented target
`SummonSoftwareLabs::FabricEpoch` exists (CMake does not export ALIAS targets, so this is verified
rather than assumed), that the target declares its include directories, and that the umbrella header is
present at the installed location. Any of those failing turns into a clear
`FabricEpoch_NOT_FOUND_MESSAGE` instead of an obscure downstream error.

`consumer/` in this repository is an **independent CMake project** that validates exactly this
surface: it finds the installed package, compiles, links, creates epoch state, registers a participant,
validates authority, fences the worker, observes the stale rejection, flushes to durable storage and
reloads it. It is not part of the Fabric Epoch build and is never installed.

## 20. Examples

Nine focused examples ship in `examples/`, each using the public API only and each exiting non-zero
on any failed assertion:

```
basic_registration        authority_validation     worker_reincarnation
fencing                   epoch_advancement        stale_epoch_rejection
stale_boot_rejection      coordinator_restart      persistence_recovery
```

## 21. Benchmarks

```
./build/release/benchmarks/fabric_epoch_benchmarks [--only=<name>]
```

Benchmarks report **completed** operations and the measured throughput of the machine that ran them. No
machine-independent performance guarantee is stated or implied.

## 22. Command-line tools

```
fabric-epoch-coordinator --store <path> [--bind <address>] [--port <port>]
                         [--replacement-policy REQUIRE_EXPLICIT_FENCE|REPLACE_INCUMBENT]
                         [--allow-multi-incarnation] [--no-advance-on-open]

fabric-epoch-worker --port <port> --participant <id> --scope <KIND:SUBJECT> [...]
                    [--host <address>] [--boot <hex|generate>] [--policy <p>] [--idle]

fabric-epoch-cli [--host <address>] --port <port> <command> [options]
  version | show-epoch | advance-epoch | list-participants | show-participant
  list-active-workers | list-fences | list-grants | validate-authority | fence-worker
  revoke-grant | retire-participant | snapshot | explain | shutdown | inspect-store
```

All CLI output is deterministic `key=value` text suitable for scripting.

## 23. Versioning

One authoritative version constant in `include/fabric_epoch/version.hpp`:

```
product version            1.0.0
persisted format version   1
wire protocol version      1
```

CMake fails configuration if the project version and the header disagree. The CLI, the library, the
package config and the HELLO exchange all report the same product version string. The persisted and wire
versions change only when those representations change.

## 24. Thread safety, ownership and lifetime

* `EpochRuntime` is safe to use concurrently from many threads. Read paths take a shared lock;
  mutating paths take an exclusive lock. No callback, event or user code is ever invoked while a lock is
  held.
* Mutating operations serialise on the durable commit path on purpose: no reader is ever allowed to
  observe authority that is not yet durable.
* Mutations are transactional. Every mutating operation is wrapped in an undo log; if the durability
  barrier fails, the in-memory state and every index are restored before the error propagates.
* Snapshots, participant views, fence records and explanations are **value types**. Nothing mutable is
  ever handed out by reference.
* `EpochRuntime` is neither copyable nor movable; it owns its durable store and its indexes.
* `CoordinatorServer::stop()` is idempotent and never joins the calling thread, so a session
  handler must not call it. Shutdown is driven by a stop flag and bounded socket waits; the listener
  handle is only ever touched by its owning thread.
* `CoordinatorClient` serialises its own calls internally. `wait_for_disconnect()` blocks
  until the peer closes: an idle but healthy session is not treated as a disconnect, and the call is
  released from another thread by `close()` or `request_shutdown()`.
* Authority decisions are structured return values and are never exceptions. `EpochError` is
  thrown only for unrecoverable infrastructure failures: unusable persistence, a failed durability
  barrier, corruption, exhausted resources or an internal invariant break.

### Concurrency and lock-ordering audit

The following properties were inspected directly in the implementation rather than inferred from the
tests:

* **No read-to-write reacquisition.** Every mutating entry point takes the exclusive lock exactly once
  and then calls only private unlocked helpers; no public method is invoked while a lock is held.
* **No write re-entry.** The exclusive lock is never taken recursively. An operation that needs a
  second mutation performs it through the same private helper under the lock it already holds.
* **No callback re-entry.** No user callback, event sink or virtual hook exists on any locked path, so
  no re-entry into the runtime is possible while a lock is held.
* **No event emission under a lock.** Fences, revocations and epoch transitions are reported to the
  caller as returned values after the lock is released.
* **Session teardown is outside the session registry lock.** Session closure copies the binding, erases
  it, and only then performs the durable fence and the commit.
* **No worker or session thread is joined while holding a lock that thread needs.** The thread list is
  swapped out under the registry mutex and joined after it is released.
* **No coordinator stop under a session lock, and no self-join.** `stop()` is idempotent, sets a stop
  flag, joins the accept thread, then joins session threads with no mutex held. It is never called from
  a session thread, and the reaper only joins threads that have already signalled completion.
* **No reversed lock ordering.** Exactly two mutexes can appear on a server session path - the runtime
  lock and the server session-registry mutex - and they are never held simultaneously.
* **Snapshot digests are computed under the shared lock only**, from a snapshot value no other thread
  can reach.
* **No asynchronous capture outliving its owner.** Worker threads capture raw pointers only to objects
  whose lifetime strictly encloses the join performed by the owning destructor.
* **Rollback safety.** A failed durability barrier restores records by durable identity (not by vector
  position, which canonicalisation can change) and then rebuilds every secondary index.

## 25. Genuine limitations

1. **Single authority, no consensus.** See section 11. Stale-epoch fencing is proven; preventing two
   coordinators on entirely separate storage from each acting authoritative is not.
2. **No cryptographic authentication.** Plain TCP and unkeyed integrity digests. Reaching the port is
   enough to issue protocol requests, subject to the authority rules.
3. **Rollback detection is local.** A restore that replaces both the image and its watermark is
   undetectable without external monotonic storage.
4. **Exact fence-record retention is bounded.** Pruned boot identifiers lose exact rejection; the
   incarnation allocator and the fencing floors still guarantee that no pruned *incarnation's* authority
   can ever become valid again, and re-registering a pruned identifier yields a brand-new incarnation
   that carries none of the old authority.
5. **Limits bound what a process will add, not what it will load.** A store written under larger
   retention settings stays loadable; the configurable retention limits are enforced on the write paths
   that own them.
6. **Scale evidence is synthetic.** Participant populations above the small real-process proofs are
   generated in-process.
7. **No multi-host partition testing.** The environment available is a single host.

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
