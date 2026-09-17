# Segment Routing Fabric 1.0.0

**Summon Software Labs — Distributed Fabric Infrastructure / Fabric OS**

Segment Routing Fabric is the explicit governed segment-list construction, validation,
lifecycle and authority runtime of the Distributed Fabric Infrastructure stack. It answers
exactly one question, and refuses to answer any other:

> Where the environment explicitly supports segment routing, what exact governed segment
> list represents this path intent, are every segment and encoding capability current and
> legal under the present fabric generations and policy, what is the authoritative
> segment-list generation, and when must that list be rejected, fenced, superseded,
> retired, or revalidated?

A governed segment list is **control-plane intent**. It is not proof that hardware
installed it. This runtime never claims otherwise.

---

## 1. What this runtime is, and what it is not

### It owns

* `SegmentListId` and segment-list generations, lineage and bounded history.
* `SegmentId` abstractions and typed segment semantics (NODE, ADJACENCY, ENDPOINT,
  BINDING, POLICY).
* Ordered segment-list construction from explicit caller-supplied intent.
* Segment-list canonicalization that never destroys or normalizes away order.
* Supported segment profile and encoding identity, and the environment support gate.
* Capability and policy binding, and topology / Path Authority bindings where required.
* Segment-list lifecycle and currentness.
* Structural validation with a fixed rejection precedence.
* Maximum segment-depth enforcement and explicit duplicate/repetition rules.
* Explicit strict/loose semantics where a profile implements them.
* Supersession, withdrawal, revocation and retirement.
* Stale-list fencing and revalidation.
* Snapshots, order-aware diffs, explanations and deterministic digests.
* Persistence, conservative recovery and distributed authority over loopback TCP.

### It explicitly does **not** own

canonical identity · topology truth · link-state truth · port configuration · capability
truth · failure-domain truth · Fabric Epoch issuance · generic candidate-path computation ·
path legality · route lifecycle · ECMP · weighted routing · adaptive routing · convergence ·
path diversity · bandwidth reservation · traffic engineering · physical segment-routing
programming.

### Non-negotiable separations

A syntactically valid segment list does **not** mean that every segment exists, that every
segment is current, that every device supports the required profile, that the resulting path
is legal, that the route using it is installed, or that physical devices programmed it.
This runtime keeps the following strictly apart and never collapses one into another:

`SEGMENT SYNTAX` · `SEGMENT IDENTITY` · `SEGMENT CAPABILITY SUPPORT` ·
`SEGMENT-LIST STRUCTURAL VALIDITY` · `PATH AUTHORITY` · `ROUTE AUTHORITY` ·
`APPLIED HARDWARE STATE`

---

## 2. Environment support gate, and the proof classification

This runtime operates **only** under an explicitly declared supported segment-routing
profile. Profile support is authoritative data (`SegmentProfile`), not an enum value.

**Every profile shipped by 1.0.0 is ABSTRACT/SYNTHETIC.** There is no SR-MPLS encoding, no
SRv6 encoding, no SID structure, no label stack, no segment-routing header, no vendor SDK
and no device programming path anywhere in this codebase. The three built-in profiles are:

| profile | kinds | payload width | max depth | empty | consecutive repeats | non-consecutive repeats | loose |
|---|---|---|---|---|---|---|---|
| `abstract-governance-v1` | node, adjacency, endpoint, binding, policy | 4 | 8 | rejected | rejected | rejected | rejected |
| `abstract-ordered-v1` | node, adjacency, endpoint | 8 | 16 | rejected | rejected | permitted | permitted |
| `abstract-nullable-v1` | node, policy | 2 | 4 | permitted | permitted | permitted | rejected |

`SupportClassification::PhysicalUnverified` exists in the type system but is
unreachable: no profile in 1.0.0 uses it, and no code path can produce it. This is
deliberate. An enum value is not evidence.

### Proof classification of the shipped evidence

