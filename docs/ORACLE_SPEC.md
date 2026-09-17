# Independent Sequence Oracle - wire specification

This document is the only input to the independent oracle implementation
(oracle/oracle_main.cpp). The oracle is written from this document and shares no
code, no header and no translation unit with the production library. It links
nothing from SegmentRoutingFabric.

The oracle reads one scenario from stdin and writes one result to stdout. The
scenario describes an evidence-free structural validation of an ordered abstract
segment sequence. External evidence (capability registries, topology, Path
Authority) is not available to this oracle; the rules below define exactly what
it must decide.

## Input grammar

Line oriented ASCII, LF separated, terminated by a line containing only END.
Fields are key=value separated by single spaces, in the order shown. All integer
values are unsigned decimal. Hex values are lowercase with an even number of
digits; the literal dash means an empty payload.

    PROFILE id=<u64> gen=<u64> kinds=<k>[,<k>...] depth=<n> width=<n> empty=<0|1> consec=<0|1> nonconsec=<0|1> loose=<0|1> reqcaps=<n>
    SEG index=<i> kind=<k> enc=<name|unset> id=<u64> gen=<u64> node=<u64> adj=<u64> endp=<u64> bind=<u64> pol=<u64> polgen=<u64> topo=<u64> payload=<hex|->
    SEG ...
    LIMITS maxseg=<n> maxpayload=<n>
    END

kinds is a comma separated list drawn from node, adjacency, endpoint, binding,
policy. The kind field of a SEG line is one of those, or invalid. enc is one of
abstractnodev1, abstractadjacencyv1, abstractendpointv1, abstractbindingv1,
abstractpolicyv1, or unset.

## Output grammar

    valid=<0|1>
    primary=<REASON>
    reasons=<REASON>[,<REASON>...]
    bytes=<hex>
    digest=<hex>

primary is the first element of reasons, or Ok when valid. reasons is the
complete, deduplicated reason set ordered by (phase, code, index, detail).
bytes is the canonical sequence encoding, hex encoded. digest is the 128-bit
digest of those bytes, printed as 32 lowercase hex digits, high lane first.

## Canonicalization performed before any check

For every segment, if enc is unset it becomes the encoding the kind requires.
This is a normalization only: it never changes order and never removes a segment.

| kind | encoding | required entity references |
|---|---|---|
| node | AbstractNodeV1 | node |
| adjacency | AbstractAdjacencyV1 | adj and topo |
| endpoint | AbstractEndpointV1 | endp |
| binding | AbstractBindingV1 | bind |
| policy | AbstractPolicyV1 | pol and polgen |

For a given kind, every entity reference the kind does not use is a reserved
field and must be 0. For a segment of kind policy, topo is reserved.

## Reason table

Only these reasons can be produced by this oracle.

| phase | code | name |
|---|---|---|
| 5 | 170 | ProfileUnknown |
| 5 | 171 | ProfileIdInvalid |
| 5 | 172 | ProfileGenerationInvalid |
| 5 | 174 | ProfileKindNotAllowed |
| 5 | 175 | ProfilePayloadWidthExceeded |
| 5 | 176 | ProfileMaxDepthExceeded |
| 5 | 177 | ProfileRepeatNotPermitted |
| 5 | 178 | ProfileEmptyNotPermitted |
| 5 | 180 | ProfileKindMismatch |
| 5 | 182 | ProfileStrictnessUnsupported |
| 7 | 250 | SegmentKindInvalid |
| 7 | 251 | SegmentIdInvalid |
| 7 | 252 | SegmentGenerationInvalid |
| 7 | 255 | SegmentReservedFieldSet |
| 7 | 256 | SegmentMissingBinding |
| 7 | 257 | SegmentUnexpectedBinding |
| 7 | 258 | SegmentBindingIncomplete |
| 7 | 259 | SegmentEncodingMismatch |
| 7 | 263 | SegmentCountMismatch |
| 7 | 264 | SegmentDuplicateConsecutive |
| 7 | 265 | SegmentDuplicateNonConsecutive |
| 8 | 290 | CapabilityMissing |
| 8 | 291 | CapabilityUnsupported |
| 8 | 293 | CapabilityEvidenceUnavailable |
| 9 | 320 | TopologyGenerationInvalid |
| 9 | 321 | TopologyGenerationMismatch |
| 9 | 322 | TopologyAdjacencyUnknown |
| 9 | 323 | TopologyAdjacencyStale |
| 9 | 325 | TopologyEvidenceUnavailable |
| 13 | 421 | LimitMaxSegmentsPerList |
| 13 | 422 | LimitMaxSegmentPayloadBytes |

## Checks, in phase order

### Phase 5 - profile

Emitted for the profile as a whole, with index=0:

