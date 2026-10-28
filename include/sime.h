// Modified by the weave-ime project (2026): search constants EnglishPenalty, EnglishPenaltyShort, BeamSize, and the scoped-penalty constants are macro-backed (SIME_WEAVE_*); adds SIME_WEAVE_CASE_FOLD and the SIME_WEAVE_LITERAL_EDGE literal-letters edge (with SIME_WEAVE_LITERAL_PER_LETTER and SIME_WEAVE_LITERAL_NONPINYIN); adds SIME_WEAVE_SPACE_BOUNDARY and SIME_WEAVE_RAW_CNT; weave defaults PENALTY_RULE=1, EN_PENALTY_MIXED=0.0, EN_SHORT_EDGE_LEN=2, EN_PENALTY_SHORT=4.0 (Phase 4.5 Task 2; were 3 and 8.0), CASE_FOLD=1, RAW_CNT=1; adds Sime::TokenizeContext (additive, no macro); adds the SIME_WEAVE_OVERLAY lexicon overlay API (LoadOverlay, layer switches, display casing, dedup by key; default 1 since Phase 4.5 Task 8c, was 0); adds the candidate-contract provenance (DecodeResult::segments and ::basis, additive, no macro); adds SIME_WEAVE_INCREMENTAL incremental decoding (snapshots per prefix, runtime switch; default 1 since Phase 4.5 Task 8c, was 0) (see NOTICE.weave.md).
#pragma once

