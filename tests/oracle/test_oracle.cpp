// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Independent cross-check against the standalone sequence oracle
// (docs/ORACLE_SPEC.md, oracle/oracle_main.cpp).
//
// The oracle is a second, independent implementation of evidence-free
// structural validation of an ordered abstract segment sequence. This suite
// renders scenarios into the oracle's stdin grammar, runs the real oracle
// executable as a child process, and compares its answer with the production
// library field by field: validity, primary reason, the complete reason set in
// (phase, code, index, detail) order, the canonical sequence bytes and the
// sequence digest.
//
// SCOPE OF THE RANDOMIZED CORPUS
// ------------------------------
// The oracle models phases 5 (profile), 7 (structural), 8 (capability), 9
// (topology) and 13 (limits) of an evidence-free sequence, and it makes a small
// number of explicit interpretation choices that the production sequence path
// does not share. The corpus is therefore generated strictly inside the joint
// scope, and every exclusion is a real, minimal, reproducible divergence that
// has its own test at the bottom of this file (SRF_TEST(oracle, divergence_*)):
//
//   * profile payload width 0        -> the oracle reports ProfileIdInvalid and
//                                       stops the profile phase; production does
//                                       neither.
//   * profile generation 0           -> production adds ProfileGenerationInvalid;
//                                       the oracle never checks gen.
//   * an empty sequence with the profile forbidding emptiness
//                                    -> production also adds the list-lifecycle
//                                       reason ListEmptyNotPermitted.
//   * a segment with every entity reference zero
//                                    -> production adds SegmentBindingIncomplete
//                                       next to SegmentMissingBinding; the oracle
//                                       emits SegmentMissingBinding alone.
//   * a payload wider than the width -> the oracle truncates it to the width,
//                                       production's encoder emits no payload
//                                       bytes at all.
//   * kind = invalid                 -> the oracle adds ProfileKindMismatch;
//                                       production cannot produce that code here.
//   * an adjacency past the scanned prefix (index > LIMITS maxseg)
//                                    -> production scans every segment for
//                                       topology evidence, the oracle only scans
//                                       the first maxseg + 1.
//   * a structural defect at index maxseg while the sequence is longer than
//     maxseg                         -> the oracle scans maxseg + 1 segments
//                                       structurally, production scans maxseg.
//   * a profile that permits repeats -> production suppresses the structural
//                                       duplicate reasons for the repeat kinds
//                                       the profile allows; the oracle reports
//                                       every equal pair regardless.
//
// One case is deliberately NOT in that list because the two implementations now
// agree on it, and it is pinned as an agreement test instead: a duplicate whose
// two spellings differ (unset versus the explicitly required encoding).
// segment_semantic_equal() resolves an unset encoding to the encoding the kind
// requires before comparing, so the raw sequence, the canonicalized sequence and
// the oracle all report the duplicate. The store-level reproduction of the
// earlier gap was a create_list of [node(3) with an unset encoding, node(3) with
// AbstractNodeV1] under a profile that forbids consecutive repeats: it used to be
// accepted while two byte-identical canonical segments were stored, whereas the
// uniformly spelled control was rejected with ProfileRepeatNotPermitted and
// SegmentDuplicateConsecutive[1]. The sequence-level guard is
// mixed_encoding_spelling_is_normalized_before_equality.
//
// Nothing else is relaxed: inside the joint scope every reason, byte and digest
// must match exactly. Every scenario is also spelled with one uniform encoding
// policy per kind, and the suite asserts that the raw and the canonicalized
// sequence produce identical reasons, so the comparison never depends on which
// spelling the production pipeline happens to use.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "fixtures.hpp"
#include "srf/srf.hpp"
#include "test_support.hpp"

namespace {

// ===========================================================================
// Oracle input writer
// ===========================================================================

[[nodiscard]] std::string_view kind_token(srf::SegmentKind kind) noexcept {
    switch (kind) {
        case srf::SegmentKind::Invalid: return "invalid";
        case srf::SegmentKind::Node: return "node";
        case srf::SegmentKind::Adjacency: return "adjacency";
        case srf::SegmentKind::Endpoint: return "endpoint";
        case srf::SegmentKind::Binding: return "binding";
        case srf::SegmentKind::Policy: return "policy";
    }
    return "invalid";
}

[[nodiscard]] std::string_view encoding_token(srf::SegmentEncodingId encoding) noexcept {
    switch (encoding) {
        case srf::SegmentEncodingId::Invalid: return "unset";
        case srf::SegmentEncodingId::AbstractNodeV1: return "abstractnodev1";
        case srf::SegmentEncodingId::AbstractAdjacencyV1: return "abstractadjacencyv1";
        case srf::SegmentEncodingId::AbstractEndpointV1: return "abstractendpointv1";
        case srf::SegmentEncodingId::AbstractBindingV1: return "abstractbindingv1";
        case srf::SegmentEncodingId::AbstractPolicyV1: return "abstractpolicyv1";
    }
    return "unset";
}

/// Lowercase, even length hex. The literal dash is the empty payload.
[[nodiscard]] std::string hex_encode(std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2u);
    for (const std::byte value : bytes) {
        const unsigned v = std::to_integer<unsigned>(value);
        out.push_back(kDigits[(v >> 4) & 0x0Fu]);
        out.push_back(kDigits[v & 0x0Fu]);
    }
    return out;
}

[[nodiscard]] std::string payload_token(const srf::ByteBuffer& payload) {
    return payload.empty() ? std::string{"-"} : hex_encode(payload);
}

/// kinds is a comma separated list of allowed kind names, never including
/// invalid. The oracle rejects an empty list as malformed input, so the writer
/// falls back to the first real kind; generators always declare at least one
/// allowed kind and the corpus asserts that they do.
[[nodiscard]] std::string kinds_token(const srf::SegmentProfile& profile) {
    static constexpr srf::SegmentKind kOrder[] = {
        srf::SegmentKind::Node, srf::SegmentKind::Adjacency, srf::SegmentKind::Endpoint,
        srf::SegmentKind::Binding, srf::SegmentKind::Policy};
    std::string out;
    for (const srf::SegmentKind kind : kOrder) {
        if (!profile.allows_kind(kind)) {
            continue;
        }
        if (!out.empty()) {
            out.push_back(',');
        }
        out += std::string(kind_token(kind));
    }
    return out.empty() ? std::string{"node"} : out;
}

struct Scenario {
    srf::SegmentProfile profile{};
    std::vector<srf::Segment> segments{};
    srf::Limits limits{};
};

/// Renders one scenario in the exact input grammar of docs/ORACLE_SPEC.md:
/// PROFILE (11 tokens), one SEG per segment (14 tokens), LIMITS (3 tokens), END.
[[nodiscard]] std::string render_scenario(const Scenario& scenario) {
    std::ostringstream out;
    out << "PROFILE id=" << scenario.profile.id.value()
        << " gen=" << scenario.profile.generation.value() << " kinds=" << kinds_token(scenario.profile)
        << " depth=" << scenario.profile.max_depth << " width=" << scenario.profile.payload_width
        << " empty=" << (scenario.profile.allow_empty ? 1 : 0)
        << " consec=" << (scenario.profile.allow_consecutive_repeats ? 1 : 0)
        << " nonconsec=" << (scenario.profile.allow_nonconsecutive_repeats ? 1 : 0)
        << " loose=" << (scenario.profile.allow_loose ? 1 : 0)
        << " reqcaps=" << scenario.profile.required_capabilities.size() << '\n';
    for (std::size_t index = 0; index < scenario.segments.size(); ++index) {
        const srf::Segment& segment = scenario.segments[index];
        out << "SEG index=" << index << " kind=" << kind_token(segment.kind)
            << " enc=" << encoding_token(segment.encoding) << " id=" << segment.id.value()
            << " gen=" << segment.generation.value() << " node=" << segment.node.value()
            << " adj=" << segment.adjacency.value() << " endp=" << segment.endpoint.value()
            << " bind=" << segment.binding.value() << " pol=" << segment.policy.value()
            << " polgen=" << segment.policy_generation.value() << " topo=" << segment.topology.value()
            << " payload=" << payload_token(segment.payload) << '\n';
    }
    out << "LIMITS maxseg=" << scenario.limits.max_segments_per_list
        << " maxpayload=" << scenario.limits.max_segment_payload_bytes << '\n';
    out << "END\n";
    return out.str();
}

