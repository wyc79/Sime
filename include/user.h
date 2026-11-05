// Modified by the weave-ime project (2026): SIME_WEAVE_LEARNING (default 0) adds a commit clock, lazily decayed per-entry counts, a bigram-only switch, a fixed bound on the adjustment, a counts-only file with strict loading, pruning and a merge of what an engine taught (Phase 5 F14, Docs/plans/phase-5-unfreeze.md).
#pragma once

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// weave-ime Phase 5 F14: selection learning for the host. 0 (default): this header and user.cc behave as without it.
// 1: the knobs exist; with all of them at their defaults CostAdjustment still takes the original path, so the
// output does not change.
#ifndef SIME_WEAVE_LEARNING
#define SIME_WEAVE_LEARNING 0
#endif

namespace sime {

class UserSentence {
public:
    using Sentence = std::vector<TokenID>;
    using TokenTextFn = std::function<std::string(TokenID)>;

    // One candidate pick: the token IDs the decoder produced.
    struct Selection {
        Sentence tokens;
    };

    // One paragraph = a sequence of consecutive selections that share
    // an unbroken context (i.e. weren't separated by punctuation, IM
    // switch or focus loss). Bigram counting flattens all selections.
    struct Entry {
        std::vector<Selection> selections;
    };

    UserSentence() = default;

    void Clear();

    // vocab_sig: opaque tag identifying the vocabulary the stored
    // token IDs were saved against. Mismatch on Load → reject the file.
    // token_text: optional id→string callback used by Save to emit
    // a tab-separated, per-token-aligned annotation column for
    // human-readable inspection. Null = ids only, no annotation.
    bool Load(const std::filesystem::path& path,
              std::string_view vocab_sig = {});
    bool Save(const std::filesystem::path& path,
              std::string_view vocab_sig = {},
              const TokenTextFn& token_text = {}) const;
    bool LoadText(std::istream& in, std::string_view vocab_sig = {});
    bool SaveText(std::ostream& out,
                  std::string_view vocab_sig = {},
                  const TokenTextFn& token_text = {}) const;

    // Migration path for vocab-mismatched files. Reads the file
    // ignoring stored token IDs, joins each paragraph's text column
    // back to a continuous string, and re-tokenizes via `tokenize`
    // (typically a Cutter.Cut bound by Sime). Each new token becomes
    // its own Selection. Caller persists the migrated state with a
    // fresh vocab_sig.
    using TokenizeFn =
        std::function<std::vector<std::pair<TokenID, std::string>>(
            std::string_view)>;
    bool LoadAndMigrate(const std::filesystem::path& path,
                        const TokenizeFn& tokenize);

    void SetMaxSentenceCount(std::size_t count);
    std::size_t MaxSentenceCount() const { return max_sentence_count_; }
    std::size_t SentenceCount() const { return sentences_.size(); }

    void SetWeight(float_t weight);
    float_t Weight() const { return weight_; }

    // Records a single user pick. `context` is the recent committed
    // tokens (typically `scorer_.Num()-1` deep). If the most recent
    // paragraph's flattened tail equals `context`, the new selection
    // is appended to that paragraph; otherwise a new paragraph starts.
    void Add(const Sentence& context, const Sentence& tokens);

    std::uint32_t UnigramCount(TokenID token) const;
    std::uint32_t BigramCount(TokenID prev, TokenID cur) const;
    std::uint32_t TotalUnigramCount() const { return total_unigram_count_; }
    // N1+(prev, *) — number of distinct successor tokens after `prev`.
    // Used by Ney's absolute-discounting backoff weight (γ).
    std::uint32_t DistinctSuccessors(TokenID prev) const;
    // Cost adjustment to add to LM cost for a transition prev→cur.
    // `lm_step` is the LM cost (positive, = -log P) for the same
    // transition. Returns ≤ 0; never makes the cost worse than LM
    // alone. Mirrors libime's UserLanguageModel::score: linear mixture
    // in probability space, max-floored at the LM score.
    float_t CostAdjustment(TokenID prev, TokenID cur, float_t lm_step) const;

    const std::deque<Entry>& Sentences() const { return sentences_; }

#if SIME_WEAVE_LEARNING
    // Default-constructed: the original model. half_life_*: commits for an entry's effective count to halve,
    // effective = count x 2^-((now - last) / H); 0 = no decay. Both 0 use the integer counts above. max_adjust:
    // the adjustment of one word step never exceeds this many nats; 0 = no bound. bigram_only: no unigram term,
    // a word with no learned bigram from its predecessor gets nothing.
    struct LearnConfig {
        bool bigram_only = false;
        float_t half_life_uni = 0.0;
        float_t half_life_bi = 0.0;
        float_t max_adjust = 0.0;
    };
    void SetLearnConfig(const LearnConfig& config) { learn_ = config; }
    const LearnConfig& GetLearnConfig() const { return learn_; }

    // One commit happened. The clock is commits, never wall time. `unlearned_tokens`: tokens of a commit that was
    // not learned; with decay on they add to the denominator total, so a word's share still falls while commits go
    // by unlearned. With no decay they change nothing.
    void NoteCommit(std::size_t unlearned_tokens);
    std::uint64_t Clock() const { return clock_; }