#include "common.h"
#include "score.h"
#include "state.h"
#include "dict.h"
#include "user.h"
#include "overlay.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// weave-ime tunables. Since weave-ime Phase 2 (Task 8) six defaults differ
// from upstream: SIME_WEAVE_PENALTY_RULE 1 (upstream 0),
// SIME_WEAVE_EN_PENALTY_MIXED 0.0 (8.0), SIME_WEAVE_EN_SHORT_EDGE_LEN 2 (0; 3 until Phase 4.5 Task 2),
// SIME_WEAVE_EN_PENALTY_SHORT 4.0 (8.0; likewise),
// SIME_WEAVE_CASE_FOLD 1 (0) and SIME_WEAVE_RAW_CNT 1 (0); state.h also
// flips SIME_WEAVE_BEAM_EXACT to 1 (0). CASE_FOLD=1 is meant for a dictionary
// with lowercase English keys (benchmarks/engines/sime/rebuild_dict.sh
// --transform lowercase-en in weave-ime); with upstream's cased save/sime.dict
// a lowercase span still finds only lowercase keys and a span typed with
// capitals finds its exact casing as upstream does (see NOTICE.weave.md).
// Every other macro
// defaults to its upstream value. Defining all of them to the upstream
// values (listed in NOTICE.weave.md) reproduces upstream exactly.
#ifndef SIME_WEAVE_EN_PENALTY
#define SIME_WEAVE_EN_PENALTY 2.5
#endif
#ifndef SIME_WEAVE_EN_PENALTY_SHORT
#define SIME_WEAVE_EN_PENALTY_SHORT 4.0  // upstream: 8.0
#endif
#ifndef SIME_WEAVE_PENALTY_RULE
#define SIME_WEAVE_PENALTY_RULE 1  // upstream: 0
#endif
#ifndef SIME_WEAVE_EN_PENALTY_MIXED
#define SIME_WEAVE_EN_PENALTY_MIXED 0.0  // upstream-equivalent: 8.0
#endif
#ifndef SIME_WEAVE_EN_SHORT_EDGE_LEN
#define SIME_WEAVE_EN_SHORT_EDGE_LEN 2  // upstream-equivalent: 0
#endif
// SIME_WEAVE_CASE_FOLD=1: English DAT lookups use the lowercased typed span
// (pair with a lowercase-English-key dictionary), plus the span as typed
// when it has capitals (a cased dictionary's capitalised keys); a typed
// uppercase letter keeps only the items with exactly that casing, when there
// are any. Units (DecodeResult::units) match letters ignoring case and keep
// the typed casing.
#ifndef SIME_WEAVE_CASE_FOLD
#define SIME_WEAVE_CASE_FOLD 1  // upstream: 0
#endif
// SIME_WEAVE_LITERAL_EDGE=1: InitNet adds one literal edge for every span of
// at least SIME_WEAVE_LITERAL_MIN_LEN ASCII letters that starts at a column
// where a dictionary edge starts. Its text is the typed letters; it is scored
// as the LM's unknown token (back-off cost, context reset) plus
// SIME_WEAVE_LITERAL_PENALTY.
#ifndef SIME_WEAVE_LITERAL_EDGE
#define SIME_WEAVE_LITERAL_EDGE 0
#endif
#ifndef SIME_WEAVE_LITERAL_MIN_LEN
#define SIME_WEAVE_LITERAL_MIN_LEN 3
#endif
#ifndef SIME_WEAVE_LITERAL_PENALTY
#define SIME_WEAVE_LITERAL_PENALTY 12.0
#endif
// Literal edge cost += SIME_WEAVE_LITERAL_PER_LETTER x (letters in the span).
#ifndef SIME_WEAVE_LITERAL_PER_LETTER
#define SIME_WEAVE_LITERAL_PER_LETTER 0.0
#endif
// SIME_WEAVE_LITERAL_NONPINYIN=1: no literal edge for a span that segments
// fully into complete pinyin syllables (Dict::IsKnownPinyin; a bare initial
// is not one).
#ifndef SIME_WEAVE_LITERAL_NONPINYIN
#define SIME_WEAVE_LITERAL_NONPINYIN 0
#endif
// SIME_WEAVE_SPACE_BOUNDARY=1: DecodeSentence treats a typed space as a hard
// boundary. No lattice edge spans it and it adds no output text or LM token;
// the LM state passes through it unchanged, and the English penalty rule is
// evaluated per space-delimited word. 0 = upstream (space
// becomes the U+2581 byte triple and is searched like any other input).
#ifndef SIME_WEAVE_SPACE_BOUNDARY
#define SIME_WEAVE_SPACE_BOUNDARY 0
#endif
// SIME_WEAVE_RAW_CNT=1: DecodeSentence reports DecodeResult::cnt in raw-input
// bytes consumed in both layers (Layer 2: the raw offset where the edge ends,
// the whole input when the edge reaches the end of the normalised input).
// 0 = upstream (Layer 1 input.size(), Layer 2 normalised edge.end).
// DecodeNumSentence and DecodeStr are unchanged (their cnt is already raw).
#ifndef SIME_WEAVE_RAW_CNT
#define SIME_WEAVE_RAW_CNT 1  // upstream: 0
#endif
#ifndef SIME_WEAVE_BEAM_SIZE
#define SIME_WEAVE_BEAM_SIZE 20
#endif
// SIME_WEAVE_INCREMENTAL=1: DecodeSentence keeps the lattice and beam of earlier calls (one snapshot
// per normalised prefix of the composition) and, when the input extends or shortens the last one,
// rebuilds the edges, resumes Process at the first column whose edges changed and rebuilds the
// output; the result equals a full re-decode exactly (weave-ime Docs/plans/phase-4.5-incremental.md).
// SetIncremental(false) switches it off at runtime. 0: every call decodes from scratch. 1 is the
// default since weave-ime Phase 4.5 Task 8c (the owner's D7, 2026-10-27; was 0).
#ifndef SIME_WEAVE_INCREMENTAL
#define SIME_WEAVE_INCREMENTAL 1
#endif

