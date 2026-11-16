// Modified by the weave-ime project (2026): search constants EnglishPenalty, EnglishPenaltyShort, BeamSize, and the scoped-penalty constants are macro-backed (SIME_WEAVE_*); adds SIME_WEAVE_CASE_FOLD and the SIME_WEAVE_LITERAL_EDGE literal-letters edge (with SIME_WEAVE_LITERAL_PER_LETTER and SIME_WEAVE_LITERAL_NONPINYIN); adds SIME_WEAVE_SPACE_BOUNDARY and SIME_WEAVE_RAW_CNT; weave defaults PENALTY_RULE=1, EN_PENALTY_MIXED=0.0, EN_SHORT_EDGE_LEN=2, EN_PENALTY_SHORT=4.0 (Phase 4.5 Task 2; were 3 and 8.0), CASE_FOLD=1, RAW_CNT=1; adds Sime::TokenizeContext (additive, no macro); adds the SIME_WEAVE_OVERLAY lexicon overlay API (LoadOverlay, layer switches, display casing, dedup by key; default 1 since Phase 4.5 Task 8c, was 0); adds the candidate-contract provenance (DecodeResult::segments and ::basis, additive, no macro); adds SIME_WEAVE_INCREMENTAL incremental decoding (snapshots per prefix, runtime switch; default 1 since Phase 4.5 Task 8c, was 0); adds the SIME_WEAVE_SWAP_EDGE swapped-letter edges (default 1 since Phase 5 unfreeze Task 4, was 0; no edge until SetSwapCost gets a cost); adds the SIME_WEAVE_READING_PRIOR rare-reading penalty table (default 1 since Task 4, was 0; nothing changes until LoadReadingPrior gets a table); adds the SIME_WEAVE_RIGHT_CONTEXT Sime::TextCost (default 1 since weave-ime Phase 5 unfreeze 2, was 0; owner feedback 7); adds the SIME_WEAVE_FUZZY front/back-nasal fuzzy pinyin edges (default 1 since unfreeze 2, was 0; no edge until SetFuzzyCost gets a cost; Segment::correction under it); adds the SIME_WEAVE_DUP_EDGE doubled-letter edges (default 1 on the p5u3 candidate (local weave, 2026-11-15; was 0); no edge until SetDupCost gets a cost; Segment::correction 3); adds the SIME_WEAVE_UE_ALIAS lue/nue alias edges (default 1 on the p5u3 candidate (local weave, 2026-11-15; was 0); no edge until SetUeAliasCost gets a cost; Segment::correction 4); adds the SIME_WEAVE_NEIGHBOUR_EDGE QWERTY-neighbour substitution edges (experiment, local branch weave-nb, default 0; no edge until SetNeighbourCost gets a cost; Segment::correction 5); adds the SIME_WEAVE_EN_TYPO English edit-1 spelling edges (experiment, local branch weave-nb, default 0; no edge until SetEnTypoCost gets a cost; Segment::correction 6) (see NOTICE.weave.md).
#pragma once