// ===========================================================================
// Child process runner
// ===========================================================================

class Sandbox {
public:
    Sandbox() {
        static std::uint64_t counter = 0;
        ++counter;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        directory_ = std::filesystem::temp_directory_path() /
                     ("srf_oracle_" + std::to_string(stamp) + "_" + std::to_string(counter));
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
        std::filesystem::create_directories(directory_, ec);
    }
    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
    }
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }

private:
    std::filesystem::path directory_{};
};

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        return std::string{};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

/// Splits on LF and trims every carriage return: the oracle prints CRLF on
/// Windows and no field of the answer grammar can contain a CR.
[[nodiscard]] std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

[[nodiscard]] std::vector<std::string> split_tokens(const std::string& text, char separator) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char c : text) {
        if (c == separator) {
            tokens.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    tokens.push_back(current);
    return tokens;
}

[[nodiscard]] std::size_t token_count(const std::string& line) {
    std::size_t count = 0;
    bool in_token = false;
    for (const char c : line) {
        if (c == ' ') {
            in_token = false;
        } else if (!in_token) {
            in_token = true;
            ++count;
        }
    }
    return count;
}

struct OracleAnswer {
    bool answered{false};  ///< the five answer lines parsed
    bool valid{false};
    int exit_code{-1};
    std::string primary{};
    std::vector<std::string> reasons{};
    std::string bytes{};
    std::string digest{};
    std::string raw{};
    std::string error{};
};

void parse_answer(OracleAnswer& answer) {
    const std::vector<std::string> lines = split_lines(answer.raw);
    if (!lines.empty() && lines.front().rfind("ERROR", 0) == 0) {
        answer.error = "oracle reported a malformed scenario: " + lines.front();
        return;
    }
    if (lines.size() != 5u) {
        answer.error = "expected exactly five answer lines, got " + std::to_string(lines.size());
        return;
    }
    static constexpr const char* kPrefixes[] = {"valid=", "primary=", "reasons=", "bytes=",
                                                "digest="};
    for (std::size_t i = 0; i < 5u; ++i) {
        if (lines[i].rfind(kPrefixes[i], 0) != 0) {
            answer.error = std::string("answer line ") + std::to_string(i) + " does not start with " +
                           kPrefixes[i];
            return;
        }
    }
    const std::string valid = lines[0].substr(6u);
    if (valid != "0" && valid != "1") {
        answer.error = "valid field is not 0 or 1";
        return;
    }
    answer.valid = valid == "1";
    answer.primary = lines[1].substr(8u);
    const std::string reasons = lines[2].substr(8u);
    if (!reasons.empty()) {
        answer.reasons = split_tokens(reasons, ',');
    }
    answer.bytes = lines[3].substr(6u);
    answer.digest = lines[4].substr(7u);
    answer.answered = true;
}

/// Runs the real oracle on one scenario. The ChildProcess helper deliberately
/// does not capture stdout, so the redirection is performed by the shell with the
/// sandbox as the working directory: "cmd /c run_oracle.bat" on Windows, where the
/// one-line script is  "<oracle>" < scenario.txt > answer.txt. The redirection is
/// not handed to cmd on the command line because ChildProcess::spawn escapes the
/// quotes around an executable path that contains spaces, and cmd.exe does not
/// read a backslash-escaped quote as a quote. The answer is read back from the
/// file the shell wrote.
[[nodiscard]] OracleAnswer query_oracle(const Sandbox& sandbox, const std::string& scenario_text) {
    OracleAnswer answer;
    const std::filesystem::path scenario_path = sandbox.directory() / "scenario.txt";
    const std::filesystem::path answer_path = sandbox.directory() / "answer.txt";
    {
        std::ofstream out(scenario_path, std::ios::binary | std::ios::trunc);
        out << scenario_text;
        out.flush();
        if (!out.good()) {
            answer.error = "the scenario file could not be written";
            return answer;
        }
    }
#ifdef _WIN32
    constexpr const char* kShell = "cmd";
    constexpr const char* kScriptName = "run_oracle.bat";
#else
    constexpr const char* kShell = "/bin/sh";
    constexpr const char* kScriptName = "run_oracle.sh";
#endif
    {
        std::ofstream script(sandbox.directory() / kScriptName, std::ios::binary | std::ios::trunc);
        script << "@echo off\n\"" << SRF_ORACLE_EXE << "\" < scenario.txt > answer.txt\n";
        script.flush();
        if (!script.good()) {
            answer.error = "the runner script could not be written";
            return answer;
        }
    }
    std::error_code ec;
    std::filesystem::remove(answer_path, ec);

    std::vector<std::string> arguments;
#ifdef _WIN32
    arguments.emplace_back("/c");
#endif
    arguments.emplace_back(kScriptName);
    srf::ValidationResult spawn_result;
    srf::ChildProcess process = srf::ChildProcess::spawn(
        kShell, arguments, sandbox.directory().string(), spawn_result);
    if (!process.started()) {
        answer.error = "the oracle process could not be started: " + spawn_result.to_string();
        return answer;
    }
    answer.exit_code = process.wait();
    answer.raw = read_text(answer_path);
    parse_answer(answer);
    return answer;
}

// ===========================================================================
// Production side
// ===========================================================================

[[nodiscard]] srf::ValidationResult validate_sequence(const Scenario& scenario,
                                                      const std::vector<srf::Segment>& segments) {
    srf::SegmentSequenceRequest request{};
    request.limits = &scenario.limits;
    request.profile = &scenario.profile;
    request.segments = std::span<const srf::Segment>(segments);
    // Evidence-free: profile, structural, capability, topology and limits phases.
    request.evidence = nullptr;
    return srf::validate_segment_sequence(request);
}

/// Reason names in the production (phase, code, index, detail) order.
[[nodiscard]] std::vector<std::string> reason_names(const srf::ValidationResult& result) {
    std::vector<std::string> names;
    names.reserve(result.reasons().size());
    for (const srf::Reason& reason : result.reasons()) {
        names.emplace_back(srf::reason_code_name(reason.code));
    }
    return names;
}

struct SequenceAnswer {
    bool valid{false};
    std::string primary{};
    std::vector<std::string> reasons{};
    std::string bytes{};
    std::string digest{};
};

[[nodiscard]] SequenceAnswer production_answer(const Scenario& scenario,
                                               const std::vector<srf::Segment>& segments) {
    const srf::ValidationResult result = validate_sequence(scenario, segments);
    SequenceAnswer answer;
    answer.valid = result.ok();
    answer.primary = std::string(result.primary_name());
    answer.reasons = reason_names(result);
    answer.bytes = hex_encode(srf::canonical_sequence_bytes(segments, scenario.profile.payload_width));
    answer.digest = srf::sequence_digest(segments, scenario.profile.payload_width).hex();
    return answer;
}

// ===========================================================================
// Comparison
// ===========================================================================

[[nodiscard]] std::string join(const std::vector<std::string>& items, char separator) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0u) {
            out.push_back(separator);
        }
        out += items[i];
    }
    return out;
}

[[nodiscard]] std::string oracle_summary(const OracleAnswer& answer) {
    return "valid=" + std::string(answer.valid ? "1" : "0") + " primary=" + answer.primary +
           " reasons=" + join(answer.reasons, ',') + " bytes=" + answer.bytes +
           " digest=" + answer.digest;
}

[[nodiscard]] std::string production_summary(const SequenceAnswer& answer) {
    return "valid=" + std::string(answer.valid ? "1" : "0") + " primary=" + answer.primary +
           " reasons=" + join(answer.reasons, ',') + " bytes=" + answer.bytes +
           " digest=" + answer.digest;
}

struct CheckOutcome {
    bool matched{false};
    SequenceAnswer production{};
};