namespace sime {

class GruReranker;
class Cutter;
#if SIME_WEAVE_OVERLAY
enum class DisplayCasing { kCanonical = 0, kAsTyped = 1 };
#endif

// weave-ime candidate contract (Docs/plans/phase-4.5-candidate-contract.md in weave-ime;
// additive, no macro: it changes no decoding). DecodeSentence fills DecodeResult::segments and
// ::basis; the other decode entry points leave segments empty.
enum class SegmentKind : std::uint8_t { kPinyin = 0, kEnglish = 1, kLiteral = 2 };
// kSentence: a Layer 1 whole-input candidate (beam score, with the decoder context).
// kWordAlt: a Layer 2 single-word alternative (context-free unigram score). Scores compare only
// within one basis.
enum class ScoreBasis : std::uint8_t { kSentence = 0, kWordAlt = 1 };
// Source-layer bits of a segment: L0 (the Sime dictionary) and the overlay's word layers
// (bit 1 + LexLayer slot: L1 ESDB, L3 term pack, L4 user; L2 is a cost, never a source).
constexpr unsigned kSourceBase = 1u << 0;
constexpr unsigned kSourceL1 = 1u << 1;
constexpr unsigned kSourceL3 = 1u << 3;
constexpr unsigned kSourceL4 = 1u << 4;

struct Segment {
    // [start, end) in raw input bytes. The first segment starts at 0 and each next one where the
    // previous ended; a separator (apostrophe, typed space) or a byte NormalizeInput drops belongs
    // to the segment before it, and the last segment ends at the candidate's raw end.
    std::size_t start = 0;
    std::size_t end = 0;
    std::string text;            // this piece of DecodeResult::text (active display casing)
    std::string typed_text;      // the piece with English letters in the typed casing
    std::string canonical_text;  // the dictionary / layer spelling (as under kCanonical display)
    SegmentKind kind = SegmentKind::kPinyin;
    unsigned layer = 0;          // the one kSource* bit that supplied the word; 0 for a literal
    unsigned sources = 0;        // every kSource* bit that contributed, after the overlay's merge
    TokenID token = NotToken;    // the L0 token; NotToken for overlay words and literals
    std::string units;           // this piece of DecodeResult::units ("" when it adds none)
    bool expansion = false;      // an abbreviation / tail-completion edge
};

#if SIME_WEAVE_INCREMENTAL
// How DecodeSentence answered (SIME_WEAVE_INCREMENTAL). kFull: a full decode with no reused state
// (first call, generation change, an edit, incremental switched off); kSame: the snapshot of the same
// normalised input was reused; kResume: Process resumed from a snapshot of a prefix; kFallback: a
// consistency check failed and the call was decoded in full.
enum class DecodeKind : std::uint8_t { kFull = 0, kSame = 1, kResume = 2, kFallback = 3 };
struct DecodeInfo {
    DecodeKind kind = DecodeKind::kFull;
    bool warm = false;            // kSame or kResume: decoder state was reused (contract cache_state)
    std::size_t resume_col = 0;   // kResume: first column whose beam was recomputed; kSame: `columns`
    std::size_t columns = 0;      // lattice columns (normalised input length + 2); 0 without a lattice
};
struct IncrementalStats {
    std::uint64_t generation = 0;   // bumped by every change that drops the snapshots
    std::size_t decodes = 0;        // DecodeSentence calls
    std::size_t full = 0, same = 0, resumed = 0, fallbacks = 0;
    std::size_t self_checks = 0;    // warm calls compared with a full decode (SetIncrementalSelfCheck)
    std::size_t columns = 0;        // over resumed calls: lattice columns
    std::size_t columns_redone = 0; // over resumed calls: columns whose beam was recomputed
    std::size_t snapshots = 0;      // snapshots kept now
    // SetIncrementalDiagnostics(true): changed columns before the old end of resumed calls, by what
    // changed there (one column can count under several), and recomputed columns at the column cap.
    std::size_t diff_append = 0;    // new edges reaching past the old end
    std::size_t diff_old_end = 0;   // an edge ending at the old end vanished or changed
    std::size_t diff_penalty = 0;   // an edge kept its identity but its penalty changed
    std::size_t diff_order = 0;     // the edges both lists share are in another order (pruning)
    std::size_t diff_other = 0;     // an edge ending before the old end appeared or vanished
    std::size_t capped_redone = 0;  // recomputed columns holding SIME_WEAVE_COLUMN_BEAM_SIZE states
};
// Test-only faults (SetIncrementalFaultForTest), each for the next call that would reuse a snapshot.
enum class IncrementalFault : std::uint8_t { kNone = 0, kBreakSnapshot = 1, kPerturbResult = 2 };
#endif

struct DecodeResult {
    std::string text;        // UTF-8 display text (▁ prefix stripped)
    std::string units;       // segmented pinyin (e.g. "ni'hao")
    std::vector<TokenID> tokens;  // token IDs for LM context
    float_t score = 0.0;    // larger is better (negative log probability negated)
    std::size_t cnt = 0;     // bytes of input consumed
    std::vector<Segment> segments;               // weave-ime: DecodeSentence only
    ScoreBasis basis = ScoreBasis::kSentence;    // weave-ime: DecodeSentence only
};

class Sime {
public:
    Sime() = default;
    Sime(const std::filesystem::path& dict_path,
         const std::filesystem::path& model_path);
    ~Sime();