#include "common.h"
#include "score.h"
#include "state.h"
#include "dict.h"
#include "user.h"
#include "overlay.h"
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
// SIME_WEAVE_SWAP_EDGE=1 (local branch weave-unfreeze, weave-ime Phase 5 unfreeze Task 2): the sentence decoder
// also looks pinyin words up with two adjacent typed letters swapped (dina reaches dian). Each match is an extra edge
// over the typed span, at a fixed cost, added after the exact edges are final, so it never replaces or filters one.
// English lookups are not touched. Doubled letters (diaan) are not corrected. The cost and the shortest edge are
// runtime (SetSwapCost), and off until set. Default 1 since weave-ime Phase 5 unfreeze Task 4 (was 0): compiled in, and
// no edge is added until SetSwapCost gets a cost of 0 or more.
#ifndef SIME_WEAVE_SWAP_EDGE
#define SIME_WEAVE_SWAP_EDGE 1
#endif
// SIME_WEAVE_READING_PRIOR=1 (experiment, local branch weave-reading-prior, weave-ime Phase 5 F12): with a table
// from LoadReadingPrior, a single-character pinyin edge on a listed (character, reading) pays that penalty on top of
// its other penalties. The n-gram model scores a token whatever its reading, so a rare reading of a common character
// otherwise costs the same as the common one. Multi-character words are never touched. Default 1 since weave-ime Phase 5 unfreeze Task 4 (was 0): compiled in, and
// nothing changes until LoadReadingPrior gets a table.
#ifndef SIME_WEAVE_READING_PRIOR
#define SIME_WEAVE_READING_PRIOR 1
#endif
// SIME_WEAVE_RIGHT_CONTEXT=1 (weave-ime Phase 5 owner feedback 7, insertion points): Sime::TextCost, the
// language model's cost of a piece of text under TokenizeContext's Cutter, so a host can compare how well each
// candidate runs into the text right of the caret. It changes no decoding: nothing calls it but the host. Default 1 since
// weave-ime Phase 5 unfreeze 2 (2026-11-13; was 0).
#ifndef SIME_WEAVE_RIGHT_CONTEXT
#define SIME_WEAVE_RIGHT_CONTEXT 1
#endif
// SIME_WEAVE_FUZZY=1 (weave-ime Phase 5 WP4.3): front/back-nasal fuzzy pinyin.
// The sentence decoder also looks pinyin words up with a syllable's final read as its nasal partner: typed an/en/in
// reaches stored ang/eng/ing (a missing g), and typed ang/eng/ing reaches stored an/en/in (an extra g, only where the
// stored syllable ends there). Each match is an extra edge over the typed span, priced per pair and per flip, added
// after the exact and swap edges are final, so it never replaces or filters one; a fuzzy edge never also swaps letters
// and never abbreviates. Every pair is off until SetFuzzyCost gives it a cost of 0 or more. A fuzzy edge carries its
// kind to Segment::correction. Requires SIME_WEAVE_SWAP_EDGE (shares its work counts). Default 1 since weave-ime
// Phase 5 unfreeze 2 (2026-11-13; was 0): compiled in, and no edge is added until SetFuzzyCost gives a pair a cost.
#ifndef SIME_WEAVE_FUZZY
#define SIME_WEAVE_FUZZY 1
#endif
#if SIME_WEAVE_FUZZY && !SIME_WEAVE_SWAP_EDGE
#error "SIME_WEAVE_FUZZY needs SIME_WEAVE_SWAP_EDGE"
#endif
// SIME_WEAVE_DUP_EDGE=1 (experiment, local branch weave-dup, weave-ime Phase 5 owner feedback of 2026-11-15): doubled
// letters. The sentence decoder also looks pinyin words up with one copy of two equal adjacent typed letters skipped
// (wooxiangchifan reaches woxiang, 我想). Each match is an extra edge over the typed span, both copies included, at a
// runtime cost (SetDupCost) plus the word's reading-table line, added after the exact, overlay, swap and fuzzy edges are
// final, so it never replaces or filters one. At most one skip per edge; never chained with a swap, a fuzzy flip, an
// abbreviation or a completion; English lookups are not touched. No learning adjustment into or out of it. It carries
// its kind to Segment::correction (kCorrectionDup). Off until SetDupCost gets a cost of 0 or more. Requires
// SIME_WEAVE_FUZZY (Segment::correction and the work counts). Default 1 on the p5u3 candidate (local weave, 2026-11-15; was 0): compiled in, and no edge is added
// until SetDupCost gets a cost.
#ifndef SIME_WEAVE_DUP_EDGE
#define SIME_WEAVE_DUP_EDGE 1
#endif
#if SIME_WEAVE_DUP_EDGE && !SIME_WEAVE_FUZZY
#error "SIME_WEAVE_DUP_EDGE needs SIME_WEAVE_FUZZY"
#endif
// SIME_WEAVE_UE_ALIAS=1 (experiment, local branch weave-dup, weave-ime Phase 5 owner feedback of 2026-11-15): lue and
// nue typed for the stored lve and nve (略 lüè is commonly typed lue). The sentence decoder also looks pinyin words up
// with a typed u read as the stored v where l or n starts the syllable and e follows (lue -> lve, nue -> nve; lu never
// becomes lv). Each match is an extra edge over the typed span at a runtime cost per alias (SetUeAliasCost) plus the
// word's reading-table line, added after the exact and overlay edges and before the swap, fuzzy and doubled-letter edges
// (an equivalence, not a correction), so it never replaces or filters an exact edge: lu|e stays. It learns like an
// exact edge. It carries its kind to Segment::correction (kCorrectionUeAlias). Off until SetUeAliasCost gets a cost of 0
// or more. Requires SIME_WEAVE_FUZZY (Segment::correction). Default 1 on the p5u3 candidate (local weave, 2026-11-15; was 0): compiled in, and no edge is added
// until SetUeAliasCost gets a cost.
#ifndef SIME_WEAVE_UE_ALIAS
#define SIME_WEAVE_UE_ALIAS 1
#endif
#if SIME_WEAVE_UE_ALIAS && !SIME_WEAVE_FUZZY
#error "SIME_WEAVE_UE_ALIAS needs SIME_WEAVE_FUZZY"
#endif
// SIME_WEAVE_NEIGHBOUR_EDGE=1 (experiment, local branch weave-nb, weave-ime Phase 5 owner feedback of 2026-11-15): a key
// pressed for its QWERTY neighbour. The sentence decoder also looks pinyin words up with one typed letter read as one of
// its neighbours on a staggered QWERTY layout (zhonhguo reaches zhongguo, 中国). Each match is an extra edge over the
// typed span at a runtime cost per (typed, intended) pair (SetNeighbourCost, SetNeighbourPairCost) plus the word's
// reading-table line, added after every other pinyin edge is final, so it never replaces or filters one. At most one
// substitution per edge; never chained with a swap, a fuzzy flip, a doubled letter, an alias, an abbreviation or a
// completion; English lookups are not touched. No learning adjustment into or out of it. It carries its kind to
// Segment::correction (kCorrectionNeighbour). Off until SetNeighbourCost gets a cost of 0 or more. Requires
// SIME_WEAVE_FUZZY (Segment::correction and the work counts). Default 0.
#ifndef SIME_WEAVE_NEIGHBOUR_EDGE
#define SIME_WEAVE_NEIGHBOUR_EDGE 0
#endif
#if SIME_WEAVE_NEIGHBOUR_EDGE && !SIME_WEAVE_FUZZY
#error "SIME_WEAVE_NEIGHBOUR_EDGE needs SIME_WEAVE_FUZZY"
#endif
// SIME_WEAVE_EN_TYPO=1 (experiment, local branch weave-nb, weave-ime Phase 5 owner feedback of 2026-11-15): one keyboard
// slip inside an English word. The sentence decoder also looks English words (the L0 English DAT and the overlay's) up
// with one edit of the typed letters: a letter read as its QWERTY neighbour, a typed letter skipped (an extra key), a
// letter supplied (a missing key) or two adjacent letters read swapped (researchproposak reaches proposal). Each match is
// an extra English edge over the typed span at a runtime cost per edit kind (SetEnTypoCost) plus the English penalty an
// exact edge over that span would pay, added after every other edge is final, so it never replaces or filters one. At
// most one edit per edge; the corrected word has at least min_len letters; with the gate on, a typed span that is itself
// an English word (an exact English or overlay edge over the same span) is never corrected. No learning adjustment into
// or out of it. It carries its kind to Segment::correction (kCorrectionEnTypo). Off until SetEnTypoCost gets a cost of 0
// or more. Requires SIME_WEAVE_NEIGHBOUR_EDGE (the QWERTY table). Default 0.
#ifndef SIME_WEAVE_EN_TYPO
#define SIME_WEAVE_EN_TYPO 0
#endif
#if SIME_WEAVE_EN_TYPO && !SIME_WEAVE_NEIGHBOUR_EDGE
#error "SIME_WEAVE_EN_TYPO needs SIME_WEAVE_NEIGHBOUR_EDGE"
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
#if SIME_WEAVE_FUZZY
    // How the typed letters were read: 0 as typed, 1 swapped letters (SIME_WEAVE_SWAP_EDGE), 2 fuzzy pinyin, 3 a doubled
    // letter read once (SIME_WEAVE_DUP_EDGE), 4 lue/nue read as lve/nve (SIME_WEAVE_UE_ALIAS), 5 a
    // letter read as its QWERTY neighbour (SIME_WEAVE_NEIGHBOUR_EDGE), 6 an English word read with one keyboard slip
    // (SIME_WEAVE_EN_TYPO). A
    // keyboard slip and a phonological confusion are kept apart (weave-ime WP4.3).
    std::uint8_t correction = 0;