/// Runs one scenario through both implementations and records one failure
/// carrying the scenario text, the oracle line and the production line for every
/// field that disagrees.
[[nodiscard]] CheckOutcome cross_check(const Scenario& scenario, const Sandbox& sandbox,
                                       srf::test::Context& context, const std::string& label) {
    const std::string scenario_text = render_scenario(scenario);
    const OracleAnswer oracle = query_oracle(sandbox, scenario_text);
    const std::vector<srf::Segment> canonical = srf::canonicalize_segments(scenario.segments);
    CheckOutcome outcome;
    outcome.production = production_answer(scenario, canonical);
    if (!oracle.answered) {
        context.record(false,
                       label + ": no oracle answer (" + oracle.error + ") exit=" +
                           std::to_string(oracle.exit_code) + " raw=[" + oracle.raw +
                           "]\n  scenario:\n" + scenario_text,
                       __FILE__, __LINE__);
        return outcome;
    }

    std::vector<std::string> problems;
    if (oracle.exit_code != 0) {
        problems.push_back("oracle exit code");
    }
    if (oracle.valid != outcome.production.valid) {
        problems.push_back("valid");
    }
    if (oracle.primary != outcome.production.primary) {
        problems.push_back("primary");
    }
    if (oracle.reasons != outcome.production.reasons) {
        problems.push_back("reasons");
    }
    if (oracle.bytes != outcome.production.bytes) {
        problems.push_back("bytes");
    }
    if (oracle.digest != outcome.production.digest) {
        problems.push_back("digest");
    }
    // One uniform encoding spelling per kind makes the raw and the canonicalized
    // sequence interchangeable on both sides; assert that rather than assuming it.
    if (reason_names(validate_sequence(scenario, scenario.segments)) !=
        reason_names(validate_sequence(scenario, canonical))) {
        problems.push_back("raw/canonical production reason names");
    }
    if (!problems.empty()) {
        context.record(false,
                       label + ": mismatch in " + join(problems, ',') + "\n  scenario:\n" +
                           scenario_text + "  oracle:     " + oracle_summary(oracle) +
                           "\n  production: " + production_summary(outcome.production),
                       __FILE__, __LINE__);
        return outcome;
    }
    outcome.matched = true;
    return outcome;
}

// ===========================================================================
// Scenario builders
// ===========================================================================