    bool Ready() const { return ready_; }
    bool GruReady() const;
    int ContextSize() const { return scorer_.Num() - 1; }

    // Cache trim hooks for the long-running daemon use case (Android
    // service / Linux fcitx5). Called periodically inside decode entry
    // points to bound the trie sep_cache content size; ResetCaches()
    // is for hard release on memory pressure (e.g. Android
    // onTrimMemory).
    void ResetCaches() const;

    void SetUserSentenceEnabled(bool enabled);
    bool UserSentenceEnabled() const { return user_sentence_enabled_; }
    // weave (SIME_WEAVE_INCREMENTAL): handing out the writable store bumps the generation.
    UserSentence& MutableUserSentence();
    const UserSentence& GetUserSentence() const { return user_sentence_; }
    // Loads from disk. Returns false on missing/corrupt/vocab-mismatch
    // files; the in-memory state is left empty in those cases. The
    // caller may delete the stale file when this returns false.
    bool LoadUserSentence(const std::filesystem::path& path);
    bool SaveUserSentence(const std::filesystem::path& path) const;
    // Opaque tag identifying the LM vocabulary the engine is currently
    // bound to. Stored alongside user sentences so they get dropped
    // when the LM is regenerated and TokenIDs no longer mean the same
    // thing. Empty until/unless an LM was successfully loaded.
    const std::string& VocabSignature() const { return vocab_sig_; }
    void LearnUserSentence(const std::vector<TokenID>& context,
                           const std::vector<TokenID>& sentence);
    // UTF-8 text for a single token id, sourced from the dict's
    // mmap'd token table. Empty for NotToken or out-of-range ids.
    std::string TokenText(TokenID id) const;

    // Decode
    std::vector<DecodeResult> DecodeStr(std::string_view input,
                                        std::size_t num = 5) const;
    std::vector<DecodeResult> DecodeSentence(std::string_view input,
                                             std::size_t extra = 0) const;
    std::vector<DecodeResult> DecodeSentence(
        std::string_view input,
        const std::vector<TokenID>& context,
        std::size_t extra = 0) const;
    // weave-ime: token IDs for committed UTF-8 text, for DecodeSentence's
    // `context`. Runs Cutter (the dictionary's tokens, scored by the LM) over
    // the last `max_code_points` code points of `utf8`; a piece with no
    // dictionary token is NotToken. Empty text or window gives an empty vector.
    std::vector<TokenID> TokenizeContext(std::string_view utf8,
                                         std::size_t max_code_points) const;
    // Prediction: given confirmed token IDs as context, suggest next words.
    // When `en` is true, only English tokens are returned (for the English
    // IME's prediction slot); Chinese tokens are filtered out.
    std::vector<DecodeResult> NextTokens(
        const std::vector<TokenID>& context,
        std::size_t num = 10,
        bool en = false) const;

    // Prefix completion: return tokens starting with `prefix`, sorted by
    // unigram score. Default searches both English and pinyin DATs (mixed
    // mode). When `en` is true, only the English DAT is searched.
    std::vector<DecodeResult> GetTokens(
        std::string_view prefix,
        std::size_t num = 10,
        bool en = false) const;

