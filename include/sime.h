// Modified by the weave-ime project (2026): search constants EnglishPenalty, EnglishPenaltyShort, BeamSize, and the scoped-penalty constants are macro-backed (SIME_WEAVE_*); adds SIME_WEAVE_CASE_FOLD and the SIME_WEAVE_LITERAL_EDGE literal-letters edge (with SIME_WEAVE_LITERAL_PER_LETTER and SIME_WEAVE_LITERAL_NONPINYIN); adds SIME_WEAVE_SPACE_BOUNDARY and SIME_WEAVE_RAW_CNT; weave defaults PENALTY_RULE=1, EN_PENALTY_MIXED=0.0, EN_SHORT_EDGE_LEN=2, EN_PENALTY_SHORT=4.0 (Phase 4.5 Task 2; were 3 and 8.0), CASE_FOLD=1, RAW_CNT=1; adds Sime::TokenizeContext (additive, no macro); adds the SIME_WEAVE_OVERLAY lexicon overlay API (LoadOverlay, layer switches, display casing, dedup by key; default 0) (see NOTICE.weave.md).
#pragma once

#include "common.h"
#include "score.h"
#include "state.h"
#include "dict.h"
#include "user.h"
#include "overlay.h"
#include <filesystem>
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

namespace sime {

class GruReranker;
class Cutter;
#if SIME_WEAVE_OVERLAY
enum class DisplayCasing { kCanonical = 0, kAsTyped = 1 };
#endif

struct DecodeResult {
    std::string text;        // UTF-8 display text (▁ prefix stripped)
    std::string units;       // segmented pinyin (e.g. "ni'hao")
    std::vector<TokenID> tokens;  // token IDs for LM context
    float_t score = 0.0;    // larger is better (negative log probability negated)
    std::size_t cnt = 0;     // bytes of input consumed
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
    UserSentence& MutableUserSentence() { return user_sentence_; }
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
};

} // namespace sime