[[nodiscard]] srf::SegmentProfile base_profile(std::uint32_t width, std::uint32_t depth) {
    srf::SegmentProfile profile{};
    profile.id = srf::SegmentProfileId{0x0001'0001ull};
    profile.generation = srf::SegmentProfileGeneration{1};
    profile.name = "oracle-cross-check";
    profile.support = srf::SupportClassification::AbstractSynthetic;
    profile.allowed_kinds_mask = static_cast<std::uint16_t>(
        srf::kind_bit(srf::SegmentKind::Node) | srf::kind_bit(srf::SegmentKind::Adjacency) |
        srf::kind_bit(srf::SegmentKind::Endpoint) | srf::kind_bit(srf::SegmentKind::Binding) |
        srf::kind_bit(srf::SegmentKind::Policy));
    profile.max_depth = depth;
    profile.payload_width = width;
    return profile;
}

[[nodiscard]] srf::Limits generous_limits() {
    srf::Limits limits{};
    limits.max_segments_per_list = 8;
    limits.max_segment_payload_bytes = 8;
    limits.max_reasons = 64;
    return limits;
}

[[nodiscard]] srf::Segment node_segment(std::uint64_t ordinal, bool referenced) {
    srf::Segment segment{};
    segment.kind = srf::SegmentKind::Node;
    segment.encoding = srf::SegmentEncodingId::AbstractNodeV1;
    segment.id = srf::SegmentId{0x3000u + ordinal};
    segment.generation = srf::SegmentGeneration{1};
    if (referenced) {
        segment.node = srf::NodeId{0x2000u + ordinal};
    }
    return segment;
}

[[nodiscard]] srf::Segment adjacency_segment(std::uint64_t ordinal) {
    srf::Segment segment{};
    segment.kind = srf::SegmentKind::Adjacency;
    segment.encoding = srf::SegmentEncodingId::AbstractAdjacencyV1;
    segment.id = srf::SegmentId{0x3000u + ordinal};
    segment.generation = srf::SegmentGeneration{1};
    segment.adjacency = srf::AdjacencyId{0x7000u + ordinal};
    segment.topology = srf::TopologyGeneration{1};
    return segment;
}

[[nodiscard]] srf::Segment endpoint_segment(std::uint64_t ordinal) {
    srf::Segment segment{};
    segment.kind = srf::SegmentKind::Endpoint;
    segment.encoding = srf::SegmentEncodingId::AbstractEndpointV1;
    segment.id = srf::SegmentId{0x3000u + ordinal};
    segment.generation = srf::SegmentGeneration{1};
    segment.endpoint = srf::EndpointId{0x8000u + ordinal};
    return segment;
}

[[nodiscard]] srf::Segment binding_segment(std::uint64_t ordinal) {
    srf::Segment segment{};
    segment.kind = srf::SegmentKind::Binding;
    segment.encoding = srf::SegmentEncodingId::AbstractBindingV1;
    segment.id = srf::SegmentId{0x3000u + ordinal};
    segment.generation = srf::SegmentGeneration{1};
    segment.binding = srf::BindingId{0xA000u + ordinal};
    return segment;
}

[[nodiscard]] srf::Segment policy_segment(std::uint64_t ordinal) {
    srf::Segment segment{};
    segment.kind = srf::SegmentKind::Policy;
    segment.encoding = srf::SegmentEncodingId::AbstractPolicyV1;
    segment.id = srf::SegmentId{0x3000u + ordinal};
    segment.generation = srf::SegmentGeneration{1};
    segment.policy = srf::SegmentPolicyId{0xD000u + ordinal};
    segment.policy_generation = srf::SegmentPolicyGeneration{1};
    return segment;
}

// ===========================================================================
// Randomized corpus
// ===========================================================================

using srf::test::Random;

constexpr std::uint64_t kCorpusSeed = 0x0AC1E05EEDull;
constexpr std::uint32_t kCorpusScenarios = 420;

/// One uniform encoding policy per scenario. The duplicate gates now resolve an
/// unset encoding before comparing (see
/// mixed_encoding_spelling_is_normalized_before_equality), so mixed spellings
/// would also be exact; the corpus keeps one policy per scenario anyway so that
/// the spelling dimension stays a deliberate, readable variable.
enum class Spelling : std::uint8_t { Required, Unset, WrongForKind };

[[nodiscard]] srf::SegmentEncodingId spelling_encoding(srf::SegmentKind kind, Spelling spelling,
                                                       Random& rng) {
    const srf::SegmentEncodingId required = srf::expected_encoding_for(kind);
    if (spelling == Spelling::Required) {
        return required;
    }
    if (spelling == Spelling::Unset) {
        return srf::SegmentEncodingId::Invalid;
    }
    static constexpr srf::SegmentEncodingId kAll[] = {
        srf::SegmentEncodingId::AbstractNodeV1, srf::SegmentEncodingId::AbstractAdjacencyV1,
        srf::SegmentEncodingId::AbstractEndpointV1, srf::SegmentEncodingId::AbstractBindingV1,
        srf::SegmentEncodingId::AbstractPolicyV1};
    srf::SegmentEncodingId candidates[5]{};
    std::size_t count = 0;
    for (const srf::SegmentEncodingId candidate : kAll) {
        if (candidate != required) {
            candidates[count] = candidate;
            ++count;
        }
    }
    return candidates[rng.below(static_cast<std::uint32_t>(count))];
}

/// Entity reference slots: 0 node, 1 adjacency, 2 endpoint, 3 binding, 4 policy,
/// 5 policy generation, 6 topology.
constexpr std::uint32_t kSlotNode = 0;
constexpr std::uint32_t kSlotAdjacency = 1;
constexpr std::uint32_t kSlotEndpoint = 2;
constexpr std::uint32_t kSlotBinding = 3;
constexpr std::uint32_t kSlotPolicy = 4;
constexpr std::uint32_t kSlotPolicyGeneration = 5;
constexpr std::uint32_t kSlotTopology = 6;

[[nodiscard]] bool slot_used_by(srf::SegmentKind kind, std::uint32_t slot) {
    switch (kind) {
        case srf::SegmentKind::Node: return slot == kSlotNode;
        case srf::SegmentKind::Adjacency:
            return slot == kSlotAdjacency || slot == kSlotTopology;
        case srf::SegmentKind::Endpoint: return slot == kSlotEndpoint;
        case srf::SegmentKind::Binding: return slot == kSlotBinding;
        case srf::SegmentKind::Policy:
            return slot == kSlotPolicy || slot == kSlotPolicyGeneration;
        case srf::SegmentKind::Invalid: break;
    }
    return false;
}

void set_slot(srf::Segment& segment, std::uint32_t slot, std::uint64_t value) {
    switch (slot) {
        case kSlotNode: segment.node = srf::NodeId{value}; break;
        case kSlotAdjacency: segment.adjacency = srf::AdjacencyId{value}; break;
        case kSlotEndpoint: segment.endpoint = srf::EndpointId{value}; break;
        case kSlotBinding: segment.binding = srf::BindingId{value}; break;
        case kSlotPolicy: segment.policy = srf::SegmentPolicyId{value}; break;
        case kSlotPolicyGeneration: segment.policy_generation = srf::SegmentPolicyGeneration{value}; break;
        default: segment.topology = srf::TopologyGeneration{value}; break;
    }
}

/// A reference the kind does not use is a reserved field: both implementations
/// report SegmentReservedFieldSet.
void set_random_reserved_slot(srf::Segment& segment, Random& rng) {
    std::uint32_t reserved[7]{};
    std::size_t count = 0;
    for (std::uint32_t slot = 0; slot < 7u; ++slot) {
        if (!slot_used_by(segment.kind, slot)) {
            reserved[count] = slot;
            ++count;
        }
    }
    if (count == 0u) {
        return;
    }
    set_slot(segment, reserved[rng.below(static_cast<std::uint32_t>(count))], 0x9100u);
}

/// Zeroes one required reference without ever leaving every reference at zero:
/// the all-zero case is a documented divergence (see
/// divergence_all_zero_references_add_binding_incomplete).
void zero_a_required_reference(srf::Segment& segment) {
    switch (segment.kind) {
        case srf::SegmentKind::Node: segment.node = srf::NodeId{}; break;
        case srf::SegmentKind::Adjacency: segment.topology = srf::TopologyGeneration{}; break;
        case srf::SegmentKind::Endpoint: segment.endpoint = srf::EndpointId{}; break;
        case srf::SegmentKind::Binding: segment.binding = srf::BindingId{}; break;
        case srf::SegmentKind::Policy: segment.policy_generation = srf::SegmentPolicyGeneration{}; break;
        case srf::SegmentKind::Invalid: break;
    }
}

void ensure_a_reference_survives(srf::Segment& segment) {
    if (segment.node.valid() || segment.adjacency.valid() || segment.endpoint.valid() ||
        segment.binding.valid() || segment.policy.valid()) {
        return;
    }
    if (segment.kind == srf::SegmentKind::Node) {
        segment.adjacency = srf::AdjacencyId{0x9500u};
    } else {
        segment.node = srf::NodeId{0x2500u};
    }
}

[[nodiscard]] srf::Segment build_segment(srf::SegmentKind kind, std::uint64_t ordinal,
                                         std::uint32_t width, Spelling spelling, Random& rng) {
    srf::Segment segment{};
    segment.kind = kind;
    segment.encoding = spelling_encoding(kind, spelling, rng);
    segment.id = srf::SegmentId{0x3000u + ordinal};
    segment.generation = srf::SegmentGeneration{1};
    switch (kind) {
        case srf::SegmentKind::Node: segment.node = srf::NodeId{0x2000u + ordinal}; break;
        case srf::SegmentKind::Adjacency:
            segment.adjacency = srf::AdjacencyId{0x7000u + ordinal};
            segment.topology = srf::TopologyGeneration{1};
            break;
        case srf::SegmentKind::Endpoint: segment.endpoint = srf::EndpointId{0x8000u + ordinal}; break;
        case srf::SegmentKind::Binding: segment.binding = srf::BindingId{0xA000u + ordinal}; break;
        case srf::SegmentKind::Policy:
            segment.policy = srf::SegmentPolicyId{0xD000u + ordinal};
            segment.policy_generation = srf::SegmentPolicyGeneration{1};
            break;
        case srf::SegmentKind::Invalid: break;
    }
    const std::uint32_t length = rng.below(width + 1u);
    for (std::uint32_t i = 0; i < length; ++i) {
        segment.payload.push_back(std::byte{static_cast<unsigned char>(rng.below(256u))});
    }
    return segment;
}

/// True when the sequence contains a pair that both implementations treat as
/// canonically equal. Production's predicate is the narrower of the two for the
/// payload widths this corpus uses (an over-wide payload is never equal for
/// production), so no duplicate here also means no duplicate for the oracle.
[[nodiscard]] bool has_canonical_duplicate(const Scenario& scenario) {
    const std::vector<srf::Segment> canonical = srf::canonicalize_segments(scenario.segments);
    for (std::size_t i = 1; i < canonical.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (srf::segment_semantic_equal(canonical[j], canonical[i],
                                            scenario.profile.payload_width)) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] srf::SegmentKind random_real_kind(Random& rng) {
    static constexpr srf::SegmentKind kKinds[] = {
        srf::SegmentKind::Node, srf::SegmentKind::Adjacency, srf::SegmentKind::Endpoint,
        srf::SegmentKind::Binding, srf::SegmentKind::Policy};
    return kKinds[rng.below(5u)];
}

/// Adjacency without topology evidence is always invalid, so a genuinely valid
/// scenario never uses it.
[[nodiscard]] srf::SegmentKind random_forwarding_kind(Random& rng) {
    static constexpr srf::SegmentKind kKinds[] = {srf::SegmentKind::Node, srf::SegmentKind::Endpoint,
                                                  srf::SegmentKind::Binding,
                                                  srf::SegmentKind::Policy};
    return kKinds[rng.below(4u)];
}

void damage_segment(srf::Segment& segment, Random& rng) {
    switch (rng.below(6u)) {
        case 0u: segment.id = srf::SegmentId{0}; break;
        case 1u: segment.generation = srf::SegmentGeneration{0}; break;
        case 2u: set_random_reserved_slot(segment, rng); break;
        case 3u:
            zero_a_required_reference(segment);
            set_random_reserved_slot(segment, rng);
            break;
        case 4u: zero_a_required_reference(segment); break;
        default: break;
    }
}

[[nodiscard]] Scenario make_scenario(Random& rng) {
    Scenario scenario{};
    const std::uint32_t width = 1u + rng.below(8u);  // never 0: a documented divergence
    scenario.profile = base_profile(width, 8);
    scenario.profile.id = srf::SegmentProfileId{0x0001'0100ull + rng.below(16u)};
    scenario.profile.generation = srf::SegmentProfileGeneration{1u + rng.below(3u)};
    scenario.limits = generous_limits();

    Spelling spelling = Spelling::Required;
    const std::uint32_t spelling_pick = rng.below(10u);
    if (spelling_pick >= 7u) {
        spelling = Spelling::Unset;
    } else if (spelling_pick >= 6u) {
        spelling = Spelling::WrongForKind;
    }

    const std::uint32_t count = (rng.below(12u) == 0u) ? 0u : (1u + rng.below(6u));
    const bool want_valid = rng.below(10u) < 4u;

    if (want_valid) {
        for (std::uint32_t i = 0; i < count; ++i) {
            const srf::SegmentKind kind = random_forwarding_kind(rng);
            scenario.segments.push_back(
                build_segment(kind, static_cast<std::uint64_t>(i) + 1u, width, spelling, rng));
        }
        scenario.profile.allowed_kinds_mask = static_cast<std::uint16_t>(
            srf::kind_bit(srf::SegmentKind::Node) | srf::kind_bit(srf::SegmentKind::Endpoint) |
            srf::kind_bit(srf::SegmentKind::Binding) | srf::kind_bit(srf::SegmentKind::Policy));
        scenario.profile.max_depth = count + rng.below(3u);
        scenario.profile.allow_empty = true;
        scenario.profile.allow_consecutive_repeats = rng.chance(50);
        scenario.profile.allow_nonconsecutive_repeats = rng.chance(50);
        scenario.limits.max_segments_per_list = count + rng.below(4u);
        scenario.limits.max_segment_payload_bytes = width + rng.below(4u);
    } else {
        for (std::uint32_t i = 0; i < count; ++i) {
            const srf::SegmentKind kind = random_real_kind(rng);
            srf::Segment segment =
                build_segment(kind, static_cast<std::uint64_t>(i) + 1u, width, spelling, rng);
            const std::uint32_t damages = rng.below(2u);
            for (std::uint32_t d = 0; d < damages; ++d) {
                damage_segment(segment, rng);
            }
            ensure_a_reference_survives(segment);
            // An exact copy is canonically equal on both sides, so it exercises
            // the repetition gate and the duplicate scan without touching the
            // mixed-spelling boundary.
            if (i > 0u && rng.chance(35)) {
                segment = scenario.segments[rng.below(i)];
            }
            scenario.segments.push_back(segment);
        }
        std::uint16_t mask = 0;
        for (std::uint32_t pick = 0; pick < 5u; ++pick) {
            const srf::SegmentKind kind = random_real_kind(rng);
            if (rng.chance(65)) {
                mask = static_cast<std::uint16_t>(mask | srf::kind_bit(kind));
            }
        }
        if (mask == 0u) {
            mask = srf::kind_bit(random_real_kind(rng));
        }
        scenario.profile.allowed_kinds_mask = mask;
        scenario.profile.max_depth = rng.below(7u);
        scenario.profile.allow_empty = rng.chance(35);
        scenario.profile.allow_consecutive_repeats = rng.chance(30);
        scenario.profile.allow_nonconsecutive_repeats = rng.chance(30);
        const std::uint32_t capabilities = rng.below(3u);
        for (std::uint32_t i = 0; i < capabilities; ++i) {
            scenario.profile.required_capabilities.push_back(srf::CapabilityKey{0x1001u + i});
        }
        scenario.limits.max_segments_per_list = rng.below(9u);
        scenario.limits.max_segment_payload_bytes = rng.below(9u);
    }

    // An empty sequence is only generated where the profile permits it: the
    // production list-lifecycle phase adds ListEmptyNotPermitted, which the
    // oracle's reason table does not contain (see
    // divergence_empty_sequence_adds_a_list_lifecycle_reason).
    if (scenario.segments.empty()) {
        scenario.profile.allow_empty = true;
    }
    // The production structural phase reports a duplicate only for the repeat
    // kinds the profile forbids, while the oracle reports every equal pair (see
    // divergence_profile_allowed_repeats_suppress_duplicates), so a scenario that
    // contains any canonical duplicate forbids both kinds.
    if (has_canonical_duplicate(scenario)) {
        scenario.profile.allow_consecutive_repeats = false;
        scenario.profile.allow_nonconsecutive_repeats = false;
    }
    // Every segment of a WrongForKind scenario carries an explicit encoding
    // mismatch, which is a structural reason, so this spelling never truncates the
    // scanned prefix.
    if (spelling == Spelling::WrongForKind &&
        static_cast<std::size_t>(scenario.limits.max_segments_per_list) <
            scenario.segments.size()) {
        scenario.limits.max_segments_per_list =
            static_cast<std::uint32_t>(scenario.segments.size());
    }
    // The oracle's topology phase scans only the first maxseg + 1 segments while
    // production scans all of them (see
    // divergence_adjacency_beyond_the_scanned_prefix), so every adjacency stays
    // inside the scanned prefix.
    for (std::size_t i = 0; i < scenario.segments.size(); ++i) {
        if (scenario.segments[i].kind == srf::SegmentKind::Adjacency &&
            i > static_cast<std::size_t>(scenario.limits.max_segments_per_list)) {
            scenario.limits.max_segments_per_list = static_cast<std::uint32_t>(i);
        }
    }
    // When the segment-count limit is exceeded, the oracle still scans segment
    // maxseg structurally and production does not (see
    // divergence_structural_scan_stops_one_segment_early), so that one segment is
    // replaced by a structurally inert one. It keeps the scenario's spelling, so
    // the scenario stays inside the uniform-spelling contract.
    const std::size_t size = scenario.segments.size();
    const std::size_t maxseg = scenario.limits.max_segments_per_list;
    if (size > maxseg) {
        scenario.segments[maxseg] =
            build_segment(scenario.segments[maxseg].kind, static_cast<std::uint64_t>(maxseg) + 1u,
                          width, spelling, rng);
    }
    return scenario;
}

}  // namespace

// ===========================================================================
// Writer grammar
// ===========================================================================

/// Store-level regression guard for the mixed-spelling duplicate gap described in
/// the file header. The governance profile forbids consecutive repeats, so two
/// spellings of the same node segment are a duplicate that a real caller must be
/// rejected for. If this test ever reports status Ok, an unset encoding has
/// stopped being resolved before equality and the gap is back: a create_list
/// would then store two byte-identical canonical segments under a profile that
/// forbids repeats, while the oracle reports
/// ProfileRepeatNotPermitted,SegmentDuplicateConsecutive for the same sequence.
SRF_TEST(oracle, store_rejects_mixed_encoding_spelling_of_a_duplicate) {
    srf::test::Harness harness;
    srf::Segment first = srf::test::node_segment(3);
    first.encoding = srf::SegmentEncodingId::Invalid;  // the unset spelling
    srf::Segment second = first;
    second.encoding = srf::SegmentEncodingId::AbstractNodeV1;  // the required encoding

    srf::ListDraft draft = harness.draft(1);
    draft.segments = {first, second};
    const srf::MutationOutcome outcome = harness.store().create_list(harness.caller(), draft);
    SRF_EXPECT_REJECTED(outcome, srf::ReasonCode::ProfileRepeatNotPermitted);
    SRF_EXPECT_REJECTED(outcome, srf::ReasonCode::SegmentDuplicateConsecutive);
    SRF_EXPECT_EQ(outcome.status, srf::StatusCode::Rejected);
    SRF_EXPECT(!harness.store().get(srf::test::list_id(1)).has_value());

    // The uniformly spelled control is rejected identically: the two spellings are
    // interchangeable, which is the property the oracle also implements.
    srf::Segment control_first = first;
    control_first.encoding = srf::SegmentEncodingId::AbstractNodeV1;
    srf::ListDraft control = harness.draft(2);
    control.segments = {control_first, control_first};
    const srf::MutationOutcome control_outcome =
        harness.store().create_list(harness.caller(), control);
    SRF_EXPECT_REJECTED(control_outcome, srf::ReasonCode::ProfileRepeatNotPermitted);
    SRF_EXPECT_REJECTED(control_outcome, srf::ReasonCode::SegmentDuplicateConsecutive);
    SRF_EXPECT_EQ(control_outcome.status, srf::StatusCode::Rejected);
    SRF_EXPECT(!harness.store().get(srf::test::list_id(2)).has_value());
}

SRF_TEST(oracle, writer_renders_the_documented_grammar) {
    Scenario scenario{};
    scenario.profile = base_profile(4, 8);
    scenario.profile.allow_empty = false;
    scenario.profile.required_capabilities = {srf::CapabilityKey{0x1001}};
    scenario.limits.max_segments_per_list = 8;
    scenario.limits.max_segment_payload_bytes = 16;

    srf::Segment unset_node = node_segment(1, true);
    unset_node.encoding = srf::SegmentEncodingId::Invalid;
    unset_node.payload = {std::byte{0x01}, std::byte{0x02}};

    srf::Segment invalid_kind{};
    invalid_kind.kind = srf::SegmentKind::Invalid;
    invalid_kind.id = srf::SegmentId{0x3009};
    invalid_kind.generation = srf::SegmentGeneration{1};

    srf::Segment policy = policy_segment(3);
    policy.payload = {std::byte{0x00}, std::byte{0xFF}};
    scenario.segments = {unset_node, invalid_kind, policy};

    const std::string text = render_scenario(scenario);
    const std::vector<std::string> lines = split_lines(text);
    SRF_EXPECT_EQ(lines.size(), static_cast<std::size_t>(6));
    if (lines.size() != 6u) {
        return;
    }
    SRF_EXPECT_EQ(token_count(lines[0]), static_cast<std::size_t>(11));
    SRF_EXPECT(lines[0].rfind("PROFILE id=", 0) == 0);
    SRF_EXPECT(lines[0].find(" kinds=node,adjacency,endpoint,binding,policy ") != std::string::npos);
    SRF_EXPECT(lines[0].find(" width=4 ") != std::string::npos);
    SRF_EXPECT(lines[0].find(" reqcaps=1") != std::string::npos);
    for (std::size_t i = 1; i <= 3u; ++i) {
        SRF_EXPECT_EQ(token_count(lines[i]), static_cast<std::size_t>(14));
        SRF_EXPECT(lines[i].rfind("SEG index=", 0) == 0);
    }
    SRF_EXPECT(lines[1].find(" enc=unset ") != std::string::npos);
    SRF_EXPECT(lines[1].find(" payload=0102") != std::string::npos);
    SRF_EXPECT(lines[2].find(" kind=invalid ") != std::string::npos);
    SRF_EXPECT(lines[2].find(" enc=unset ") != std::string::npos);
    SRF_EXPECT(lines[2].find(" payload=-") != std::string::npos);
    SRF_EXPECT(lines[3].find(" kind=policy ") != std::string::npos);
    SRF_EXPECT(lines[3].find(" enc=abstractpolicyv1 ") != std::string::npos);
    SRF_EXPECT(lines[3].find(" payload=00ff") != std::string::npos);
    SRF_EXPECT_EQ(token_count(lines[4]), static_cast<std::size_t>(3));
    SRF_EXPECT_EQ(lines[4], std::string("LIMITS maxseg=8 maxpayload=16"));
    SRF_EXPECT_EQ(lines[5], std::string("END"));
}

// ===========================================================================
// Deterministic cases inside the joint scope
// ===========================================================================

SRF_TEST(oracle, empty_sequence_is_legal_input) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(4, 4);
    scenario.profile.allow_empty = true;
    scenario.limits = generous_limits();
    const CheckOutcome outcome = cross_check(scenario, sandbox, srf_ctx, "empty sequence");
    SRF_EXPECT(outcome.matched);
    SRF_EXPECT(outcome.production.valid);
    SRF_EXPECT_EQ(outcome.production.primary, std::string("Ok"));
    SRF_EXPECT_EQ(join(outcome.production.reasons, ','), std::string(""));
    // A sequence header is the magic plus the count and nothing else.
    SRF_EXPECT_EQ(outcome.production.bytes, std::string("03c500000000"));
}

SRF_TEST(oracle, unset_encodings_are_normalized_identically) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(4, 8);
    scenario.limits = generous_limits();
    srf::Segment node = node_segment(1, true);
    node.encoding = srf::SegmentEncodingId::Invalid;  // normalized on both sides
    srf::Segment endpoint = endpoint_segment(2);
    endpoint.encoding = srf::SegmentEncodingId::Invalid;
    endpoint.payload = {std::byte{0x2A}};
    scenario.segments = {node, endpoint};
    const CheckOutcome outcome = cross_check(scenario, sandbox, srf_ctx, "unset encodings");
    SRF_EXPECT(outcome.matched);
    SRF_EXPECT(outcome.production.valid);
    SRF_EXPECT_EQ(outcome.production.reasons.size(), static_cast<std::size_t>(0));
    // The canonical encoding of an unset spelling is the required encoding: the
    // first segment is the magic, the count and a kind/encoding pair of 1 and 1.
    SRF_EXPECT(outcome.production.bytes.rfind("03c5020000000100", 0) == 0);
}