#endif
};
#if SIME_WEAVE_FUZZY
constexpr std::uint8_t kCorrectionSwap = 1;
constexpr std::uint8_t kCorrectionFuzzy = 2;
#if SIME_WEAVE_DUP_EDGE
constexpr std::uint8_t kCorrectionDup = 3;
#endif
#if SIME_WEAVE_UE_ALIAS
constexpr std::uint8_t kCorrectionUeAlias = 4;
#endif
#if SIME_WEAVE_NEIGHBOUR_EDGE
constexpr std::uint8_t kCorrectionNeighbour = 5;
#endif
#if SIME_WEAVE_EN_TYPO
constexpr std::uint8_t kCorrectionEnTypo = 6;
// The English edit kinds, each priced on its own (SetEnTypoCost): a neighbour key for the intended letter, an extra
// typed key, a missing key, two adjacent letters swapped.
enum class EnTypoKind : std::uint8_t { kSubstitute = 0, kExtra = 1, kMissing = 2, kTranspose = 3 };
constexpr std::size_t kEnTypoKinds = 4;
#endif
// The fuzzy pairs, each switched and priced on its own (SetFuzzyCost).
enum class FuzzyPair : std::uint8_t { kAnAng = 0, kEnEng = 1, kInIng = 2 };
constexpr std::size_t kFuzzyPairs = 3;
#endif

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
#if SIME_WEAVE_RIGHT_CONTEXT
    // weave-ime (SIME_WEAVE_RIGHT_CONTEXT): the LM cost of `utf8`: the Cutter's cut (TokenizeContext's), scored from the
    // start state, ScoreMove then Back per token; a character with no dictionary token costs the unknown back-off.
    // 0 for empty text or an engine that isn't ready. A host compares TextCost(tail + right) - TextCost(tail) across
    // candidates (weave-ime benchmarks/results/phase5/insertion_context/).
    double TextCost(std::string_view utf8) const;
