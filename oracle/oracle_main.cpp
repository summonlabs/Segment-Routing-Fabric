// ===========================================================================
// oracle/oracle_main.cpp
//
// Independent sequence oracle for the Segment Routing Fabric wire
// specification (docs/ORACLE_SPEC.md).
//
// This translation unit is standalone by construction: it includes no header
// from include/srf, links nothing from the SegmentRoutingFabric library and
// uses only the C++20 standard library.
//
//   well formed scenario -> result block on stdout, exit code 0
//   malformed scenario   -> single "ERROR <reason>" line on stdout, exit code 2
//
// Implementation notes (reading of docs/ORACLE_SPEC.md):
//   * "the rest of the profile phase stops" after width == 0 is read as: no
//     further phase 5 reason is produced; later phases still run.
//   * the repetition gate and the phase 7 duplicate scan share one canonical
//     equality (every scalar field equal, payloads equal after zero padding to
//     width) and one scanned prefix (the first min(segments, maxseg + 1)).
//   * for the binding bullets, "every entity reference is 0" is treated as the
//     all-zero case (SegmentMissingBinding); SegmentBindingIncomplete is
//     reported when the references are partially set but a required one is 0.
// ===========================================================================

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ------------------------------------------------------------- input bounds
constexpr std::size_t kMaxLineBytes = 1u << 20;   // 1 MiB per input line
constexpr std::size_t kMaxLines = 1u << 18;       // 262144 input lines
constexpr std::size_t kMaxEncodedBytes = 1u << 24;  // 16 MiB canonical bytes

struct ScenarioError final {
    std::string reason;
};

[[noreturn]] void fail(std::string reason) {
    throw ScenarioError{std::move(reason)};
}

// ------------------------------------------------------------------- kinds
enum class Kind : u16 {
    Invalid = 0,
    Node = 1,
    Adjacency = 2,
    Endpoint = 3,
    Binding = 4,
    Policy = 5
};

enum class Enc : u16 {
    Unset = 0,
    AbstractNodeV1 = 1,
    AbstractAdjacencyV1 = 2,
    AbstractEndpointV1 = 3,
    AbstractBindingV1 = 4,
    AbstractPolicyV1 = 5
};

constexpr std::size_t kKindCount = 6;

// Encoding each kind requires (unset for the invalid kind, which has none).
constexpr Enc kRequiredEncoding[kKindCount] = {
    Enc::Unset,
    Enc::AbstractNodeV1,
    Enc::AbstractAdjacencyV1,
    Enc::AbstractEndpointV1,
    Enc::AbstractBindingV1,
    Enc::AbstractPolicyV1,
};

// Entity references used by each kind; anything not used is reserved and must
// be zero.  For kind policy, topo is reserved.  Indexed by kind code.
constexpr bool kUsesNode[kKindCount] = {false, true, false, false, false, false};
constexpr bool kUsesAdj[kKindCount] = {false, false, true, false, false, false};
constexpr bool kUsesTopo[kKindCount] = {false, false, true, false, false, false};
constexpr bool kUsesEndp[kKindCount] = {false, false, false, true, false, false};
constexpr bool kUsesBind[kKindCount] = {false, false, false, false, true, false};
constexpr bool kUsesPol[kKindCount] = {false, false, false, false, false, true};
constexpr bool kUsesPolgen[kKindCount] = {false, false, false, false, false, true};

struct KindName {
    std::string_view name;
    Kind kind;
};

constexpr KindName kKindNames[] = {
    {std::string_view{"invalid"}, Kind::Invalid},
    {std::string_view{"node"}, Kind::Node},
    {std::string_view{"adjacency"}, Kind::Adjacency},
    {std::string_view{"endpoint"}, Kind::Endpoint},
    {std::string_view{"binding"}, Kind::Binding},
    {std::string_view{"policy"}, Kind::Policy},
};

struct EncName {
    std::string_view name;
    Enc enc;
};

constexpr EncName kEncNames[] = {
    {std::string_view{"unset"}, Enc::Unset},
    {std::string_view{"abstractnodev1"}, Enc::AbstractNodeV1},
    {std::string_view{"abstractadjacencyv1"}, Enc::AbstractAdjacencyV1},
    {std::string_view{"abstractendpointv1"}, Enc::AbstractEndpointV1},
    {std::string_view{"abstractbindingv1"}, Enc::AbstractBindingV1},
    {std::string_view{"abstractpolicyv1"}, Enc::AbstractPolicyV1},
};