SRF_TEST(oracle, duplicates_are_reported_identically) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(4, 8);
    scenario.limits = generous_limits();
    const srf::Segment repeated = node_segment(1, true);
    scenario.segments = {repeated, repeated, endpoint_segment(2), repeated};
    const CheckOutcome outcome = cross_check(scenario, sandbox, srf_ctx, "duplicates");
    SRF_EXPECT(outcome.matched);
    SRF_EXPECT(!outcome.production.valid);
    // Index 1 is a consecutive repeat of index 0, index 3 is a non-consecutive
    // repeat of index 0; the profile forbids both, so each drives one profile
    // reason and one structural duplicate reason.
    SRF_EXPECT_EQ(join(outcome.production.reasons, ','),
                  std::string("ProfileRepeatNotPermitted,ProfileRepeatNotPermitted,"
                              "SegmentDuplicateConsecutive,SegmentDuplicateNonConsecutive"));
    SRF_EXPECT_EQ(outcome.production.primary, std::string("ProfileRepeatNotPermitted"));
}

SRF_TEST(oracle, limits_are_enforced_identically) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(4, 8);
    scenario.limits = generous_limits();
    scenario.limits.max_segments_per_list = 1;
    scenario.limits.max_segment_payload_bytes = 1;
    srf::Segment node = node_segment(1, true);
    node.payload = {std::byte{0x11}, std::byte{0x22}};
    srf::Segment endpoint = endpoint_segment(2);
    endpoint.payload = {std::byte{0x33}, std::byte{0x44}};
    scenario.segments = {node, endpoint};
    const CheckOutcome outcome = cross_check(scenario, sandbox, srf_ctx, "limits");
    SRF_EXPECT(outcome.matched);
    SRF_EXPECT_EQ(join(outcome.production.reasons, ','),
                  std::string("LimitMaxSegmentsPerList,LimitMaxSegmentPayloadBytes,"
                              "LimitMaxSegmentPayloadBytes"));
    SRF_EXPECT_EQ(outcome.production.primary, std::string("LimitMaxSegmentsPerList"));
}