* **REAL** — actual operating-system process death and restart of the coordinator and
  worker executables, loopback TCP transport, real file-backed durable storage with
  replace-by-rename, and the linked library itself.
* **SYNTHETIC** — abstract segment profiles; fabricated node, adjacency, endpoint, binding,
  path, route and constraint evidence in `SyntheticEvidence`; the clock-free
  deterministic race injection hooks.
* **UNSUPPORTED / NOT CLAIMED** — physical SR programming, real SRv6, real SR-MPLS, physical
  forwarding, vendor SDKs, multi-host convergence, path computation, path legality, route
  ownership, traffic engineering, path diversity, consensus and cryptographic
  authentication.

---

## 3. Order is semantic

Segment lists are ordered. Canonicalization normalizes **each segment's encoding** and the
**payload width**, and does nothing else:

* it never sorts;
* it never reorders;
* it never deduplicates;
* it never repairs a broken sequence by finding an alternative one.

Two lists that differ only by the order of their segments have different canonical bytes,
different content digests and different sequence digests. All six permutations of three
segments produce six distinct digests (proved in `tests/unit/test_canonical.cpp`).

---

## 4. Segment-list identity and generations

A list has a **stable semantic identity** (`SegmentListId`, derivable from an explicit
scope + name key by `srf::derive_segment_list_id`) and a separate **mutable
generation** (`SegmentListGeneration`). The generation is never part of the identity.
Exact replay of unchanged semantics advances nothing: the revision, the generation and the
durable write count are all unchanged.

Every generation counter in this runtime is checked, monotonic and non-wrapping.
`Generation::next()` returns `std::nullopt` at the maximum value and the caller
surfaces that as an explicit exhaustion rejection
(`ListGenerationExhausted`, `CommitGenerationExhausted`, `EpochExhausted`).

---

## 5. Depth, empty lists, duplicates

* `Limits::max_segments_per_list` is enforced in create, replace, batch, persistence
  load and wire decode.
* An empty list is **rejected** unless the profile explicitly defines empty-list semantics
  (`ProfileEmptyNotPermitted` / `ListEmptyNotPermitted`). `abstract-nullable-v1`
  is the one shipped profile that does define them.
* Consecutive exact repeats are rejected unless the profile explicitly permits them
  (`SegmentDuplicateConsecutive`, gate `ProfileRepeatNotPermitted`).
* Non-consecutive repeats are legal only where the profile permits them
  (`SegmentDuplicateNonConsecutive`).

Ordered intent is **never** silently deduplicated. The offending index is always reported.

---

## 6. Structural validation and rejection precedence

Validation is never a `bool`. It returns a `ValidationResult` carrying a
deterministic **primary reason** plus a bounded, deduplicated, phase-ordered **complete
reason list**, with an explicit truncation flag when `Limits::max_reasons` is reached.

Fixed precedence — a defect in an earlier phase always dominates a defect in a later one:

| # | phase | covers |
|---|---|---|
| 1 | `WireDecode` | framing, magic, version, integrity, enums, bounded assembly |
| 2 | `CallerIdentity` | publisher, boot, attempt, scope, fencing |
| 3 | `EpochBootScope` | coordinator epoch and scope authority |
| 4 | `ListLifecycleGeneration` | list identity, generation, literal transition table |
| 5 | `Profile` | kind permission, payload width, depth, repeats, strictness, encoding |
| 6 | `Policy` | profile/kind/depth/ownership/derivation/replacement restrictions |
| 7 | `StructuralSegment` | encoding, references, reserved fields, entity existence, duplicates |
| 8 | `CapabilityEvidence` | required capability support at the bound generation |
| 9 | `TopologyEvidence` | topology generation, exact adjacency identity |
| 10 | `PathAuthority` | path existence, authority generation, scope |
| 11 | `RouteBinding` | route identity and generation |
| 12 | `ConstraintBinding` | constraint evaluation identity and generation |
| 13 | `ResourceLimits` | every configured bound |
| 14 | `Commit` | watermark re-verification and atomic commit |