* width equal to 0 -> ProfileIdInvalid, and the rest of the profile phase stops.
* segments.size() > depth -> ProfileMaxDepthExceeded, detail = depth.
* segments empty and empty=0 -> ProfileEmptyNotPermitted.
* loose=0 and the scenario strictness is loose -> ProfileStrictnessUnsupported.
  Scenarios are always strict, so this reason is never produced; it is listed so
  the table is total.
* Repetition gate: for every pair (j, i) with j < i whose canonical segments are
  equal, if j + 1 == i and consec=0 emit ProfileRepeatNotPermitted at index=i; if
  j + 1 != i and nonconsec=0 emit ProfileRepeatNotPermitted at index=i. Pairs are
  visited with i ascending in the outer loop and j ascending in the inner loop.
  Identical (reason, index, detail) triples are deduplicated by the ordering rule.

Emitted per segment index i ascending; at most maxseg + 1 segments are scanned:

* kind not in kinds -> ProfileKindNotAllowed, detail = kind code.
* payload.size() > width -> ProfilePayloadWidthExceeded, detail = width.
* the required encoding of an unknown kind is invalid -> ProfileKindMismatch.

Kind codes: invalid=0, node=1, adjacency=2, endpoint=3, binding=4, policy=5.

### Phase 7 - structural

Per segment index ascending, over the same scanned prefix:

* kind == invalid -> SegmentKindInvalid, and this segment contributes nothing else.
* id == 0 -> SegmentIdInvalid.
* gen == 0 -> SegmentGenerationInvalid.
* explicit enc differs from the kind required encoding -> SegmentEncodingMismatch,
  detail = the supplied encoding code.
* a required entity reference is missing, including topo == 0 for adjacency and
  polgen == 0 for policy -> SegmentBindingIncomplete.
* a reference the kind does not use is non-zero -> SegmentReservedFieldSet. A
  reserved field is reported only when the field is non-zero.
* every entity reference is 0, including for a kind that requires one ->
  SegmentMissingBinding.

Encoding codes: unset=0, AbstractNodeV1=1, AbstractAdjacencyV1=2,
AbstractEndpointV1=3, AbstractBindingV1=4, AbstractPolicyV1=5.

Duplicate detection then runs over the same scanned prefix, i ascending in the
outer loop and j ascending in the inner loop. Equal canonical segments with
j + 1 == i produce SegmentDuplicateConsecutive at index=i, detail = the id of
segment i. Otherwise they produce SegmentDuplicateNonConsecutive at index=i with
the same detail.

Two segments are canonically equal when every scalar field is equal and their
payloads are equal after zero padding both to width.

SegmentCountMismatch and SegmentUnexpectedBinding are listed for table
completeness; a well formed scenario cannot produce them.

### Phase 8 - capability evidence

The oracle has no capability evidence.

* reqcaps > 0 -> CapabilityEvidenceUnavailable at index=0.

### Phase 9 - topology evidence

The oracle has no topology evidence.

* any scanned segment has kind == adjacency -> TopologyEvidenceUnavailable at
  index=0, emitted once.

### Phase 13 - resource limits

* segments.size() > maxseg -> LimitMaxSegmentsPerList, detail = maxseg.
* for each segment index ascending, payload.size() > maxpayload ->
  LimitMaxSegmentPayloadBytes at that index, detail = maxpayload.

## Ordering, deduplication and truncation

Sort every produced reason by (phase, code, index, detail) ascending, then remove
duplicate (code, index, detail) triples. The primary reason is the first element.
Scenarios are generated so that no more than 24 distinct reasons are produced.

## Canonical sequence encoding

    u16  0xC503                     little endian
    u32  segment_count              little endian
    repeated segment_count times:
        u16 kind                    little endian
        u16 encoding                little endian
        u64 id                      little endian
        u64 generation              little endian
        u64 node                    little endian
        u64 adjacency               little endian
        u64 endpoint                little endian
        u64 binding                 little endian
        u64 policy                  little endian
        u64 policy_generation       little endian
        u64 topology                little endian
        u16 payload_width           little endian
        u8[payload_width] payload   left aligned, zero padded

The payload is written at width bytes exactly. A payload longer than width is
truncated to width by the encoder; the oracle must still emit bytes for such
scenarios.

## Digest

128-bit digest of the canonical bytes:

    PRIME = 0x00000100000001B3
    A     = 0xCBF29CE484222325
    B     = 0x9E3779B97F4A7C15
    C     = 0x2545F4914F6CDD1D

    fnv(seed, data): h = seed; for each byte b: h = (h XOR b) * PRIME  (mod 2^64)
    hi = fnv(A, data)
    lo = fnv(B, data); lo = fnv(lo, le64(len(data))); lo = fnv(lo XOR C, data)
    if hi == 0 and lo == 0: lo = 1

digest prints %016llx of hi followed by %016llx of lo.