bool kindFromName(std::string_view name, Kind& out) {
    for (const KindName& entry : kKindNames) {
        if (entry.name == name) {
            out = entry.kind;
            return true;
        }
    }
    return false;
}

bool encFromName(std::string_view name, Enc& out) {
    for (const EncName& entry : kEncNames) {
        if (entry.name == name) {
            out = entry.enc;
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------ reason codes
constexpr u32 kProfileUnknown = 170;
constexpr u32 kProfileIdInvalid = 171;
constexpr u32 kProfileKindNotAllowed = 174;
constexpr u32 kProfilePayloadWidthExceeded = 175;
constexpr u32 kProfileMaxDepthExceeded = 176;
constexpr u32 kProfileRepeatNotPermitted = 177;
constexpr u32 kProfileEmptyNotPermitted = 178;
constexpr u32 kProfileKindMismatch = 180;
constexpr u32 kSegmentKindInvalid = 250;
constexpr u32 kSegmentIdInvalid = 251;
constexpr u32 kSegmentGenerationInvalid = 252;
constexpr u32 kSegmentReservedFieldSet = 255;
constexpr u32 kSegmentMissingBinding = 256;
constexpr u32 kSegmentBindingIncomplete = 258;
constexpr u32 kSegmentEncodingMismatch = 259;
constexpr u32 kSegmentDuplicateConsecutive = 264;
constexpr u32 kSegmentDuplicateNonConsecutive = 265;
constexpr u32 kCapabilityEvidenceUnavailable = 293;
constexpr u32 kTopologyEvidenceUnavailable = 325;
constexpr u32 kLimitMaxSegmentsPerList = 421;
constexpr u32 kLimitMaxSegmentPayloadBytes = 422;

constexpr u32 kPhaseProfile = 5;
constexpr u32 kPhaseStructural = 7;
constexpr u32 kPhaseCapability = 8;
constexpr u32 kPhaseTopology = 9;
constexpr u32 kPhaseLimits = 13;

struct ReasonNameEntry {
    u32 code;
    const char* name;
};

constexpr ReasonNameEntry kReasonNames[] = {
    {kProfileUnknown, "ProfileUnknown"},
    {kProfileIdInvalid, "ProfileIdInvalid"},
    {kProfileKindNotAllowed, "ProfileKindNotAllowed"},
    {kProfilePayloadWidthExceeded, "ProfilePayloadWidthExceeded"},
    {kProfileMaxDepthExceeded, "ProfileMaxDepthExceeded"},
    {kProfileRepeatNotPermitted, "ProfileRepeatNotPermitted"},
    {kProfileEmptyNotPermitted, "ProfileEmptyNotPermitted"},
    {kProfileKindMismatch, "ProfileKindMismatch"},
    {kSegmentKindInvalid, "SegmentKindInvalid"},
    {kSegmentIdInvalid, "SegmentIdInvalid"},
    {kSegmentGenerationInvalid, "SegmentGenerationInvalid"},
    {kSegmentReservedFieldSet, "SegmentReservedFieldSet"},
    {kSegmentMissingBinding, "SegmentMissingBinding"},
    {kSegmentBindingIncomplete, "SegmentBindingIncomplete"},
    {kSegmentEncodingMismatch, "SegmentEncodingMismatch"},
    {kSegmentDuplicateConsecutive, "SegmentDuplicateConsecutive"},
    {kSegmentDuplicateNonConsecutive, "SegmentDuplicateNonConsecutive"},
    {kCapabilityEvidenceUnavailable, "CapabilityEvidenceUnavailable"},
    {kTopologyEvidenceUnavailable, "TopologyEvidenceUnavailable"},
    {kLimitMaxSegmentsPerList, "LimitMaxSegmentsPerList"},
    {kLimitMaxSegmentPayloadBytes, "LimitMaxSegmentPayloadBytes"},
};

const char* reasonName(u32 code) {
    for (const ReasonNameEntry& entry : kReasonNames) {
        if (entry.code == code) {
            return entry.name;
        }
    }
    return "Unknown";
}

// --------------------------------------------------------------- scenario
struct Segment {
    Kind kind = Kind::Invalid;
    Enc enc = Enc::Unset;
    u64 id = 0;
    u64 gen = 0;
    u64 node = 0;
    u64 adj = 0;
    u64 endp = 0;
    u64 bind = 0;
    u64 pol = 0;
    u64 polgen = 0;
    u64 topo = 0;
    std::vector<u8> payload;  // exactly as supplied (raw size drives the checks)
};

struct Scenario {
    u64 profileId = 0;
    u64 profileGen = 0;
    bool kindAllowed[kKindCount] = {false, false, false, false, false, false};
    u64 depth = 0;
    u64 width = 0;
    u64 empty = 0;
    u64 consec = 0;
    u64 nonconsec = 0;
    u64 loose = 0;
    u64 reqcaps = 0;
    u64 maxseg = 0;
    u64 maxpayload = 0;
    std::vector<Segment> segments;
};

// Canonical segment: encoding normalised, payload truncated to width and with
// trailing zero bytes removed.  Two canonical segments are equal exactly when
// every scalar field is equal and the payloads match after zero padding to
// width, so this form is usable as an equality/map key directly.
struct CanonSegment {
    u16 kind = 0;
    u16 enc = 0;
    u64 id = 0;
    u64 gen = 0;
    u64 node = 0;
    u64 adj = 0;
    u64 endp = 0;
    u64 bind = 0;
    u64 pol = 0;
    u64 polgen = 0;
    u64 topo = 0;
    std::vector<u8> payload;
};

bool operator<(const CanonSegment& a, const CanonSegment& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.enc != b.enc) return a.enc < b.enc;
    if (a.id != b.id) return a.id < b.id;
    if (a.gen != b.gen) return a.gen < b.gen;
    if (a.node != b.node) return a.node < b.node;
    if (a.adj != b.adj) return a.adj < b.adj;
    if (a.endp != b.endp) return a.endp < b.endp;
    if (a.bind != b.bind) return a.bind < b.bind;
    if (a.pol != b.pol) return a.pol < b.pol;
    if (a.polgen != b.polgen) return a.polgen < b.polgen;
    if (a.topo != b.topo) return a.topo < b.topo;
    return a.payload < b.payload;
}

// ---------------------------------------------------------------- reasons
struct Reason {
    u32 phase = 0;
    u32 code = 0;
    u64 index = 0;
    u64 detail = 0;
};

// ---------------------------------------------------------------- parsing
bool hasKeyword(std::string_view line, std::string_view keyword) {
    if (line == keyword) {
        return true;
    }
    return line.size() > keyword.size() && line.compare(0, keyword.size(), keyword) == 0 &&
           line[keyword.size()] == ' ';
}

std::vector<std::string_view> splitFields(std::string_view line, const char* what) {
    std::vector<std::string_view> tokens;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = line.find(' ', start);
        if (pos == std::string_view::npos) {
            tokens.push_back(line.substr(start));
            break;
        }
        tokens.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    for (const std::string_view& token : tokens) {
        if (token.empty()) {
            fail(std::string{"malformed-"} + what);
        }
    }
    return tokens;
}

bool field(std::string_view token, std::string_view key, std::string_view& value) {
    if (token.size() <= key.size()) {
        return false;
    }
    if (token.compare(0, key.size(), key) != 0) {
        return false;
    }
    if (token[key.size()] != '=') {
        return false;
    }
    value = token.substr(key.size() + 1);
    return !value.empty();
}

u64 toU64(std::string_view text, const char* why) {
    if (text.empty()) {
        fail(why);
    }
    u64 acc = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            fail(why);
        }
        const u64 digit = static_cast<u64>(c - '0');
        if (acc > (std::numeric_limits<u64>::max() - digit) / 10u) {
            fail("integer-overflow");
        }
        acc = acc * 10u + digit;
    }
    return acc;
}