The specification's ordering is exactly preserved. Phases 11 and 12 are external-authority
bindings of the same class as Path Authority and are inserted between it and the resource
limits without disturbing the required subsequence.

---

## 7. Canonical encoding

Explicit, deterministic, little-endian bytes. Nothing in this runtime ever serializes a raw
C++ object representation. Malformed lengths, invalid kinds, non-canonical alternate forms
and trailing bytes are all rejected. Two accepted spellings of the same segment — an
unpadded payload and a zero-padded one, or an unset encoding and the encoding the kind
requires — canonicalize to **identical** bytes.

Canonical sequence encoding (also mirrored by the independent oracle):

    u16  0xC503                       little endian
    u32  segment_count                little endian
    repeated per segment:
        u16 kind            u16 encoding
        u64 id              u64 generation
        u64 node            u64 adjacency      u64 endpoint     u64 binding
        u64 policy          u64 policy_generation
        u64 topology
        u16 payload_width
        u8[payload_width] payload   left aligned, zero padded

`Digest128` is an FNV-1a derived **non-cryptographic** integrity and identity digest.
It is used for identity, change detection and integrity trailers. **This runtime claims no
cryptographic authentication of any kind.**

---

## 8. Capability, topology and Path Authority binding

* Every list binds the exact capability evidence its profile and segment kinds require.
  Missing evidence is UNKNOWN/UNSUPPORTED and is never inferred into support. A
  capability-generation advance invalidates only the lists that actually bound it.
* Every ADJACENCY segment binds an exact `TopologyGeneration` and an exact authoritative
  `AdjacencyId`. An adjacency is never inferred from a label or a string name.
* A list that represents or constrains an exact path binds the exact `PathId` and
  `PathAuthorityGeneration`. Segment Routing Fabric never overrides Path Authority: a
  structurally valid segment list can still be legally unusable, and is reported as such.
* Route and constraint provenance is preserved explicitly. Derivation is never opaque, and
  it never performs a search.

---

## 9. Lifecycle

Nine states, a literal total transition table of 9 × 16 = 144 pairs (exactly 55 permitted,
every denial explicit), and a test that walks every pair.

`DECLARED` · `VALIDATING` · `ACTIVE` · `REVALIDATION_REQUIRED` ·
`WITHDRAWING` · `WITHDRAWN` · `SUPERSEDED` · `REVOKED` · `RETIRED`

* Replacement preserves lineage: the superseded revision is recorded in bounded history, and
  the new revision records what it superseded.
* Withdrawal is two-phase and removes availability at `WITHDRAWING` without erasing history.
* Revocation is an administrative prohibition. Retirement permanently closes the lineage.
* A `RETIRED`, `REVOKED`, `SUPERSEDED` or `WITHDRAWN` lineage can **never**
  reactivate through any path: replacement, revalidation and withdrawal are all denied.

Currentness distinguishes `CURRENT`, `REVALIDATION_REQUIRED`,
`STALE_PROFILE`, `STALE_POLICY`, `STALE_TOPOLOGY`, `STALE_CAPABILITY`,
`STALE_PATH_AUTHORITY`, `STALE_ROUTE_BINDING`, `STALE_CONSTRAINT_BINDING`,
`STALE_EPOCH` and `FENCED_PUBLISHER`.

**Revalidation is not repair.** It re-binds dependency generations to what is current now and
re-validates the existing ordered content. It never searches for an alternative segment
sequence and never silently fixes a broken list. If an adjacency generation has moved, the
list must be explicitly re-derived by replacement.

---

## 10. Stale-completion defense

Two-phase validation is mandatory:

1. snapshot dependency generations;
2. validate **outside** the broad store lock;
3. reacquire;
4. re-verify every generation and watermark;
5. commit atomically.