#endif
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
#if SIME_WEAVE_SWAP_EDGE
    // Edge cost for swapped letters; a negative cost switches the edges off. A swap edge covers at least min_span
    // typed bytes.
    void SetSwapCost(float_t swap_cost, std::size_t min_span = 2);
    // Work counts of the last lattice built by the sentence decoder.
    struct LatticeWork {
        std::size_t edges = 0;         // edges of the whole lattice, sentinels and swap edges included
        std::size_t swap_edges = 0;    // of which added by swapped letters
        std::size_t walk_steps = 0;    // pinyin trie steps of the corrected walks (the exact lookup not counted)
#if SIME_WEAVE_FUZZY
        std::size_t fuzzy_edges = 0;       // of `edges`, added by fuzzy pinyin
        std::size_t fuzzy_walk_steps = 0;  // pinyin trie steps of the fuzzy walks (exact prefix steps included)
#endif
#if SIME_WEAVE_DUP_EDGE
        std::size_t dup_edges = 0;         // of `edges`, added by doubled letters
        std::size_t dup_walk_steps = 0;    // pinyin trie steps of the doubled-letter walks (exact prefix steps included)
#endif
#if SIME_WEAVE_NEIGHBOUR_EDGE
        std::size_t nb_edges = 0;          // of `edges`, added by neighbour-key substitutions
        std::size_t nb_walk_steps = 0;     // pinyin trie steps of the neighbour walks (exact prefix steps included)
#endif
#if SIME_WEAVE_EN_TYPO
        std::size_t en_typo_edges = 0;     // of `edges`, added by English slips
        std::size_t en_typo_walk_steps = 0;  // English trie steps of the slip walks (exact prefix steps included)
#endif
    };
    LatticeWork LastLatticeWork() const;