    // Num-key decode (T9/nine-key).
    // `start` is the confirmed prefix (letters, possibly with `'`
    // separators). Supports both pinyin and English prefixes.
    std::vector<DecodeResult> DecodeNumStr(
        std::string_view nums,
        std::string_view start = {},
        std::size_t num = 18) const;
    // Layer 1: full sentence N-best covering start + nums. Returns
    // 1 + `extra` sentences (top sentence is always included; `extra`
    // additional alternatives are appended).
    // Layer 2: word/char alternatives anchored at the first digit
    // column. Both layers are scored against the
    // LM context produced by the prefix `start`.
    std::vector<DecodeResult> DecodeNumSentence(
        std::string_view nums,
        std::string_view start = {},
        std::size_t extra = 0) const;
    std::vector<DecodeResult> DecodeNumSentence(
        std::string_view nums,
        std::string_view start,
        const std::vector<TokenID>& context,
        std::size_t extra = 0) const;
#if SIME_WEAVE_OVERLAY
    // weave lexicon overlay (weave-ime Docs/plans/phase-4.5-lexicon.md). Loads the layer files
    // in `config` (replacing any loaded overlay); false (with *error) leaves no overlay.
    // DecodeSentence and DecodeStr then add the overlay's words as English edges at their
    // bounded cost; an overlay word is a history break for the LM. The T9 path, GetTokens and
    // NextTokens do not use the overlay.
    bool LoadOverlay(const OverlayConfig& config, std::string* error = nullptr);
    void ClearOverlay();
    // Runtime off switch per layer; switching off equals loading without that layer.
    bool SetOverlayLayerEnabled(LexLayer layer, bool enabled);
    const OverlayStats& GetOverlayStats() const { return overlay_.Stats(); }
    static bool IsOverlayToken(TokenID id) { return Overlay::IsToken(id); }
    // Display casing of English words (D5). kCanonical (default) is today's output for L0
    // words (a word the term pack or user dictionary also has shows that spelling; L1 never re-cases); kAsTyped shows
    // every English word with the typed letters' casing.
    void SetDisplayCasing(DisplayCasing casing);
    // De-duplicate candidate lists by the displayed text's key (ASCII lowercase, spaces
    // removed) instead of exact text (default off).
    void SetDedupByKey(bool on);
#endif
#if SIME_WEAVE_INCREMENTAL
    // weave incremental decoding (weave-ime Docs/plans/phase-4.5-incremental.md). On by default in a
    // build with the macro; off makes every DecodeSentence a full decode through the unchanged path.
    // Switching, ResetIncremental and every change of the overlay, display casing, dedup by key, the
    // user sentences or the decoder context bump the generation, which drops every snapshot.
    void SetIncremental(bool on);
    bool Incremental() const;
    void ResetIncremental();
    // Each warm call is also decoded in full and compared; a difference returns the full result and
    // counts a fallback (default off: it doubles the work).
    void SetIncrementalSelfCheck(bool on);
    // Classify the changed columns of resumed calls (IncrementalStats::diff_*; default off).
    void SetIncrementalDiagnostics(bool on);
    IncrementalStats GetIncrementalStats() const;
    DecodeInfo LastDecodeInfo() const;
    // A full decode that neither reads nor changes the snapshots (the differential's reference).
    std::vector<DecodeResult> DecodeSentenceFull(std::string_view input,
                                                 const std::vector<TokenID>& context,
                                                 std::size_t extra = 0) const;
    void SetIncrementalFaultForTest(IncrementalFault fault);
#endif
private:
    // Lattice types
    struct Link {
        std::size_t start = 0;
        std::size_t end = 0;
        TokenID id = 0;
        const char* pieces = nullptr;  // piece path, e.g. "ni'hao"
        float_t penalty = 0;           // syllable mismatch penalty
        bool expansion = false;        // tail-expansion edge (lower priority in PruneNode)
        bool english = false;          // edge from English DAT (lower priority than full pinyin)
    };

    struct Node {
        std::vector<Link> es;
        NetStates states;
    };
    // The edge list of a lattice column (DecodeSentence's output builder reads columns through it).
    using ColumnEdges = std::function<const std::vector<Link>&(std::size_t)>;
#if SIME_WEAVE_INCREMENTAL
    // The lattice and beam of one normalised input. Columns are shared with the snapshot it was
    // resumed from where they did not change (edges before the resume column, beams up to it).
    struct Snapshot {
        std::string norm;
        std::uint64_t generation = 0;
        std::vector<std::shared_ptr<const std::vector<Link>>> edges;  // norm.size() + 2
        std::vector<std::shared_ptr<NetStates>> states;               // norm.size() + 2
    };
    static constexpr std::size_t kMaxSnapshots = 128;
#endif