The potentially expensive external call (`IEvidenceSource::snapshot()`) is made outside
the lock. The cheap, contractually non-blocking generation read
(`IEvidenceSource::watermark()`) is the atomicity check under the lock — which is
precisely what step 4 requires.

The following in-flight races are proved, deterministically and concurrently: topology
advance, capability advance, Path Authority advance, profile advance, policy advance, epoch
advance and target-list generation advance. In every case the old result is rejected with
`CommitWatermarkChanged`, `EpochStale` or `ListGenerationConflict`, and it
never becomes current.

A mutation is **never acknowledged before it is durable**: the candidate state is persisted
first and only then swapped in. A persistence failure leaves the store exactly as it was.

---

## 11. Authority and fencing

Every mutation binds `CoordinatorEpoch`, `PublisherId`, `WorkerBootId`,
scope, expected generation and `MutationAttemptId`. The default is deny.

* Exact replay of an unchanged mutation is **idempotent** — nothing advances, nothing is written.
* Reuse of the same attempt id with different content is rejected
  (`CommitReplayPayloadMismatch`).
* A fresh process requires a fresh `WorkerBootId`; a fenced boot stays fenced permanently
  and can never be registered again (`BootAlreadyFenced`).
* Ending a writer's connection — including a real process kill — permanently fences its boot
  and degrades its lists' currentness to `FENCED_PUBLISHER`.

---

## 12. Persistence and recovery

Magic `SRFL`, explicit format version, checked record counts, deterministic two-level
encoding, CRC-32C integrity trailers over the header and the record payload, per-record size
bounds, and atomic replace-by-rename. Acknowledgement happens only after the durable write.

Recovery is conservative: **durable intent survives; live authority and unproven currentness
do not.** On load, the coordinator epoch is raised strictly above the persisted epoch, every
restored publisher is dead and its boot permanently fenced, every `ACTIVE` list becomes
`REVALIDATION_REQUIRED`, and terminal states (`RETIRED`, `REVOKED`,
`SUPERSEDED`, `WITHDRAWN`) are preserved. Availability is restored only by an
explicit, fully revalidated revalidation.

---

## 13. Distributed protocol

Explicit wire version, stable message identifiers, bounded frames, an integrity trailer over
the semantic header and the payload, strict enum validation and trailing-byte rejection.
Started partial frames have bounded assembly behaviour
(`Limits::max_frame_bytes`, `WireAssemblyOverflow`). No raw struct serialization.

Transport is real TCP over the loopback interface. It is **not** a WAN transport and this
runtime makes no availability claim about one. Remote segment-profile registration is not
implemented in 1.0.0; the coordinator declares its abstract profiles at start-up.

---

## 14. Resource limits

Every bound in `srf::Limits` is consulted on a real code path and has a test that proves
it: `max_lists`, `max_segments_per_list`, `max_segment_payload_bytes`,
`max_profiles`, `max_policies`, `max_batch_size`, `max_publishers`,
`max_frame_bytes`, `max_history`, `max_explanation_entries`,
`max_persistence_record_bytes`, `max_reasons`, `max_diff_entries`,
`max_total_segments`, `max_attempt_records`, `max_evidence_records`.
There are no dead limits.

---

## 15. Snapshots, diffs and explanations

A snapshot is an immutable value carrying the exact ordered list, profile, policy,
generations, bindings, lifecycle, currentness, authority, provenance and digest. It is never
invalidated by later mutations.

Diffs are order-aware and distinguish `Insert`, `Remove`, `Replace`,
`Reorder`, `ProfileChange`, `PolicyChange`, `DependencyChange`,
`CurrentnessChange`, `LifecycleChange`, `GenerationAdvance`,
`StrictnessChange`, `AuthorityChange`, `Created` and `Destroyed`.
A pure reordering is reported as `Reorder`, not as a set of replacements.

Explanations answer: why a list is `ACTIVE`; why it is not; why segment N is invalid;
which capability is missing; which topology generation is stale; why a list was superseded;
and why revalidation is required.