    // Effective counts now (the integer ones when no decay is configured).
    float_t EffectiveUnigram(TokenID token) const;
    float_t EffectiveBigram(TokenID prev, TokenID cur) const;
    float_t EffectiveTotal() const;
    std::size_t UnigramEntries() const { return unigram_.size(); }
    std::size_t BigramEntries() const { return bigram_.size(); }
    // Entries whose effective count is at least `floor` (what a pruned file would keep).
    std::size_t LiveUnigrams(float_t floor) const;
    std::size_t LiveBigrams(float_t floor) const;
    // Work counts: CostAdjustment calls, and calls that found the word in the store.
    std::uint64_t AdjustCalls() const { return adjust_calls_; }
    std::uint64_t AdjustHits() const { return adjust_hits_; }

    // Counts-only file: no sentences, no paragraphs, no text. Sorted by key, so file order keeps no sequence. It ends
    // with the row count, so a cut file is caught. Writing to a temporary name and renaming is the caller's job.
    bool SaveCounts(const std::filesystem::path& path, std::string_view vocab_sig = {}) const;
    // Strict: parses into a temporary and changes this store only if the whole file is good. Refuses another
    // vocabulary (when vocab_sig is given), a bad token (NotToken, an overlay token, or one `token_ok` refuses), a
    // non-finite or negative count, a `last` past the file's clock, a repeated key, a pair or successor row whose
    // token has no unigram row, or a wrong row count. On success the sentences and the unwritten delta are gone;
    // the knobs and the weight stay.
    bool LoadCounts(const std::filesystem::path& path, std::string_view vocab_sig = {},
                    const std::function<bool(TokenID)>& token_ok = {});

    // Drops pairs at effective count <= 0.5 (the bigram discount makes them do nothing), then the oldest last-seen over
    // `max_pairs`. A word entry stays if a kept pair uses it, on either side, or its effective count is >= 0.05.
    // Returns the pairs dropped.
    std::size_t Prune(std::size_t max_pairs);

    // What this store taught since the last ClearDelta. The commit count and the tokens that were not learned go
    // with it, so a merge moves the clock and the total the same way.
    struct CountsDelta {
        std::uint64_t commits = 0;
        std::uint64_t unlearned_tokens = 0;
        std::unordered_map<TokenID, std::uint32_t> unigram;
        std::unordered_map<std::uint64_t, std::uint32_t> bigram;
    };
    const CountsDelta& Delta() const { return delta_; }
    void ClearDelta() { delta_ = {}; }

    // Commit-clock merge: the clock goes up by delta.commits, and each delta entry lands at the new clock, so
    // count = effective count + delta, last = new clock. Commit times inside one delta are lost: at 64 commits and
    // H 2000 an entry reads about 2% high at most. A word a pair needs and this store lacks gets the pair's count, so
    // the result is always a file LoadCounts takes. Uses this store's knobs. Doesn't record a delta of its own.
    void MergeCounts(const CountsDelta& delta);
#endif

private:
    static std::uint64_t BigramKey(TokenID prev, TokenID cur);
    static Sentence CleanSentence(const Sentence& sentence);
    static Sentence Flatten(const Entry& entry);
    static bool EndsWith(const Sentence& sentence, const Sentence& suffix);

    void StartParagraph(Selection selection);
    void AppendToLatest(Selection selection);
    void AdjustEntryCounts(const Entry& entry, int delta);
    void Rebuild();

    std::size_t max_sentence_count_ = 8192;
    // libime's UserLanguageModel uses 0.2 as its default mixture
    // weight (constants.h: DEFAULT_USER_LANGUAGE_MODEL_USER_WEIGHT).
    float_t weight_ = 0.2;
    std::deque<Entry> sentences_;
    std::unordered_map<TokenID, std::uint32_t> unigram_;
    std::unordered_map<std::uint64_t, std::uint32_t> bigram_;
    // N1+(prev, *): distinct-successor count per prev token. Maintained
    // incrementally when bigram counts cross the 0 boundary.
    std::unordered_map<TokenID, std::uint32_t> distinct_successors_;
    std::uint32_t total_unigram_count_ = 0;

#if SIME_WEAVE_LEARNING
    // count as of commit `last`; lazy: only an entry that is touched is rewritten.
    struct Aged {
        float_t count = 0.0;
        std::uint64_t last = 0;
    };
    static float_t Effective(const Aged& e, std::uint64_t now, float_t half_life);
    static void Bump(Aged& e, std::uint64_t now, float_t half_life, float_t delta);
    void AgeToken(TokenID prev, TokenID token);
    float_t AgedAdjustment(TokenID prev, TokenID cur, float_t lm_step) const;
    float_t Mix(float_t pr_user, float_t lm_step) const;

    LearnConfig learn_;
    std::uint64_t clock_ = 0;
    std::unordered_map<TokenID, Aged> aged_uni_;
    std::unordered_map<std::uint64_t, Aged> aged_bi_;
    std::unordered_map<TokenID, Aged> aged_succ_;
    Aged aged_total_;
    CountsDelta delta_;
    mutable std::uint64_t adjust_calls_ = 0;
    mutable std::uint64_t adjust_hits_ = 0;
#endif
};

} // namespace sime