#endif
#if SIME_WEAVE_FUZZY
    // Edge cost per flip of one fuzzy pair; a negative cost switches the pair off (the default for every pair).
    void SetFuzzyCost(FuzzyPair pair, float_t cost);
    // A fuzzy edge covers at least min_span typed bytes and makes at most max_flips flips (each paid).
    void SetFuzzyLimits(std::size_t min_span, std::size_t max_flips);
#endif
#if SIME_WEAVE_DUP_EDGE
    // Edge cost of a doubled letter read once; a negative cost (the default) switches the edges off. A doubled-letter
    // edge covers at least min_span typed bytes.
    void SetDupCost(float_t cost, std::size_t min_span = 3);
#endif
#if SIME_WEAVE_UE_ALIAS
    // Edge cost per lue/nue alias; a negative cost (the default) switches the edges off.
    void SetUeAliasCost(float_t cost);
#endif
#if SIME_WEAVE_NEIGHBOUR_EDGE
    // Edge cost of one typed letter read as a QWERTY neighbour (staggered rows qwertyuiop / asdfghjkl / zxcvbnm, offsets
    // 0, 1/4, 3/4 key; a neighbour is beside it in its row, or in the row above or below less than one key away). Sets
    // every neighbour pair in the same row to `cost` and every pair across rows to `cross_cost`; a negative `cost` (the
    // default) switches the edges off, a negative `cross_cost` only the pairs across rows. A neighbour edge covers at
    // least min_span typed bytes.
    void SetNeighbourCost(float_t cost, float_t cross_cost, std::size_t min_span = 3);
    static bool QwertySameRow(char a, char b);
    // Replaces the cost of one (typed, intended) pair after SetNeighbourCost; a negative cost drops that pair. Returns
    // false (and changes nothing) unless both are letters a-z and neighbours.
    bool SetNeighbourPairCost(char typed, char intended, float_t cost);
    static bool QwertyNeighbours(char a, char b);
#endif
#if SIME_WEAVE_EN_TYPO
    // Edge cost of one English slip of the given kind; a negative cost (the default for every kind) switches that kind
    // off. The corrected word has at least min_len letters; with `gate`, a typed span that is already an English word is
    // never corrected.
    void SetEnTypoCost(EnTypoKind kind, float_t cost);
    void SetEnTypoLimits(std::size_t min_len, bool gate);
#endif
#if SIME_WEAVE_READING_PRIOR
    // Lines `character<TAB>reading<TAB>penalty` (UTF-8; penalty a plain decimal number in [0, 1000], natural-log
    // cost, parsed independently of the C locale, or the word `drop`, which removes the edge altogether). A pair
    // listed twice, an empty penalty and a hex or out-of-range number are refused. Replaces any earlier table. Fails, leaving the table empty, on a malformed line or a pair the dictionary does not hold as a
    // single-character item.
    bool LoadReadingPrior(const std::filesystem::path& path, std::string* error = nullptr);
    std::size_t ReadingPriorSize() const;