    // Search parameters
    static constexpr std::size_t NodeSize = 60;
    static constexpr std::size_t BeamSize = SIME_WEAVE_BEAM_SIZE;
    static constexpr float_t DistancePenalty = 1.8;
    // Flat per-edge penalty for expansion edges (abbreviations / tail
    // completions). A single value — not multiplied by syllable mismatch
    // count — so longer abbreviations aren't punished more than shorter
    // ones (mirrors English exact). Short input (≤4) gets a smaller
    // penalty so n-initial abbreviations like 87 → 他说 can beat short
    // English exact (e.g. up); long input gets a larger penalty so
    // abbreviation expansions don't crowd out the natural full-pinyin
    // path in long sentences.
    static constexpr float_t ExpansionPenalty = 5.5;
    static constexpr float_t ExpansionPenaltyShort = 4.0;
    // Flat penalty for English edges so Chinese full-pinyin matches rank
    // higher in beam search and Layer 2, while common English words
    // (iphone, hello) still surface when there's no strong Chinese rival.
    static constexpr float_t EnglishPenalty = SIME_WEAVE_EN_PENALTY;
    // Short-input English penalty: short inputs (≤4 letters/digits) where
    // an English exact word coincidentally spell-matches the input
    // (e.g. "nm" / "us" / "be") need a stronger nudge so CN 简拼
    // candidates can win — the LM's <eos>|english=0 transition gives
    // English an unfair tail-cost edge in beam scoring otherwise.
    static constexpr float_t EnglishPenaltyShort = SIME_WEAVE_EN_PENALTY_SHORT;
    // weave scoped rule (SIME_WEAVE_PENALTY_RULE=1): penalty for an English
    // edge inside longer input, and the max edge length (letters) that still
    // pays EnglishPenaltyShort there.
    static constexpr float_t EnglishPenaltyMixed = SIME_WEAVE_EN_PENALTY_MIXED;
    static constexpr std::size_t EnglishShortEdgeLen = SIME_WEAVE_EN_SHORT_EDGE_LEN;
#if SIME_WEAVE_LITERAL_EDGE
    // weave literal edge: the token ID it carries is outside the dictionary
    // and the LM, so Scorer::ScoreMove scores it as an unknown token (back-off
    // weights + root probability) and returns the empty context.
    static constexpr TokenID LiteralToken = 0xFFFFFFFEu;
    static constexpr float_t LiteralPenalty = SIME_WEAVE_LITERAL_PENALTY;
    static constexpr std::size_t LiteralMinLen = SIME_WEAVE_LITERAL_MIN_LEN;
    static constexpr float_t LiteralPerLetter = SIME_WEAVE_LITERAL_PER_LETTER;
#endif
    // weave typed-space edge (SIME_WEAVE_SPACE_BOUNDARY=1): carries the LM state
    // through unchanged (Process), produces no text and no token.
    static constexpr TokenID SpaceToken = 0xFFFFFFFDu;
    static constexpr bool IsNoToken(TokenID id) {
        return id == NotToken || id == SpaceToken;
    }

    // Lattice building
    void InitNet(std::string_view input,
                    std::vector<Node>& net,
                    bool expansion = true
#if SIME_WEAVE_OVERLAY
                    , std::vector<std::vector<Link>>* overlay_edges = nullptr
#endif
                    ) const;
#if SIME_WEAVE_OVERLAY
    // Gives the overlay edges InitNet collected their English penalties, filters (exact over
    // completion) and prunes them among themselves, and appends them after net's own edges.
    void AppendOverlayEdges(std::vector<Node>& net,
                            std::vector<std::vector<Link>>& overlay_edges,
                            std::string_view input) const;
    // LM step of an edge: the overlay's band cost and a history break (root state) for an
    // overlay token, the scorer's move otherwise.
    float_t StepCost(Scorer::Pos pos, TokenID id, Scorer::Pos& next) const;
    std::string DedupKey(const std::string& text) const;
#endif
    static void ComputeEdgePenalties(std::vector<Node>& net,
                                     std::string_view input);
    void PruneNode(std::vector<Link>& edges,
                   std::unordered_map<TokenID, float_t>* score_cache = nullptr) const;

    // Beam search
    State InitialState(const std::vector<TokenID>& context = {}) const;
    void Process(std::vector<Node>& net) const;
    // One step of Process: state `value` takes edge `word` into `target` (word.end's column).
    void Relax(const State& value, const Link& word, NetStates& target) const;