SRF_TEST(oracle, capability_and_topology_evidence_agree) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(4, 8);
    scenario.profile.required_capabilities = {srf::CapabilityKey{0x1001}, srf::CapabilityKey{0x1002}};
    scenario.limits = generous_limits();
    scenario.segments = {adjacency_segment(1)};
    const CheckOutcome outcome = cross_check(scenario, sandbox, srf_ctx, "evidence");
    SRF_EXPECT(outcome.matched);
    SRF_EXPECT_EQ(join(outcome.production.reasons, ','),
                  std::string("CapabilityEvidenceUnavailable,TopologyEvidenceUnavailable"));
    SRF_EXPECT_EQ(outcome.production.primary, std::string("CapabilityEvidenceUnavailable"));
}

// ===========================================================================
// Randomized cross-check
// ===========================================================================

SRF_TEST(oracle, randomized_corpus_matches_the_oracle) {
    srf::test::set_active_seed(kCorpusSeed);
    Random rng(kCorpusSeed);
    Sandbox sandbox;

    std::size_t valid = 0;
    std::size_t invalid = 0;
    std::size_t mismatches = 0;
    std::size_t empty_masks = 0;
    std::size_t worst_reason_count = 0;
    std::vector<std::string> vocabulary;

    // docs/ORACLE_SPEC.md: scenarios are generated so that no more than 24
    // distinct reasons are produced. Candidates that exceed the budget are
    // discarded before they become part of the corpus.
    constexpr std::size_t kReasonBudget = 24;
    std::vector<Scenario> corpus;
    std::uint32_t candidates = 0;
    for (std::uint32_t attempt = 0; attempt < kCorpusScenarios * 40u; ++attempt) {
        ++candidates;
        Scenario candidate = make_scenario(rng);
        const std::vector<srf::Segment> canonical = srf::canonicalize_segments(candidate.segments);
        if (production_answer(candidate, canonical).reasons.size() > kReasonBudget) {
            continue;
        }
        corpus.push_back(std::move(candidate));
        if (corpus.size() >= kCorpusScenarios) {
            break;
        }
    }

    for (std::size_t index = 0; index < corpus.size(); ++index) {
        const Scenario& scenario = corpus[index];
        if (scenario.profile.allowed_kinds_mask == 0u) {
            ++empty_masks;
        }
        const CheckOutcome outcome =
            cross_check(scenario, sandbox, srf_ctx, "corpus case " + std::to_string(index));
        if (!outcome.matched) {
            ++mismatches;
        }
        if (outcome.production.valid) {
            ++valid;
        } else {
            ++invalid;
        }
        worst_reason_count = std::max(worst_reason_count, outcome.production.reasons.size());
        for (const std::string& reason : outcome.production.reasons) {
            vocabulary.push_back(reason);
        }
    }
    std::sort(vocabulary.begin(), vocabulary.end());
    vocabulary.erase(std::unique(vocabulary.begin(), vocabulary.end()), vocabulary.end());

    std::printf("    corpus: %zu scenarios from %u candidates, %zu valid, %zu invalid, "
                "%zu distinct reasons, worst case %zu reasons\n",
                corpus.size(), candidates, valid, invalid, vocabulary.size(), worst_reason_count);
    std::fflush(stdout);

    SRF_EXPECT_EQ(mismatches, static_cast<std::size_t>(0));
    SRF_EXPECT_EQ(valid + invalid, corpus.size());
    SRF_EXPECT(corpus.size() >= static_cast<std::size_t>(kCorpusScenarios));
    SRF_EXPECT(valid >= 60u);
    SRF_EXPECT(invalid >= 100u);
    SRF_EXPECT_EQ(empty_masks, static_cast<std::size_t>(0));
    // Every reason the joint scope can produce must actually be exercised, so the
    // corpus cannot pass by being uniformly valid or uniformly trivial.
    static constexpr const char* kRequiredReasons[] = {
        "ProfileKindNotAllowed",        "ProfileMaxDepthExceeded",
        "ProfileRepeatNotPermitted",    "SegmentIdInvalid",
        "SegmentGenerationInvalid",     "SegmentReservedFieldSet",
        "SegmentBindingIncomplete",     "SegmentEncodingMismatch",
        "SegmentDuplicateConsecutive",  "SegmentDuplicateNonConsecutive",
        "CapabilityEvidenceUnavailable", "TopologyEvidenceUnavailable",
        "LimitMaxSegmentsPerList",      "LimitMaxSegmentPayloadBytes"};
    std::size_t missing_reasons = 0;
    for (const char* expected : kRequiredReasons) {
        if (std::find(vocabulary.begin(), vocabulary.end(), expected) == vocabulary.end()) {
            ++missing_reasons;
            std::printf("    reason never produced by the corpus: %s\n", expected);
        }
    }
    SRF_EXPECT_EQ(missing_reasons, static_cast<std::size_t>(0));
    // docs/ORACLE_SPEC.md: scenarios are generated so that no more than 24
    // distinct reasons are produced.
    SRF_EXPECT(worst_reason_count <= 24u);
}