---

## 16. Build, test, install

Requires CMake 3.20+ and a C++20 compiler. The verified configuration is MSVC 19.44 x64 with
`/W4 /WX /permissive-`.

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release
    ctest --test-dir build -C Release --output-on-failure

Options: `SRF_BUILD_TESTS`, `SRF_BUILD_EXAMPLES`, `SRF_BUILD_BENCH`,
`SRF_BUILD_TOOLS`, `SRF_WERROR` (default ON), `SRF_ANALYZE` (MSVC
`/analyze`), `SRF_SANITIZE` (AddressSanitizer).

**No test timeouts are used anywhere.** A hanging test is a defect, so tests run plainly and
are allowed to complete naturally. Internal bounded waits exist only where exceeding the bound
is itself an explicit failure, and they are asserted on.

### Install and consume

    cmake --install build --config Release --prefix <prefix>

`cmake`

    find_package(SegmentRoutingFabric CONFIG REQUIRED)
    target_link_libraries(app PRIVATE SummonSoftwareLabs::SegmentRoutingFabric)

`consumer/` is an independent consumer that is built only against an installed prefix:
it creates an abstract profile list, proves capability rejection, creates a valid list,
reorders it and observes a changed digest, and revalidates.

---

## 17. Public API at a glance

`cmake`

    #include "srf/srf.hpp"

    srf::StoreConfig config{};
    config.evidence = std::make_shared<srf::SyntheticEvidence>();
    srf::SegmentListStore store(std::move(config));
    (void)store.register_profile(srf::abstract_governance_profile());
    (void)store.authority().register_publisher(publisher, boot, scope, store.authority().epoch(),
                                               store.limits());

    srf::ListDraft draft{};
    draft.id = srf::derive_segment_list_id("core", "primary");
    draft.scope = scope;
    draft.profile = srf::abstract_governance_profile().id;
    draft.segments.push_back(node_segment);
    const srf::MutationOutcome outcome = store.create_list(caller, draft);

---

## 18. Genuine limitations

* No physical segment routing of any kind. Every profile is ABSTRACT/SYNTHETIC.
* No path computation, no path legality, no route ownership, no traffic engineering.
* The distributed protocol is loopback-only and single-coordinator. There is no consensus
  and no cryptographic authentication.
* Remote profile and policy registration is not implemented; the coordinator declares its
  abstract profiles at start-up.
* GET returns a bounded list summary, not a full list transfer.
* Revalidation rebinds dependency generations; a changed adjacency generation requires an
  explicit replacement.
* The digest is a non-cryptographic integrity and identity digest.
* The standalone oracle in @BT@oracle/@BT@ is an evidence-free cross-check of the
  segment-sequence path. Production and the oracle agree exactly on validity, primary
  reason, the complete reason set, canonical bytes and digest across a 420-scenario
  randomized corpus. A small number of deliberate interpretation differences remain and
  are pinned as deterministic tests that assert both sides: the oracle does not check the
  profile generation; it reports @BT@ProfileIdInvalid@BT@ for a zero-width profile where
  production reports @BT@ProfilePayloadWidthExceeded@BT@; production adds
  @BT@ListEmptyNotPermitted@BT@ for an empty sequence and @BT@SegmentBindingIncomplete@BT@
  alongside @BT@SegmentMissingBinding@BT@ for all-zero references; production scans every
  segment for topology evidence while the oracle scans the bounded prefix; and the oracle
  reports equal pairs that a permitting profile allows.
* Four bounds (@BT@max_history@BT@, @BT@max_explanation_entries@BT@, @BT@max_diff_entries@BT@,
  @BT@max_attempt_records@BT@) are enforced by bounded truncation rather than by a
  @BT@LimitMax*@BT@ rejection; the adversarial suite pins the exact retained size and the
  truncation flag for those four and the exact rejection code for the other twelve.

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