#endif
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
#if SIME_WEAVE_SWAP_EDGE
        bool swap = false;             // edge added by AppendSwapEdges (the typed letters are not the reading)
#endif
#if SIME_WEAVE_FUZZY
        bool fuzzy = false;            // edge added by AppendFuzzyEdges (a nasal final read as its partner)
#endif
#if SIME_WEAVE_DUP_EDGE
        bool dup = false;              // edge added by AppendDupEdges (one copy of a doubled letter skipped)
#endif
#if SIME_WEAVE_UE_ALIAS
        bool ue_alias = false;         // edge added by AppendUeAliasEdges (lue/nue read as lve/nve)
#endif
#if SIME_WEAVE_NEIGHBOUR_EDGE
        bool nb = false;               // edge added by AppendNeighbourEdges (one letter read as its QWERTY neighbour)
#endif
#if SIME_WEAVE_EN_TYPO
        bool en_typo = false;          // edge added by AppendEnTypoEdges (an English word read with one slip)
#endif
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
#if SIME_WEAVE_READING_PRIOR
    void ApplyReadingPrior(std::vector<Node>& net) const;
#endif
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
#if SIME_WEAVE_SWAP_EDGE
    void AppendSwapEdges(std::vector<Node>& net, std::string_view input) const;
#endif
#if SIME_WEAVE_FUZZY
    void AppendFuzzyEdges(std::vector<Node>& net, std::string_view input) const;
#endif
#if SIME_WEAVE_DUP_EDGE
    void AppendDupEdges(std::vector<Node>& net, std::string_view input) const;
#endif
#if SIME_WEAVE_UE_ALIAS
    void AppendUeAliasEdges(std::vector<Node>& net, std::string_view input) const;
#endif
#if SIME_WEAVE_NEIGHBOUR_EDGE
    void AppendNeighbourEdges(std::vector<Node>& net, std::string_view input) const;
#endif
#if SIME_WEAVE_EN_TYPO
    void AppendEnTypoEdges(std::vector<Node>& net, std::string_view input) const;
#endif
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
#if SIME_WEAVE_SWAP_EDGE
    float_t swap_cost_ = -1;
    std::size_t swap_min_span_ = 2;
    mutable LatticeWork lattice_work_;
#endif
#if SIME_WEAVE_FUZZY
    float_t fuzzy_cost_[kFuzzyPairs] = {-1, -1, -1};
    std::size_t fuzzy_min_span_ = 3;
    std::size_t fuzzy_max_flips_ = 1;
#endif
#if SIME_WEAVE_DUP_EDGE
    float_t dup_cost_ = -1;
    std::size_t dup_min_span_ = 3;
#endif
#if SIME_WEAVE_UE_ALIAS
    float_t ue_alias_cost_ = -1;
#endif
#if SIME_WEAVE_NEIGHBOUR_EDGE
    // nb_cost_[typed * 26 + intended]; negative = no edge. nb_on_ is false until SetNeighbourCost gets a cost >= 0.
    std::array<float_t, 26 * 26> nb_cost_{};
    bool nb_on_ = false;
    std::size_t nb_min_span_ = 3;
#endif
#if SIME_WEAVE_EN_TYPO
    float_t en_typo_cost_[kEnTypoKinds] = {-1, -1, -1, -1};
    std::size_t en_typo_min_len_ = 4;
    bool en_typo_gate_ = true;
#endif
#if SIME_WEAVE_READING_PRIOR
    // token -> (reading, penalty); only single-character tokens with a listed reading.
    std::unordered_map<TokenID, std::vector<std::pair<std::string, float_t>>> reading_prior_;
    std::size_t reading_prior_size_ = 0;
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