u64 toBit(std::string_view text, const char* why) {
    if (text == "0") {
        return 0;
    }
    if (text == "1") {
        return 1;
    }
    fail(why);
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

std::vector<u8> hexDecode(std::string_view text) {
    if (text.empty() || (text.size() % 2u) != 0u) {
        fail("invalid-hex");
    }
    std::vector<u8> out;
    out.reserve(text.size() / 2u);
    for (std::size_t i = 0; i < text.size(); i += 2u) {
        const int hi = hexNibble(text[i]);
        const int lo = hexNibble(text[i + 1u]);
        if (hi < 0 || lo < 0) {
            fail("invalid-hex");
        }
        out.push_back(static_cast<u8>((hi << 4) | lo));
    }
    return out;
}

std::vector<std::string> readLines(std::istream& in) {
    std::vector<std::string> lines;
    std::string line;
    char ch = 0;
    while (in.get(ch)) {
        if (ch == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(line);
            line.clear();
            if (lines.size() > kMaxLines) {
                fail("input-too-large");
            }
        } else {
            if (line.size() >= kMaxLineBytes) {
                fail("line-too-long");
            }
            line.push_back(ch);
        }
    }
    if (in.bad()) {
        fail("input-read-error");
    }
    if (!line.empty()) {
        if (line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
        if (lines.size() > kMaxLines) {
            fail("input-too-large");
        }
    }
    return lines;
}

void parseProfile(std::string_view line, Scenario& scenario) {
    const std::vector<std::string_view> tokens = splitFields(line, "profile-line");
    if (tokens.size() != 11u || tokens[0] != "PROFILE") {
        fail("malformed-profile");
    }
    std::string_view value;
    if (!field(tokens[1], "id", value)) fail("malformed-profile");
    scenario.profileId = toU64(value, "malformed-profile");
    if (!field(tokens[2], "gen", value)) fail("malformed-profile");
    scenario.profileGen = toU64(value, "malformed-profile");
    if (!field(tokens[3], "kinds", value)) fail("malformed-profile");
    {
        std::size_t start = 0;
        while (true) {
            const std::size_t pos = value.find(',', start);
            const std::string_view name =
                (pos == std::string_view::npos) ? value.substr(start) : value.substr(start, pos - start);
            Kind kind = Kind::Invalid;
            if (name.empty() || !kindFromName(name, kind) || kind == Kind::Invalid) {
                fail("invalid-kind");
            }
            scenario.kindAllowed[static_cast<std::size_t>(static_cast<u16>(kind))] = true;
            if (pos == std::string_view::npos) {
                break;
            }
            start = pos + 1u;
        }
    }
    if (!field(tokens[4], "depth", value)) fail("malformed-profile");
    scenario.depth = toU64(value, "malformed-profile");
    if (!field(tokens[5], "width", value)) fail("malformed-profile");
    scenario.width = toU64(value, "malformed-profile");
    if (!field(tokens[6], "empty", value)) fail("malformed-profile");
    scenario.empty = toBit(value, "malformed-profile");
    if (!field(tokens[7], "consec", value)) fail("malformed-profile");
    scenario.consec = toBit(value, "malformed-profile");
    if (!field(tokens[8], "nonconsec", value)) fail("malformed-profile");
    scenario.nonconsec = toBit(value, "malformed-profile");
    if (!field(tokens[9], "loose", value)) fail("malformed-profile");
    scenario.loose = toBit(value, "malformed-profile");
    if (!field(tokens[10], "reqcaps", value)) fail("malformed-profile");
    scenario.reqcaps = toU64(value, "malformed-profile");
}

void parseSegment(std::string_view line, Scenario& scenario) {
    const std::vector<std::string_view> tokens = splitFields(line, "segment-line");
    if (tokens.size() != 14u || tokens[0] != "SEG") {
        fail("malformed-segment");
    }
    Segment seg;
    std::string_view value;
    if (!field(tokens[1], "index", value)) fail("malformed-segment");
    if (toU64(value, "malformed-segment") != static_cast<u64>(scenario.segments.size())) {
        fail("segment-index-out-of-order");
    }
    if (!field(tokens[2], "kind", value)) fail("malformed-segment");
    if (!kindFromName(value, seg.kind)) fail("invalid-kind");
    if (!field(tokens[3], "enc", value)) fail("malformed-segment");
    if (!encFromName(value, seg.enc)) fail("invalid-encoding");
    if (!field(tokens[4], "id", value)) fail("malformed-segment");
    seg.id = toU64(value, "malformed-segment");
    if (!field(tokens[5], "gen", value)) fail("malformed-segment");
    seg.gen = toU64(value, "malformed-segment");
    if (!field(tokens[6], "node", value)) fail("malformed-segment");
    seg.node = toU64(value, "malformed-segment");
    if (!field(tokens[7], "adj", value)) fail("malformed-segment");
    seg.adj = toU64(value, "malformed-segment");
    if (!field(tokens[8], "endp", value)) fail("malformed-segment");
    seg.endp = toU64(value, "malformed-segment");
    if (!field(tokens[9], "bind", value)) fail("malformed-segment");
    seg.bind = toU64(value, "malformed-segment");
    if (!field(tokens[10], "pol", value)) fail("malformed-segment");
    seg.pol = toU64(value, "malformed-segment");
    if (!field(tokens[11], "polgen", value)) fail("malformed-segment");
    seg.polgen = toU64(value, "malformed-segment");
    if (!field(tokens[12], "topo", value)) fail("malformed-segment");
    seg.topo = toU64(value, "malformed-segment");
    if (!field(tokens[13], "payload", value)) fail("malformed-segment");
    if (value != "-") {
        seg.payload = hexDecode(value);
    }
    scenario.segments.push_back(std::move(seg));
}

void parseLimits(std::string_view line, Scenario& scenario) {
    const std::vector<std::string_view> tokens = splitFields(line, "limits-line");
    if (tokens.size() != 3u || tokens[0] != "LIMITS") {
        fail("malformed-limits");
    }
    std::string_view value;
    if (!field(tokens[1], "maxseg", value)) fail("malformed-limits");
    scenario.maxseg = toU64(value, "malformed-limits");
    if (!field(tokens[2], "maxpayload", value)) fail("malformed-limits");
    scenario.maxpayload = toU64(value, "malformed-limits");
}

Scenario parseScenario(const std::vector<std::string>& lines) {
    Scenario scenario;
    std::size_t at = 0;
    if (lines.empty() || !hasKeyword(lines[0], "PROFILE")) {
        fail("missing-profile");
    }
    parseProfile(lines[0], scenario);
    ++at;
    while (at < lines.size() && hasKeyword(lines[at], "SEG")) {
        parseSegment(lines[at], scenario);
        ++at;
    }
    if (at >= lines.size() || !hasKeyword(lines[at], "LIMITS")) {
        fail("missing-limits");
    }
    parseLimits(lines[at], scenario);
    ++at;
    if (at >= lines.size() || lines[at] != "END") {
        fail("missing-end");
    }
    ++at;
    // Blank lines after END are tolerated (trailing newline / CRLF artifacts).
    while (at < lines.size() && lines[at].empty()) {
        ++at;
    }
    if (at != lines.size()) {
        fail("trailing-input");
    }
    return scenario;
}

// ---------------------------------------------------------- canonical form
std::vector<CanonSegment> canonicalize(const Scenario& scenario) {
    std::vector<CanonSegment> canon;
    canon.reserve(scenario.segments.size());
    for (const Segment& seg : scenario.segments) {
        const std::size_t kindCode = static_cast<std::size_t>(static_cast<u16>(seg.kind));
        CanonSegment out;
        out.kind = static_cast<u16>(seg.kind);
        out.enc = (seg.enc == Enc::Unset) ? static_cast<u16>(kRequiredEncoding[kindCode])
                                          : static_cast<u16>(seg.enc);
        out.id = seg.id;
        out.gen = seg.gen;
        out.node = seg.node;
        out.adj = seg.adj;
        out.endp = seg.endp;
        out.bind = seg.bind;
        out.pol = seg.pol;
        out.polgen = seg.polgen;
        out.topo = seg.topo;
        const u64 kept = std::min<u64>(static_cast<u64>(seg.payload.size()), scenario.width);
        out.payload.assign(seg.payload.begin(), seg.payload.begin() + static_cast<std::ptrdiff_t>(kept));
        while (!out.payload.empty() && out.payload.back() == 0u) {
            out.payload.pop_back();
        }
        canon.push_back(std::move(out));
    }
    return canon;
}

std::size_t scannedCount(const Scenario& scenario) {
    const u64 limit = (scenario.maxseg == std::numeric_limits<u64>::max())
                          ? std::numeric_limits<u64>::max()
                          : scenario.maxseg + 1u;
    return static_cast<std::size_t>(
        std::min<u64>(static_cast<u64>(scenario.segments.size()), limit));
}

struct DuplicateFacts {
    bool consecutive = false;
    bool nonConsecutive = false;
};

// i is the index being examined; prior holds the ascending indices j < i whose
// canonical segment equals the one at i.
DuplicateFacts duplicateFacts(const std::vector<u64>& prior, u64 i) {
    DuplicateFacts facts;
    if (prior.empty()) {
        return facts;
    }
    const u64 previous = (i == 0u) ? std::numeric_limits<u64>::max() : i - 1u;
    facts.consecutive = (prior.back() == previous);
    facts.nonConsecutive = (prior.size() >= 2u) || (prior.front() != previous);
    return facts;
}

void emit(std::vector<Reason>& out, u32 phase, u32 code, u64 index, u64 detail) {
    Reason reason;
    reason.phase = phase;
    reason.code = code;
    reason.index = index;
    reason.detail = detail;
    out.push_back(reason);
}

std::vector<Reason> check(const Scenario& scenario, const std::vector<CanonSegment>& canon) {
    std::vector<Reason> reasons;
    const std::size_t scanned = scannedCount(scenario);

    // Duplicate facts over the scanned prefix, shared by phase 5 and phase 7.
    std::vector<DuplicateFacts> facts(canon.size());
    {
        std::map<CanonSegment, std::vector<u64>> seen;
        for (std::size_t i = 0; i < scanned; ++i) {
            std::vector<u64>& prior = seen[canon[i]];
            facts[i] = duplicateFacts(prior, static_cast<u64>(i));
            prior.push_back(static_cast<u64>(i));
        }
    }

    // ------------------------------------------------------------ phase 5
    bool profileStopped = false;
    if (scenario.width == 0u) {
        emit(reasons, kPhaseProfile, kProfileIdInvalid, 0u, 0u);
        profileStopped = true;
    }
    if (!profileStopped) {
        if (static_cast<u64>(scenario.segments.size()) > scenario.depth) {
            emit(reasons, kPhaseProfile, kProfileMaxDepthExceeded, 0u, scenario.depth);
        }
        if (scenario.segments.empty() && scenario.empty == 0u) {
            emit(reasons, kPhaseProfile, kProfileEmptyNotPermitted, 0u, 0u);
        }
        for (std::size_t i = 0; i < scanned; ++i) {
            if (facts[i].consecutive && scenario.consec == 0u) {
                emit(reasons, kPhaseProfile, kProfileRepeatNotPermitted, static_cast<u64>(i), 0u);
            }
            if (facts[i].nonConsecutive && scenario.nonconsec == 0u) {
                emit(reasons, kPhaseProfile, kProfileRepeatNotPermitted, static_cast<u64>(i), 0u);
            }
            const Segment& seg = scenario.segments[i];
            const std::size_t kindCode = static_cast<std::size_t>(static_cast<u16>(seg.kind));
            if (!scenario.kindAllowed[kindCode]) {
                emit(reasons, kPhaseProfile, kProfileKindNotAllowed, static_cast<u64>(i),
                     static_cast<u64>(kindCode));
            }
            if (static_cast<u64>(seg.payload.size()) > scenario.width) {
                emit(reasons, kPhaseProfile, kProfilePayloadWidthExceeded, static_cast<u64>(i),
                     scenario.width);
            }
            if (kRequiredEncoding[kindCode] == Enc::Unset) {
                emit(reasons, kPhaseProfile, kProfileKindMismatch, static_cast<u64>(i), 0u);
            }
        }
    }

    // ------------------------------------------------------------ phase 7
    for (std::size_t i = 0; i < scanned; ++i) {
        const Segment& seg = scenario.segments[i];
        const CanonSegment& can = canon[i];
        const std::size_t kindCode = static_cast<std::size_t>(static_cast<u16>(seg.kind));
        if (seg.kind == Kind::Invalid) {
            emit(reasons, kPhaseStructural, kSegmentKindInvalid, static_cast<u64>(i), 0u);
        } else {
            if (seg.id == 0u) {
                emit(reasons, kPhaseStructural, kSegmentIdInvalid, static_cast<u64>(i), 0u);
            }
            if (seg.gen == 0u) {
                emit(reasons, kPhaseStructural, kSegmentGenerationInvalid, static_cast<u64>(i), 0u);
            }
            if (can.enc != static_cast<u16>(kRequiredEncoding[kindCode])) {
                emit(reasons, kPhaseStructural, kSegmentEncodingMismatch, static_cast<u64>(i),
                     static_cast<u64>(can.enc));
            }
            bool requiredMissing = false;
            if (kUsesNode[kindCode] && seg.node == 0u) requiredMissing = true;
            if (kUsesAdj[kindCode] && seg.adj == 0u) requiredMissing = true;
            if (kUsesTopo[kindCode] && seg.topo == 0u) requiredMissing = true;
            if (kUsesEndp[kindCode] && seg.endp == 0u) requiredMissing = true;
            if (kUsesBind[kindCode] && seg.bind == 0u) requiredMissing = true;
            if (kUsesPol[kindCode] && seg.pol == 0u) requiredMissing = true;
            if (kUsesPolgen[kindCode] && seg.polgen == 0u) requiredMissing = true;
            bool reservedSet = false;
            if (!kUsesNode[kindCode] && seg.node != 0u) reservedSet = true;
            if (!kUsesAdj[kindCode] && seg.adj != 0u) reservedSet = true;
            if (!kUsesTopo[kindCode] && seg.topo != 0u) reservedSet = true;
            if (!kUsesEndp[kindCode] && seg.endp != 0u) reservedSet = true;
            if (!kUsesBind[kindCode] && seg.bind != 0u) reservedSet = true;
            if (!kUsesPol[kindCode] && seg.pol != 0u) reservedSet = true;
            if (!kUsesPolgen[kindCode] && seg.polgen != 0u) reservedSet = true;
            const bool allZero = seg.node == 0u && seg.adj == 0u && seg.endp == 0u && seg.bind == 0u &&
                                 seg.pol == 0u && seg.polgen == 0u && seg.topo == 0u;
            if (allZero) {
                emit(reasons, kPhaseStructural, kSegmentMissingBinding, static_cast<u64>(i), 0u);
            } else if (requiredMissing) {
                emit(reasons, kPhaseStructural, kSegmentBindingIncomplete, static_cast<u64>(i), 0u);
            }
            if (reservedSet) {
                emit(reasons, kPhaseStructural, kSegmentReservedFieldSet, static_cast<u64>(i), 0u);
            }
        }
        if (facts[i].consecutive) {
            emit(reasons, kPhaseStructural, kSegmentDuplicateConsecutive, static_cast<u64>(i), seg.id);
        }
        if (facts[i].nonConsecutive) {
            emit(reasons, kPhaseStructural, kSegmentDuplicateNonConsecutive, static_cast<u64>(i), seg.id);
        }
    }

    // ------------------------------------------------------------ phase 8
    if (scenario.reqcaps > 0u) {
        emit(reasons, kPhaseCapability, kCapabilityEvidenceUnavailable, 0u, 0u);
    }

    // ------------------------------------------------------------ phase 9
    for (std::size_t i = 0; i < scanned; ++i) {
        if (scenario.segments[i].kind == Kind::Adjacency) {
            emit(reasons, kPhaseTopology, kTopologyEvidenceUnavailable, 0u, 0u);
            break;
        }
    }

    // ----------------------------------------------------------- phase 13
    if (static_cast<u64>(scenario.segments.size()) > scenario.maxseg) {
        emit(reasons, kPhaseLimits, kLimitMaxSegmentsPerList, 0u, scenario.maxseg);
    }
    for (std::size_t i = 0; i < scenario.segments.size(); ++i) {
        if (static_cast<u64>(scenario.segments[i].payload.size()) > scenario.maxpayload) {
            emit(reasons, kPhaseLimits, kLimitMaxSegmentPayloadBytes, static_cast<u64>(i),
                 scenario.maxpayload);
        }
    }

    std::sort(reasons.begin(), reasons.end(), [](const Reason& a, const Reason& b) {
        if (a.phase != b.phase) return a.phase < b.phase;
        if (a.code != b.code) return a.code < b.code;
        if (a.index != b.index) return a.index < b.index;
        return a.detail < b.detail;
    });
    reasons.erase(std::unique(reasons.begin(), reasons.end(),
                              [](const Reason& a, const Reason& b) {
                                  return a.code == b.code && a.index == b.index && a.detail == b.detail;
                              }),
                  reasons.end());
    return reasons;
}

// ------------------------------------------------------- canonical encoding
void putU16(std::vector<u8>& out, u16 value) {
    out.push_back(static_cast<u8>(value & 0xFFu));
    out.push_back(static_cast<u8>((value >> 8) & 0xFFu));
}

void putU32(std::vector<u8>& out, u32 value) {
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
        out.push_back(static_cast<u8>((value >> shift) & 0xFFu));
    }
}

void putU64(std::vector<u8>& out, u64 value) {
    for (unsigned shift = 0; shift < 64u; shift += 8u) {
        out.push_back(static_cast<u8>((value >> shift) & 0xFFu));
    }
}

std::vector<u8> encodeSequence(const Scenario& scenario, const std::vector<CanonSegment>& canon) {
    const std::size_t perSegment = 78u;  // 2 + 2 + 9 * 8 + 2
    const u64 count = static_cast<u64>(canon.size());
    const u64 cap = static_cast<u64>(kMaxEncodedBytes);
    if (scenario.width > cap) {
        fail("output-too-large");
    }
    const u64 per = static_cast<u64>(perSegment) + scenario.width;
    if (count > (cap - 6u) / per) {
        fail("output-too-large");
    }
    std::vector<u8> out;
    out.reserve(static_cast<std::size_t>(6u + count * per));
    putU16(out, 0xC503u);
    putU32(out, static_cast<u32>(count));
    for (const CanonSegment& seg : canon) {
        putU16(out, seg.kind);
        putU16(out, seg.enc);
        putU64(out, seg.id);
        putU64(out, seg.gen);
        putU64(out, seg.node);
        putU64(out, seg.adj);
        putU64(out, seg.endp);
        putU64(out, seg.bind);
        putU64(out, seg.pol);
        putU64(out, seg.polgen);
        putU64(out, seg.topo);
        putU16(out, static_cast<u16>(scenario.width));
        for (u64 k = 0; k < scenario.width; ++k) {
            out.push_back(k < static_cast<u64>(seg.payload.size())
                              ? seg.payload[static_cast<std::size_t>(k)]
                              : static_cast<u8>(0u));
        }
    }
    return out;
}

// ------------------------------------------------------------------ digest
constexpr u64 kPrime = 0x00000100000001B3ull;
constexpr u64 kDigestA = 0xCBF29CE484222325ull;
constexpr u64 kDigestB = 0x9E3779B97F4A7C15ull;
constexpr u64 kDigestC = 0x2545F4914F6CDD1Dull;

u64 fnv(u64 seed, const u8* data, std::size_t length) {
    u64 h = seed;
    for (std::size_t i = 0; i < length; ++i) {
        h = (h ^ static_cast<u64>(data[i])) * kPrime;
    }
    return h;
}

void digest128(const std::vector<u8>& data, u64& hi, u64& lo) {
    hi = fnv(kDigestA, data.data(), data.size());
    lo = fnv(kDigestB, data.data(), data.size());
    u8 lengthBytes[8];
    const u64 length = static_cast<u64>(data.size());
    for (unsigned shift = 0; shift < 64u; shift += 8u) {
        lengthBytes[shift / 8u] = static_cast<u8>((length >> shift) & 0xFFu);
    }
    lo = fnv(lo, lengthBytes, 8u);
    lo = fnv(lo ^ kDigestC, data.data(), data.size());
    if (hi == 0u && lo == 0u) {
        lo = 1u;
    }
}

std::string toHex(const std::vector<u8>& bytes) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2u);
    for (const u8 byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0Fu]);
    }
    return out;
}