// ===========================================================================
// Documented divergences between the oracle and the production sequence path
// ===========================================================================

SRF_TEST(oracle, divergence_empty_sequence_adds_a_list_lifecycle_reason) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(4, 4);
    scenario.profile.allow_empty = false;
    scenario.limits = generous_limits();
    const OracleAnswer oracle = query_oracle(sandbox, render_scenario(scenario));
    const SequenceAnswer production = production_answer(scenario, scenario.segments);
    SRF_EXPECT(oracle.answered);
    SRF_EXPECT(!production.valid);
    SRF_EXPECT_EQ(oracle.valid, production.valid);
    // Production rejects the empty sequence in the list-lifecycle phase as well.
    SRF_EXPECT_EQ(join(production.reasons, ','),
                  std::string("ListEmptyNotPermitted,ProfileEmptyNotPermitted"));
    SRF_EXPECT_EQ(join(oracle.reasons, ','), std::string("ProfileEmptyNotPermitted"));
    SRF_EXPECT_EQ(production.primary, std::string("ListEmptyNotPermitted"));
    SRF_EXPECT_EQ(oracle.primary, std::string("ProfileEmptyNotPermitted"));
    // The canonical bytes are unaffected: both sides encode the empty sequence.
    SRF_EXPECT_EQ(oracle.bytes, production.bytes);
    SRF_EXPECT_EQ(oracle.digest, production.digest);
}

SRF_TEST(oracle, divergence_all_zero_references_add_binding_incomplete) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(4, 4);
    scenario.limits = generous_limits();
    scenario.segments = {node_segment(1, false)};  // every entity reference is zero
    const OracleAnswer oracle = query_oracle(sandbox, render_scenario(scenario));
    const SequenceAnswer production = production_answer(scenario, srf::canonicalize_segments(scenario.segments));
    SRF_EXPECT(oracle.answered);
    // Production reports the all-zero case as both incomplete and missing.
    SRF_EXPECT_EQ(join(production.reasons, ','),
                  std::string("SegmentMissingBinding,SegmentBindingIncomplete"));
    SRF_EXPECT_EQ(join(oracle.reasons, ','), std::string("SegmentMissingBinding"));
    SRF_EXPECT_EQ(production.primary, std::string("SegmentMissingBinding"));
    SRF_EXPECT_EQ(oracle.primary, std::string("SegmentMissingBinding"));
    SRF_EXPECT_EQ(oracle.bytes, production.bytes);
    SRF_EXPECT_EQ(oracle.digest, production.digest);
}

// Formerly a pinned divergence: production's encoder used to refuse an over-wide
// payload and write no payload bytes at all. It now truncates to the width exactly
// as the oracle does, so both sides agree byte for byte.
SRF_TEST(oracle, over_wide_payload_is_encoded_identically) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(2, 4);
    scenario.limits = generous_limits();
    srf::Segment node = node_segment(1, true);
    node.payload = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    scenario.segments = {node};
    const OracleAnswer oracle = query_oracle(sandbox, render_scenario(scenario));
    const SequenceAnswer production = production_answer(scenario, scenario.segments);
    SRF_EXPECT(oracle.answered);
    SRF_EXPECT_EQ(join(production.reasons, ','), std::string("ProfilePayloadWidthExceeded"));
    SRF_EXPECT_EQ(join(oracle.reasons, ','), std::string("ProfilePayloadWidthExceeded"));
    // Both sides write the payload truncated to the width: 6 header bytes plus
    // 78 bytes per segment plus 2 payload bytes, printed as 172 hex digits.
    SRF_EXPECT_EQ(production.bytes.size(), static_cast<std::size_t>(172));
    SRF_EXPECT_EQ(oracle.bytes.size(), static_cast<std::size_t>(172));
    SRF_EXPECT_EQ(oracle.bytes, production.bytes);
    SRF_EXPECT_EQ(oracle.digest, production.digest);
    SRF_EXPECT_EQ(production.primary, std::string("ProfilePayloadWidthExceeded"));
}

// Formerly a pinned divergence: production had no producer for ProfileKindMismatch.
// It now reports the missing encoding family member, so both sides agree.
SRF_TEST(oracle, invalid_kind_is_reported_identically) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(2, 4);
    scenario.profile.allowed_kinds_mask = static_cast<std::uint16_t>(
        srf::kind_bit(srf::SegmentKind::Node) | srf::kind_bit(srf::SegmentKind::Endpoint));
    scenario.limits = generous_limits();
    srf::Segment segment{};
    segment.kind = srf::SegmentKind::Invalid;
    segment.id = srf::SegmentId{0x3001};
    segment.generation = srf::SegmentGeneration{1};
    scenario.segments = {segment};
    const OracleAnswer oracle = query_oracle(sandbox, render_scenario(scenario));
    const SequenceAnswer production = production_answer(scenario, scenario.segments);
    SRF_EXPECT(oracle.answered);
    // An unknown kind has no required encoding, which both sides report as
    // ProfileKindMismatch alongside the malformed kind itself.
    SRF_EXPECT_EQ(join(production.reasons, ','),
                  std::string("ProfileKindNotAllowed,ProfileKindMismatch,SegmentKindInvalid"));
    SRF_EXPECT_EQ(join(oracle.reasons, ','),
                  std::string("ProfileKindNotAllowed,ProfileKindMismatch,SegmentKindInvalid"));
    SRF_EXPECT_EQ(production.primary, std::string("ProfileKindNotAllowed"));
    SRF_EXPECT_EQ(oracle.primary, std::string("ProfileKindNotAllowed"));
    SRF_EXPECT_EQ(oracle.bytes, production.bytes);
    SRF_EXPECT_EQ(oracle.digest, production.digest);
}

SRF_TEST(oracle, divergence_zero_width_profile_is_profile_id_invalid_to_the_oracle) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(0, 4);  // the width the oracle reads as "no profile"
    scenario.limits = generous_limits();
    srf::Segment node = node_segment(1, true);
    node.payload = {std::byte{0x01}};
    scenario.segments = {node};
    const OracleAnswer oracle = query_oracle(sandbox, render_scenario(scenario));
    const SequenceAnswer production = production_answer(scenario, scenario.segments);
    SRF_EXPECT(oracle.answered);
    // The oracle reports ProfileIdInvalid and stops the profile phase.
    SRF_EXPECT(!oracle.valid);
    SRF_EXPECT_EQ(join(oracle.reasons, ','), std::string("ProfileIdInvalid"));
    SRF_EXPECT_EQ(oracle.primary, std::string("ProfileIdInvalid"));
    // Production validates the profile normally and rejects the payload width.
    SRF_EXPECT_EQ(join(production.reasons, ','), std::string("ProfilePayloadWidthExceeded"));
    SRF_EXPECT_EQ(production.primary, std::string("ProfilePayloadWidthExceeded"));
    // With width 0 neither side writes payload bytes, so the canonical bytes
    // still agree.
    SRF_EXPECT_EQ(oracle.bytes, production.bytes);
    SRF_EXPECT_EQ(oracle.digest, production.digest);
}