    // DecodeSentence's parts: the lattice edges of the normalised input (InitNet, penalties, pruning,
    // overlay), the full decode, and the candidate lists from a processed lattice.
    void BuildSentenceNet(std::string_view lower, std::vector<Node>& net) const;
    std::vector<DecodeResult> DecodeSentenceUnlocked(std::string_view input,
                                                     const std::vector<TokenID>& context,
                                                     std::size_t extra) const;
    std::vector<DecodeResult> SentenceResults(std::string_view input, std::string_view lower,
                                              const std::vector<std::size_t>& raw_off,
                                              const std::vector<State>& tail,
                                              std::size_t net_size, const ColumnEdges& column,
                                              std::size_t extra) const;
#if SIME_WEAVE_INCREMENTAL
    std::vector<DecodeResult> DecodeSession(std::string_view input,
                                            const std::vector<TokenID>& context,
                                            std::size_t extra) const;
    std::unique_ptr<Snapshot> FullSnapshot(const std::string& lower,
                                           const std::vector<TokenID>& context) const;
    // nullptr when a consistency check fails.
    std::unique_ptr<Snapshot> ResumeSnapshot(const Snapshot& base, const std::string& lower,
                                             std::size_t& resume_col) const;
    // Process over a snapshot from column k: columns before k replay their edges that end after k.
    void ProcessFrom(Snapshot& snap, std::size_t k) const;
    void ClassifyColumn(const std::vector<Link>& before, const std::vector<Link>& after,
                        std::size_t old_end) const;
    void BumpGeneration() const;        // decode_mutex_ held
    void BumpGenerationLocked() const;  // takes decode_mutex_
#endif
    static std::vector<Link> Backtrace(const State& tail_state,
                                       std::size_t end);

    // `input` is the normalized input the lattice was built from; literal
    // edges (SIME_WEAVE_LITERAL_EDGE) take their text from it.
    std::u32string ToText(const Link& n, std::string_view input = {}) const;
    std::string ExtractText(const std::vector<Link>& path,
                            std::string_view input = {}) const;
    static std::string ExtractUnits(const std::vector<Link>& path,
                                    std::string_view input);
    static std::string AbbreviatePieces(const char* full_pieces,
                                        std::string_view input);
    std::vector<TokenID> ExtractTokens(const std::vector<Link>& path) const;
    static std::string TextFromU32(std::u32string& u32);
    // weave-ime candidate contract: one Segment per token link of `path` (separators join the
    // segment before them). `raw_off` maps normalised to raw input offsets (NormalizeInput).
    void FillSegment(const Link& link, std::string_view input, Segment& seg) const;
    std::vector<Segment> ExtractSegments(const std::vector<Link>& path, std::string_view input,
                                         const std::vector<std::size_t>& raw_off,
                                         std::size_t raw_size) const;

    // Num-key lattice
    void InitNumNet(std::string_view start,
                     std::string_view nums,
                     std::vector<Node>& net,
                     bool expansion = true) const;

    // Periodic soft trim of trie sep_cache, called from decode entries.
    // Drops cached sep lists every kSepCacheTrimInterval decodes so the
    // daemon's memory footprint stays bounded. Soft (no allocator
    // churn): ~free in latency.
    void MaybeTrimCaches() const;
    static constexpr std::size_t kSepCacheTrimInterval = 5000;
    mutable std::size_t decode_count_ = 0;
    // Dict::scratch_ and the trie separator caches are intentionally reused
    // between calls. Serialize public decode/cache entry points so const
    // callers may safely share one Sime instance across threads.
    mutable std::mutex decode_mutex_;

    // Resources
    Dict dict_;
    Scorer scorer_;
    UserSentence user_sentence_;
    std::string vocab_sig_;
    bool user_sentence_enabled_ = false;
    bool ready_ = false;
    std::unique_ptr<GruReranker> gru_;
#if SIME_WEAVE_OVERLAY
    Overlay overlay_;
    DisplayCasing casing_ = DisplayCasing::kCanonical;
    bool dedup_by_key_ = false;
#endif
    // TokenizeContext's Cutter, built on first use.
    mutable std::once_flag cutter_once_;
    mutable std::unique_ptr<Cutter> cutter_;
#if SIME_WEAVE_INCREMENTAL
    // The composition's snapshots, each one's input a prefix of the next (decode_mutex_ guards them).
    mutable std::vector<std::unique_ptr<Snapshot>> chain_;
    mutable std::vector<TokenID> inc_context_;
    mutable IncrementalStats inc_stats_;
    mutable DecodeInfo last_info_;
    mutable IncrementalFault inc_fault_ = IncrementalFault::kNone;
    bool inc_on_ = true;
    bool inc_self_check_ = false;
    bool inc_diag_ = false;
#endif
};

} // namespace sime