// -------------------------------------------------------------------- main
std::string run() {
    const std::vector<std::string> lines = readLines(std::cin);
    const Scenario scenario = parseScenario(lines);
    const std::vector<CanonSegment> canon = canonicalize(scenario);
    const std::vector<Reason> reasons = check(scenario, canon);
    const std::vector<u8> bytes = encodeSequence(scenario, canon);
    u64 hi = 0;
    u64 lo = 0;
    digest128(bytes, hi, lo);

    char digestText[33];
    std::snprintf(digestText, sizeof(digestText), "%016llx%016llx",
                  static_cast<unsigned long long>(hi), static_cast<unsigned long long>(lo));

    const bool valid = reasons.empty();
    std::string out;
    out += "valid=";
    out += valid ? "1" : "0";
    out += "\nprimary=";
    out += valid ? std::string{"Ok"} : std::string{reasonName(reasons.front().code)};
    out += "\nreasons=";
    for (std::size_t i = 0; i < reasons.size(); ++i) {
        if (i != 0u) {
            out += ',';
        }
        out += reasonName(reasons[i].code);
    }
    out += "\nbytes=";
    out += toHex(bytes);
    out += "\ndigest=";
    out += digestText;
    out += '\n';
    return out;
}

}  // namespace

int main() {
    std::ios_base::sync_with_stdio(false);
    std::string output;
    int code = 0;
    try {
        output = run();
    } catch (const ScenarioError& error) {
        output = "ERROR " + error.reason + "\n";
        code = 2;
    } catch (const std::bad_alloc&) {
        output = "ERROR out-of-memory\n";
        code = 2;
    }
    std::cout << output;
    std::cout.flush();
    return code;
}