SRF_TEST(oracle, divergence_zero_profile_generation_is_never_checked_by_the_oracle) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(4, 4);
    scenario.profile.generation = srf::SegmentProfileGeneration{0};
    scenario.limits = generous_limits();
    scenario.segments = {node_segment(1, true)};
    const OracleAnswer oracle = query_oracle(sandbox, render_scenario(scenario));
    const SequenceAnswer production = production_answer(scenario, srf::canonicalize_segments(scenario.segments));
    SRF_EXPECT(oracle.answered);
    SRF_EXPECT(oracle.valid);
    SRF_EXPECT_EQ(join(oracle.reasons, ','), std::string(""));
    SRF_EXPECT_EQ(oracle.primary, std::string("Ok"));
    SRF_EXPECT(!production.valid);
    SRF_EXPECT_EQ(join(production.reasons, ','), std::string("ProfileGenerationInvalid"));
    SRF_EXPECT_EQ(production.primary, std::string("ProfileGenerationInvalid"));
    SRF_EXPECT_EQ(oracle.bytes, production.bytes);
    SRF_EXPECT_EQ(oracle.digest, production.digest);
}

SRF_TEST(oracle, divergence_adjacency_beyond_the_scanned_prefix) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(4, 8);
    scenario.limits = generous_limits();
    scenario.limits.max_segments_per_list = 1;  // only the first two segments are scanned
    scenario.segments = {node_segment(1, true), endpoint_segment(2), adjacency_segment(3)};
    const OracleAnswer oracle = query_oracle(sandbox, render_scenario(scenario));
    const SequenceAnswer production = production_answer(scenario, scenario.segments);
    SRF_EXPECT(oracle.answered);
    // Production scans every segment for topology evidence; the oracle scans the
    // first LIMITS maxseg + 1 only.
    SRF_EXPECT_EQ(join(production.reasons, ','),
                  std::string("TopologyEvidenceUnavailable,LimitMaxSegmentsPerList"));
    SRF_EXPECT_EQ(join(oracle.reasons, ','), std::string("LimitMaxSegmentsPerList"));
    SRF_EXPECT_EQ(production.primary, std::string("TopologyEvidenceUnavailable"));
    SRF_EXPECT_EQ(oracle.primary, std::string("LimitMaxSegmentsPerList"));
    // Both sides still encode the complete sequence.
    SRF_EXPECT_EQ(oracle.bytes, production.bytes);
    SRF_EXPECT_EQ(oracle.digest, production.digest);
}

// Formerly a pinned divergence: production's structural scan stopped one segment
// short of the documented bounded prefix. It now scans maxseg + 1 like the oracle.
SRF_TEST(oracle, structural_scan_covers_the_bounded_prefix_identically) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(4, 8);
    scenario.limits = generous_limits();
    scenario.limits.max_segments_per_list = 1;
    srf::Segment broken = endpoint_segment(2);
    broken.generation = srf::SegmentGeneration{0};
    scenario.segments = {node_segment(1, true), broken, binding_segment(3)};
    const OracleAnswer oracle = query_oracle(sandbox, render_scenario(scenario));
    const SequenceAnswer production = production_answer(scenario, scenario.segments);
    SRF_EXPECT(oracle.answered);
    // Both sides scan min(3, maxseg + 1) = 2 segments structurally and see the zero
    // generation at index 1; the depth violation is still reported as well.
    SRF_EXPECT_EQ(join(production.reasons, ','),
                  std::string("SegmentGenerationInvalid,LimitMaxSegmentsPerList"));
    SRF_EXPECT_EQ(join(oracle.reasons, ','),
                  std::string("SegmentGenerationInvalid,LimitMaxSegmentsPerList"));
    SRF_EXPECT_EQ(production.primary, std::string("SegmentGenerationInvalid"));
    SRF_EXPECT_EQ(oracle.primary, std::string("SegmentGenerationInvalid"));
    SRF_EXPECT_EQ(oracle.bytes, production.bytes);
    SRF_EXPECT_EQ(oracle.digest, production.digest);
}

SRF_TEST(oracle, divergence_profile_allowed_repeats_suppress_duplicates) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(4, 8);
    scenario.profile.allow_consecutive_repeats = true;  // the profile permits the repeat
    scenario.limits = generous_limits();
    const srf::Segment repeated = node_segment(1, true);
    scenario.segments = {repeated, repeated};
    const OracleAnswer oracle = query_oracle(sandbox, render_scenario(scenario));
    const SequenceAnswer production = production_answer(scenario, scenario.segments);
    SRF_EXPECT(oracle.answered);
    // The oracle reports the equal pair even though the profile permits repetition.
    SRF_EXPECT_EQ(join(oracle.reasons, ','), std::string("SegmentDuplicateConsecutive"));
    SRF_EXPECT(!oracle.valid);
    // Production treats the permitted repeat as legal and reports nothing.
    SRF_EXPECT_EQ(join(production.reasons, ','), std::string(""));
    SRF_EXPECT(production.valid);
    SRF_EXPECT_EQ(oracle.primary, std::string("SegmentDuplicateConsecutive"));
    SRF_EXPECT_EQ(production.primary, std::string("Ok"));
    SRF_EXPECT_EQ(oracle.bytes, production.bytes);
    SRF_EXPECT_EQ(oracle.digest, production.digest);
}

SRF_TEST(oracle, mixed_encoding_spelling_is_normalized_before_equality) {
    Sandbox sandbox;
    Scenario scenario{};
    scenario.profile = base_profile(4, 8);
    scenario.profile.allow_consecutive_repeats = false;
    scenario.profile.allow_nonconsecutive_repeats = false;
    scenario.limits = generous_limits();
    srf::Segment first = node_segment(1, true);
    first.encoding = srf::SegmentEncodingId::Invalid;  // unset
    srf::Segment second = first;
    second.encoding = srf::SegmentEncodingId::AbstractNodeV1;  // the required encoding
    scenario.segments = {first, second};

    const OracleAnswer oracle = query_oracle(sandbox, render_scenario(scenario));
    const std::vector<srf::Segment> canonical = srf::canonicalize_segments(scenario.segments);
    SRF_EXPECT(oracle.answered);
    // The oracle normalizes before it checks anything and sees the duplicate.
    SRF_EXPECT_EQ(join(oracle.reasons, ','),
                  std::string("ProfileRepeatNotPermitted,SegmentDuplicateConsecutive"));
    SRF_EXPECT(!oracle.valid);
    // Canonicalizing first gives production the same answer.
    SRF_EXPECT_EQ(join(reason_names(validate_sequence(scenario, canonical)), ','),
                  join(oracle.reasons, ','));
    // The two spellings canonicalize to identical bytes...
    SRF_EXPECT_EQ(hex_encode(srf::canonical_segment_bytes(canonical[0], 4)),
                  hex_encode(srf::canonical_segment_bytes(canonical[1], 4)));
    // ... and the raw sequence is rejected exactly as the oracle rejects it:
    // segment_semantic_equal resolves an unset encoding to the encoding the kind
    // requires before comparing, so the spelling cannot hide a duplicate. This is
    // the regression guard for the store-level gap described in the file header.
    const srf::ValidationResult raw = validate_sequence(scenario, scenario.segments);
    SRF_EXPECT(!raw.ok());
    SRF_EXPECT_EQ(join(reason_names(raw), ','), join(oracle.reasons, ','));
    SRF_EXPECT_EQ(oracle.bytes, hex_encode(srf::canonical_sequence_bytes(canonical, 4)));
    SRF_EXPECT_EQ(oracle.digest, srf::sequence_digest(canonical, 4).hex());
}
