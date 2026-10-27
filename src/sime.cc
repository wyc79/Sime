// Modified by the weave-ime project (2026): ComputeEdgePenalties gains the SIME_WEAVE_PENALTY_RULE scoped English penalty; English DAT lookups gain SIME_WEAVE_CASE_FOLD (folded and, for a span with capitals, as-typed lookups merged per folded key) and AbbreviatePieces matches letters ignoring case under it; InitNet gains the SIME_WEAVE_LITERAL_EDGE literal-letters edge (with SIME_WEAVE_LITERAL_PER_LETTER and SIME_WEAVE_LITERAL_NONPINYIN); NormalizeInput, InitNet and DecodeSentence gain SIME_WEAVE_SPACE_BOUNDARY and SIME_WEAVE_RAW_CNT; adds Sime::TokenizeContext (Cutter over the last N code points of committed text; additive, no macro); adds the SIME_WEAVE_OVERLAY lexicon overlay (overlay English edges at a bounded cost with a history break, display casing, dedup by key; default 0); adds the candidate-contract provenance (edge flags kept through State and Backtrace, DecodeResult::segments and ::basis from DecodeSentence; additive, no macro); DecodeSentence split into BuildSentenceNet / SentenceResults and Process into Relax, with segments filled only for emitted candidates (no output change); adds SIME_WEAVE_INCREMENTAL incremental decoding (snapshots per normalised prefix, Process resumed at the first changed column, generation counter, fallback; default 0).
#include "sime.h"

#include "cut.h"
#include "gru_reranker.h"
#include "ustr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sime {

namespace {

constexpr std::size_t kGruCandidates = 10;
constexpr float_t kGruPinyinScale = 0.60;
constexpr float_t kGruT9Scale = 0.72;

void RerankWithGru(std::vector<DecodeResult>& candidates,
                   const GruReranker* reranker,
                   bool t9) {
    if (!reranker || !reranker->Ready()) return;
    if (candidates.size() > kGruCandidates) {
        candidates.resize(kGruCandidates);
    }
    const float_t scale = t9 ? kGruT9Scale : kGruPinyinScale;
    for (auto& candidate : candidates) {
        candidate.score += scale * reranker->Score(candidate.tokens, t9);
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const DecodeResult& lhs, const DecodeResult& rhs) {
            return lhs.score > rhs.score;
        });
}

#if SIME_WEAVE_CASE_FOLD
// weave case fold: the English DAT is looked up with the typed span's ASCII
// letters lowercased (pair with a dictionary whose English keys are
// lowercase, benchmarks/engines/sime/rebuild_dict.sh --transform
// lowercase-en). Bytes keep their positions, so DAT match lengths are spans
// of the typed input.
std::string FoldEnKey(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// True iff the token's text equals `typed` byte for byte, or, with
// `prefix`, starts with it (completion edges cover a typed prefix).
bool TokenMatchesTyped(const Dict& dict, TokenID id, std::string_view typed,
                       bool prefix) {
    const char32_t* t = dict.TokenAt(id);
    if (!t) return false;
    std::size_t i = 0;
    for (; i < typed.size(); ++i) {
        if (t[i] == 0 || t[i] != static_cast<unsigned char>(typed[i])) {
            return false;
        }
    }
    return prefix || t[i] == 0;
}

// Calls f(item) for the `count` items to emit for the typed span: all of
// them, unless the span has an uppercase letter and at least one item's
// token matches the typed casing; then only the matching items.
template <class F>
void ForEachCasedItem(const Dict& dict, const Dict::Item* items,
                      std::size_t count, std::string_view typed, bool prefix,
                      F&& f) {
    bool only_matching = false;
    if (std::any_of(typed.begin(), typed.end(),
                    [](char c) { return c >= 'A' && c <= 'Z'; })) {
        for (std::size_t i = 0; i < count && !only_matching; ++i) {
            only_matching = TokenMatchesTyped(dict, items[i].id, typed, prefix);
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!only_matching
            || TokenMatchesTyped(dict, items[i].id, typed, prefix)) {
            f(items[i]);
        }
    }
}

// One case-folded English lookup for the typed span `typed` (`folded` is
// FoldEnKey(typed)): calls f(length, item) for every item to emit, length
// being the DAT match length. lookup(key) runs the DAT search. With
// `filter`, ForEachCasedItem picks the casing per group of items (`typed`
// cut to the match length for exact matches, whole for `prefix`
// completions); without it every item is emitted.
//
// A dictionary with cased English keys (upstream's save/sime.dict) keeps
// `Unity` and `HTTP` under capitalised keys, which the folded key cannot
// reach. So when the typed span has capitals, the typed span is looked up
// as typed too, and the results of both lookups are grouped by match length
// and folded key (an English item's pieces are its key): first-seen order,
// a later entry adding only the token IDs its group lacks. The casing filter
// then runs over each group. With lowercase English keys the as-typed
// lookup finds no key the folded one does not (a key matching a capital
// would need a capital), so every group is one folded-lookup entry as is.
template <class Lookup, class F>
void ForEachFoldedEnItem(const Dict& dict, std::string_view typed,
                         std::string_view folded, bool prefix, bool filter,
                         Lookup&& lookup, F&& f) {
    auto emit = [&](std::size_t length, const Dict::Item* items,
                    std::size_t count) {
        if (!filter) {
            for (std::size_t i = 0; i < count; ++i) f(length, items[i]);
            return;
        }
        ForEachCasedItem(dict, items, count,
                         prefix ? typed : typed.substr(0, length), prefix,
                         [&](const Dict::Item& item) { f(length, item); });
    };
    const auto results = lookup(folded);
    if (typed == folded) {
        for (const auto& r : results) {
            const auto entry = dict.GetEntry(Dict::LetterEn, r.value);
            emit(r.length, entry.items, entry.count);
        }
        return;
    }
    struct Group {
        std::size_t length;
        std::vector<Dict::Item> items;
    };
    std::vector<Group> groups;
    std::unordered_map<std::string, std::size_t> index;
    auto add = [&](const std::vector<trie::SearchResult>& rs) {
        for (const auto& r : rs) {
            const auto entry = dict.GetEntry(Dict::LetterEn, r.value);
            std::string key = std::to_string(r.length) + ':';
            if (entry.count && entry.items[0].pieces) {
                key += FoldEnKey(entry.items[0].pieces);
            }
            const auto [it, fresh] = index.emplace(std::move(key), groups.size());
            if (fresh) {
                groups.push_back({r.length, std::vector<Dict::Item>(
                    entry.items, entry.items + entry.count)});
                continue;
            }
            auto& items = groups[it->second].items;
            for (uint32_t i = 0; i < entry.count; ++i) {
                const TokenID id = entry.items[i].id;
                if (std::none_of(items.begin(), items.end(),
                        [&](const Dict::Item& x) { return x.id == id; })) {
                    items.push_back(entry.items[i]);
                }
            }
        }
    };
    add(results);
    add(lookup(typed));
    for (const auto& g : groups) emit(g.length, g.items.data(), g.items.size());
}
#endif

bool BetterLayer2Entry(const DecodeResult& lhs, const DecodeResult& rhs) {
    // Dedup tie-break: pure score comparison. Penalty is already in score
    // so the cleaner-provenance entry (exact emit, no expansion penalty)
    // naturally wins.
    if (lhs.score != rhs.score) return lhs.score > rhs.score;
    if (lhs.cnt != rhs.cnt) return lhs.cnt > rhs.cnt;
    return lhs.units.size() > rhs.units.size();
}

void PushBestLayer2Entry(std::vector<DecodeResult>& best_entries,
                         std::unordered_map<std::string, std::size_t>& index_by_text,
                         DecodeResult candidate) {
    auto it = index_by_text.find(candidate.text);
    if (it == index_by_text.end()) {
        index_by_text.emplace(candidate.text, best_entries.size());
        best_entries.push_back(std::move(candidate));
        return;
    }
    auto& current = best_entries[it->second];
    if (BetterLayer2Entry(candidate, current)) {
        current = std::move(candidate);
    }
}

#if SIME_WEAVE_OVERLAY
std::string FoldAsciiLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}
#endif

bool IsAsciiLetter(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); }
char32_t LowerAscii(char32_t c) { return (c >= U'A' && c <= U'Z') ? c - U'A' + U'a' : c; }

// D5 "as typed": `text` with each ASCII letter in the casing of the matching typed letter
// (letters past the typed ones lowercase). Unchanged when the text is not all ASCII, or its
// letters do not spell the typed letters (C# typed csharp), or there is no typed span.
std::u32string AsTyped(const std::u32string& text, std::string_view typed) {
    if (typed.empty()) return text;
    if (std::any_of(text.begin(), text.end(), [](char32_t c) { return c >= 0x80; })) return text;
    std::u32string out = text;
    std::size_t j = 0;
    auto next_letter = [&] {
        while (j < typed.size() && !IsAsciiLetter(static_cast<unsigned char>(typed[j]))) ++j;
    };
    for (auto& c : out) {
        if (!IsAsciiLetter(c)) continue;
        next_letter();
        if (j >= typed.size()) {
            c = LowerAscii(c);
            continue;
        }
        const char32_t t = static_cast<unsigned char>(typed[j++]);
        if (LowerAscii(t) != LowerAscii(c)) return text;
        c = t;
    }
    next_letter();
    return j < typed.size() ? text : out;
}

// Vocabulary signature used to detect stale user-sentence files when the
// LM is regenerated (new TokenIDs). LM file size + mtime is a cheap proxy
// for "was the LM regenerated"; when it changes we drop the old file.
std::string MakeVocabSignature(const std::filesystem::path& dict_path,
                               const std::filesystem::path& lm_path) {
    std::error_code ec;
    auto dict_size = std::filesystem::file_size(dict_path, ec);
    if (ec) return {};
    auto lm_size = std::filesystem::file_size(lm_path, ec);
    if (ec) return {};
    auto lm_mtime = std::filesystem::last_write_time(lm_path, ec);
    if (ec) return {};
    auto lm_mtime_secs =
        std::chrono::duration_cast<std::chrono::seconds>(
            lm_mtime.time_since_epoch())
            .count();
    std::ostringstream ss;
    ss << dict_size << ':' << lm_size << ':' << lm_mtime_secs;
    return ss.str();
}

} // namespace

void Sime::MaybeTrimCaches() const {
    if (++decode_count_ % kSepCacheTrimInterval == 0) {
        dict_.ClearSepCaches();
    }
}

void Sime::ResetCaches() const {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    dict_.ResetSepCaches();
}

Sime::Sime(const std::filesystem::path& dict_path,
                         const std::filesystem::path& model_path) {
    if (!dict_.Load(dict_path)) {
        return;
    }
    if (!scorer_.Load(model_path)) {
        dict_.Clear();
        return;
    }
    vocab_sig_ = MakeVocabSignature(dict_path, model_path);
    gru_ = std::make_unique<GruReranker>();
    gru_->Load(model_path.parent_path());
    ready_ = true;
}

Sime::~Sime() = default;

bool Sime::GruReady() const {
    return gru_ && gru_->Ready();
}

void Sime::SetUserSentenceEnabled(bool enabled) {
    user_sentence_enabled_ = enabled;
#if SIME_WEAVE_INCREMENTAL
    BumpGenerationLocked();
#endif
}

UserSentence& Sime::MutableUserSentence() {
#if SIME_WEAVE_INCREMENTAL
    BumpGenerationLocked();
#endif
    return user_sentence_;
}

bool Sime::LoadUserSentence(const std::filesystem::path& path) {
#if SIME_WEAVE_INCREMENTAL
    // Bumps the generation once the store has been (re)loaded, on every return path.
    struct Bump {
        const Sime* e;
        ~Bump() { e->BumpGenerationLocked(); }
    } bump{this};
#endif
    if (user_sentence_.Load(path, vocab_sig_)) {
        return true;  // fast path: vocab unchanged
    }

    // Vocab mismatch (LM regenerated, token IDs shifted). Try to
    // migrate: re-tokenize each paragraph's text under the new
    // vocabulary via Cutter, then persist immediately so the file is
    // consistent and next load takes the fast path.
    if (!ready_) return false;
    Cutter cutter(dict_, scorer_);
    auto tokenize = [&cutter](std::string_view text) {
        std::vector<std::pair<TokenID, std::string>> out;
        auto cuts = cutter.Cut(text);
        out.reserve(cuts.size());
        for (auto& ct : cuts) {
            TokenID id = ct.is_unk ? NotToken : ct.id;
            out.emplace_back(id, std::move(ct.text));
        }
        return out;
    };
    if (!user_sentence_.LoadAndMigrate(path, tokenize)) {
        return false;
    }
    // Re-write the file with the new vocab_sig and re-tokenized IDs so
    // subsequent loads skip migration.
    SaveUserSentence(path);
    return true;
}

bool Sime::SaveUserSentence(const std::filesystem::path& path) const {
    return user_sentence_.Save(path, vocab_sig_,
        [this](TokenID id) { return TokenText(id); });
}

void Sime::LearnUserSentence(const std::vector<TokenID>& context,
                             const std::vector<TokenID>& sentence) {
    if (!user_sentence_enabled_) {
        return;
    }
#if SIME_WEAVE_OVERLAY
    // Overlay token IDs are not stable across overlay versions; user words belong in L4.
    if (std::any_of(context.begin(), context.end(), Overlay::IsToken) ||
        std::any_of(sentence.begin(), sentence.end(), Overlay::IsToken)) {
        return;
    }
#endif
    user_sentence_.Add(context, sentence);
#if SIME_WEAVE_INCREMENTAL
    BumpGenerationLocked();
#endif
}

#if SIME_WEAVE_OVERLAY
bool Sime::LoadOverlay(const OverlayConfig& config, std::string* error) {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (!ready_) {
        if (error) *error = "engine not ready";
        overlay_.Clear();
#if SIME_WEAVE_INCREMENTAL
        BumpGeneration();
#endif
        return false;
    }
#if SIME_WEAVE_INCREMENTAL
    BumpGeneration();
#endif
    return overlay_.Load(config, dict_, error);
}

void Sime::ClearOverlay() {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    overlay_.Clear();
#if SIME_WEAVE_INCREMENTAL
    BumpGeneration();
#endif
}

bool Sime::SetOverlayLayerEnabled(LexLayer layer, bool enabled) {
    std::lock_guard<std::mutex> lock(decode_mutex_);
#if SIME_WEAVE_INCREMENTAL
    BumpGeneration();
#endif
    return overlay_.SetEnabled(layer, enabled, dict_);
}

void Sime::SetDisplayCasing(DisplayCasing casing) {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    casing_ = casing;
#if SIME_WEAVE_INCREMENTAL
    BumpGeneration();
#endif
}

void Sime::SetDedupByKey(bool on) {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    dedup_by_key_ = on;
#if SIME_WEAVE_INCREMENTAL
    BumpGeneration();
#endif
}

float_t Sime::StepCost(Scorer::Pos pos, TokenID id, Scorer::Pos& next) const {
    if (Overlay::IsToken(id)) {
        next = Scorer::Pos{};  // history break: the next word is scored from the root
        return overlay_.Cost(id);
    }
    return scorer_.ScoreMove(pos, id, next);
}

std::string Sime::DedupKey(const std::string& text) const {
    if (!dedup_by_key_) return text;
    std::string out;
    for (char c : text) {
        if (c == ' ') continue;
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

void Sime::AppendOverlayEdges(std::vector<Node>& net,
                              std::vector<std::vector<Link>>& overlay_edges,
                              std::string_view input) const {
    if (std::all_of(overlay_edges.begin(), overlay_edges.end(),
                    [](const std::vector<Link>& es) { return es.empty(); })) {
        return;
    }
    std::vector<Node> ov(net.size());
    for (std::size_t s = 0; s < overlay_edges.size() && s < ov.size(); ++s) {
        ov[s].es = std::move(overlay_edges[s]);
    }
    ComputeEdgePenalties(ov, input);
    for (std::size_t s = 0; s < ov.size(); ++s) {
        auto& edges = ov[s].es;
        if (edges.empty()) continue;
        // Among overlay edges only: an exact edge drops completions with the same span.
        std::unordered_set<std::size_t> exact_end;
        for (const auto& e : edges)
            if (!e.expansion) exact_end.insert(e.end);
        edges.erase(std::remove_if(edges.begin(), edges.end(), [&](const Link& e) {
                        return e.expansion && exact_end.contains(e.end);
                    }), edges.end());
        PruneNode(edges);
        net[s].es.insert(net[s].es.end(), edges.begin(), edges.end());
    }
}
#endif

std::string Sime::TokenText(TokenID id) const {
    if (id == NotToken || !ready_) {
        return {};
    }
#if SIME_WEAVE_OVERLAY
    if (const auto* t = overlay_.TokenOf(id)) return t->spelling;
#endif
    const char32_t* p = dict_.TokenAt(id);
    if (!p) {
        return {};
    }
    std::u32string u32;
    while (*p) {
        u32.push_back(*p);
        ++p;
    }
    return TextFromU32(u32);
}

void Sime::ComputeEdgePenalties(std::vector<Node>& net,
                                std::string_view input) {
    // English penalty: only "long input + full coverage" English gets
    // the lighter EnglishPenalty (e.g. google on 466453, hello on
    // input "hello"). Everything else (partial-coverage like zg in
    // zgkxjsdx, zgkxjs in zgkxjsdx, or short input full-cover like up
    // / nm / key) gets EnglishPenaltyShort — these are likely
    // coincidental English spellings of CN abbreviations.
    // Expansion penalty: short input gets ExpansionPenaltyShort (lets
    // 87→他说 beat short English exact); long input keeps stricter
    // ExpansionPenalty so abbrev edges don't crowd out the natural
    // full-pinyin path.
    // weave (SIME_WEAVE_SPACE_BOUNDARY): the rule below is evaluated per
    // space-delimited word: full_cover and short_input use the bounds of the
    // word that contains the edge (spaces excluded).
    std::size_t word_begin = 0, word_end = input.size();
    bool short_input = input.size() <= 4;
    float_t expansion_penalty = short_input
        ? ExpansionPenaltyShort : ExpansionPenalty;
    for (auto& col : net) {
        for (auto& edge : col.es) {
#if SIME_WEAVE_SPACE_BOUNDARY
            if (edge.id == SpaceToken) continue;
            word_begin = input.substr(0, edge.start).find_last_of(' ');
            word_begin = word_begin == std::string_view::npos ? 0 : word_begin + 1;
            word_end = input.find(' ', edge.start);
            if (word_end == std::string_view::npos) word_end = input.size();
            short_input = word_end - word_begin <= 4;
            expansion_penalty = short_input
                ? ExpansionPenaltyShort : ExpansionPenalty;
#endif
#if SIME_WEAVE_LITERAL_EDGE
            // Literal edges pay only their own flat penalty (on top of the
            // LM's unknown-token cost added in Process).
            if (edge.id == LiteralToken) {
                // A literal span is letters only, so its length is its
                // letter count.
                edge.penalty = LiteralPenalty
                    + LiteralPerLetter
                        * static_cast<float_t>(edge.end - edge.start);
                continue;
            }
#endif
            if (!edge.pieces || edge.id == NotToken) continue;
            float_t en_pen = 0.0f;
            if (edge.english) {
                bool full_cover =
                    edge.start == word_begin && edge.end == word_end;
#if SIME_WEAVE_PENALTY_RULE == 0
                en_pen = (full_cover && !short_input)
                    ? EnglishPenalty : EnglishPenaltyShort;
#else
                // Scoped rule (weave): whole-input words keep the upstream
                // penalties; an English edge inside longer input pays
                // EnglishPenaltyMixed, or EnglishPenaltyShort when it is at
                // most EnglishShortEdgeLen letters long.
                if (full_cover) {
                    en_pen = short_input
                        ? EnglishPenaltyShort : EnglishPenalty;
                } else if (std::count_if(
                               input.begin() + edge.start,
                               input.begin() + edge.end,
                               [](unsigned char c) {
                                   return (c >= 'a' && c <= 'z')
                                       || (c >= 'A' && c <= 'Z');
                               }) <= static_cast<std::ptrdiff_t>(
                                         EnglishShortEdgeLen)) {
                    en_pen = EnglishPenaltyShort;
                } else {
                    en_pen = EnglishPenaltyMixed;
                }
#endif
            }
            edge.penalty = en_pen
                + (edge.expansion ? expansion_penalty : 0.0f);
        }
    }
}

void Sime::InitNumNet(std::string_view start,
                              std::string_view nums,
                              std::vector<Node>& net,
                              bool expansion) const {
    const std::size_t p = start.size();
    const std::size_t d = nums.size();
    const std::size_t total = p + d;

    net.clear();
    net.resize(total + 2);

    auto emit = [&](std::size_t s, std::size_t new_col,
                    TokenID tid, const char* pieces,
                    bool en = false) {
        net[s].es.push_back({s, new_col, tid, pieces, 0, false, en});
    };

#if SIME_WEAVE_CASE_FOLD
    // English lookups (en) use the case-folded letters; the typed span picks
    // the casing (see ForEachCasedItem). Callers pass suffixes of `start`.
    const std::string en_start = FoldEnKey(start);
    auto en_key = [&](std::size_t s, std::string_view typed) {
        return std::string_view(en_start).substr(s, typed.size());
    };
    auto emit_dat = [&](std::size_t s, Dict::DatType type,
                        std::string_view suffix, std::size_t max_end,
                        bool pinyin, bool en) {
        if (en) {
            ForEachFoldedEnItem(dict_, suffix, en_key(s, suffix),
                                /*prefix=*/false, /*filter=*/true,
                                [&](std::string_view k) {
                                    return dict_.Dat(type).PrefixSearch(k, 512);
                                },
                                [&](std::size_t length, const Dict::Item& item) {
                if (s + length > max_end) return;
                emit(s, s + length, item.id, item.pieces, en);
            });
            return;
        }
        auto results = pinyin
            ? dict_.Dat(type).PrefixSearchPinyin(suffix, 512)
            : dict_.Dat(type).PrefixSearch(suffix, 512);
        for (const auto& r : results) {
            std::size_t new_col = s + r.length;
            if (new_col > max_end) continue;
            auto entry = dict_.GetEntry(type, r.value);
            for (uint32_t i = 0; i < entry.count; ++i) {
                emit(s, new_col, entry.items[i].id, entry.items[i].pieces, en);
            }
        }
    };

    auto expand_dat = [&](std::size_t s, Dict::DatType type,
                          std::string_view tail, std::size_t target_col,
                          std::size_t max_num, bool pinyin, bool en) {
        if (en) {
            ForEachFoldedEnItem(dict_, tail, en_key(s, tail),
                                /*prefix=*/true, /*filter=*/true,
                                [&](std::string_view k) {
                                    return dict_.Dat(type).FindWordsWithPrefix(k, max_num);
                                },
                                [&](std::size_t length, const Dict::Item& item) {
                if (length <= tail.size()) return;
                net[s].es.push_back({s, target_col, item.id, item.pieces,
                                     0, true, en});
            });
            return;
        }
        auto results = pinyin
            ? dict_.Dat(type).FindWordsWithPrefixPinyin(tail, max_num)
            : dict_.Dat(type).FindWordsWithPrefix(tail, max_num);
        for (const auto& r : results) {
            if (!pinyin && r.length <= tail.size()) continue;
            auto entry = dict_.GetEntry(type, r.value);
            for (uint32_t i = 0; i < entry.count; ++i) {
                net[s].es.push_back({s, target_col, entry.items[i].id,
                                     entry.items[i].pieces, 0, true, en});
            }
        }
    };
#else
    auto emit_dat = [&](std::size_t s, Dict::DatType type,
                        std::string_view suffix, std::size_t max_end,
                        bool pinyin, bool en) {
        auto results = pinyin
            ? dict_.Dat(type).PrefixSearchPinyin(suffix, 512)
            : dict_.Dat(type).PrefixSearch(suffix, 512);
        for (const auto& r : results) {
            std::size_t new_col = s + r.length;
            if (new_col > max_end) continue;
            auto entry = dict_.GetEntry(type, r.value);
            for (uint32_t i = 0; i < entry.count; ++i) {
                emit(s, new_col, entry.items[i].id, entry.items[i].pieces, en);
            }
        }
    };

    auto expand_dat = [&](std::size_t s, Dict::DatType type,
                          std::string_view tail, std::size_t target_col,
                          std::size_t max_num, bool pinyin, bool en) {
        auto results = pinyin
            ? dict_.Dat(type).FindWordsWithPrefixPinyin(tail, max_num)
            : dict_.Dat(type).FindWordsWithPrefix(tail, max_num);
        for (const auto& r : results) {
            // For English exact-stem matches PrefixSearch already emits
            // them, so skip duplicates. CN fuzzy walks (q→qi etc.) can
            // hit eow at length == tail.size() — keep those, since
            // PrefixSearchPinyin drops fuzzy states and would miss them.
            if (!pinyin && r.length <= tail.size()) continue;
            auto entry = dict_.GetEntry(type, r.value);
            for (uint32_t i = 0; i < entry.count; ++i) {
                net[s].es.push_back({s, target_col, entry.items[i].id,
                                     entry.items[i].pieces, 0, true, en});
            }
        }
    };
#endif

    // letter_bounds = greedy syllable walk (mirrors InitNet).
    // letter_is_syllable[i] is true iff segment i is a *terminal*
    // (non-extendable) pinyin syllable.
    std::vector<std::size_t> letter_bounds;
    std::vector<bool> letter_is_syllable;
    if (p > 0) {
        letter_bounds.push_back(0);
        std::size_t pos = 0;
        while (pos < p) {
            if (start[pos] == '\'') {
                ++pos;
                letter_bounds.push_back(pos);
                letter_is_syllable.push_back(false);
                continue;
            }
            bool terminal = false;
            std::size_t best = 1;
            for (std::size_t len = std::min(p - pos, std::size_t(6));
                 len >= 2; --len) {
                std::string ps(start.substr(pos, len));
                if (Dict::IsKnownPinyin(ps)) {
                    best = len;
                    terminal = !Dict::IsExtendablePinyin(ps);
                    break;
                }
            }
            pos += best;
            letter_bounds.push_back(pos);
            letter_is_syllable.push_back(terminal);
        }
    }

    // digit_bounds = greedy T9-syllable walk (mirrors letter_bounds).
    // digit_is_syllable[i] is true iff segment i is a *terminal*
    // (non-extendable) T9 syllable.
    std::vector<std::size_t> digit_bounds;
    std::vector<bool> digit_is_syllable;
    if (d > 0) {
        digit_bounds.push_back(0);
        std::size_t pos = 0;
        while (pos < d) {
            if (nums[pos] == '\'') {
                ++pos;
                digit_bounds.push_back(pos);
                digit_is_syllable.push_back(false);
                continue;
            }
            bool terminal = false;
            std::size_t best = 1;
            for (std::size_t len = std::min(d - pos, std::size_t(6));
                 len >= 2; --len) {
                std::string_view span = nums.substr(pos, len);
                if (Dict::IsKnownT9Syllable(span)) {
                    best = len;
                    terminal = !Dict::IsExtendableT9Syllable(span);
                    break;
                }
            }
            pos += best;
            digit_bounds.push_back(pos);
            digit_is_syllable.push_back(terminal);
        }
    }

    for (std::size_t s = 0; s < total; ++s) {
        // === Prefix letter columns ===
        if (s < p) {
            if (start[s] == '\'') {
                net[s].es.push_back({s, s + 1, NotToken});
                continue;
            }

            std::string_view suffix = start.substr(s);
            emit_dat(s, Dict::LetterPinyin, suffix, p, true, false);
            emit_dat(s, Dict::LetterEn, suffix, p, false, true);

            // Cross-boundary: search with letters + digits combined.
            // PrefixSearchT9 now handles letter chars via AdvancePinyin,
            // so a mixed string like "b'9'3" works directly.
            // Only search from the last few letter columns — earlier columns
            // would need impractically long words to span into digit territory.
            if (!nums.empty() && (p - s) <= 6) {
                std::string cross(suffix);
                cross += nums;
#if SIME_WEAVE_CASE_FOLD
                // English: case-folded letters + digits, and the letters as
                // typed when they have capitals (ForEachFoldedEnItem). Digits
                // carry no casing, so every item of a match is emitted.
                std::string en_cross(en_key(s, suffix));
                en_cross += nums;
#endif
                auto cross_emit = [&](Dict::DatType type,
                                      trie::DoubleArray::CharExpander expander,
                                      std::size_t max_num, bool en) {
#if SIME_WEAVE_CASE_FOLD
                    if (en) {
                        ForEachFoldedEnItem(dict_, cross, en_cross,
                                            /*prefix=*/false, /*filter=*/false,
                                            [&](std::string_view k) {
                                                return dict_.Dat(type).PrefixSearchT9(
                                                    k, expander, max_num);
                                            },
                                            [&](std::size_t length, const Dict::Item& item) {
                            const std::size_t new_col = s + length;
                            if (new_col <= p || new_col > total) return;
                            emit(s, new_col, item.id, item.pieces, en);
                        });
                        return;
                    }
                    auto results = dict_.Dat(type).PrefixSearchT9(
                        cross, expander, max_num);
#else
                    auto results = dict_.Dat(type).PrefixSearchT9(
                        cross, expander, max_num);
#endif
                    for (const auto& r : results) {
                        std::size_t new_col = s + r.length;
                        if (new_col <= p) continue;  // already covered by letter-only search
                        if (new_col > total) continue;
                        auto entry = dict_.GetEntry(type, r.value);
                        for (uint32_t i = 0; i < entry.count; ++i) {
                            emit(s, new_col, entry.items[i].id,
                                 entry.items[i].pieces, en);
                        }
                    }
                };
                cross_emit(Dict::LetterPinyin, Dict::NumToLettersLower, 512, false);
                cross_emit(Dict::LetterEn, Dict::NumToLetters, 512, true);
            }
            continue;
        }

        // === Digit columns ===
        const std::size_t dpos = s - p;

        if (nums[dpos] == '\'') {
            net[s].es.push_back({s, s + 1, NotToken});
            continue;
        }

        // T9: expand digits to letters and search letter DATs
        std::string_view num_suffix = nums.substr(dpos);
        auto t9_emit = [&](Dict::DatType type, trie::DoubleArray::CharExpander expander,
                           std::size_t max_num, bool en) {
            auto results = dict_.Dat(type).PrefixSearchT9(
                num_suffix, expander, max_num);
            for (const auto& r : results) {
                std::size_t new_col = s + r.length;
                if (new_col > total) continue;
                auto entry = dict_.GetEntry(type, r.value);
                for (uint32_t i = 0; i < entry.count; ++i) {
                    emit(s, new_col, entry.items[i].id, entry.items[i].pieces, en);
                }
            }
        };
        // Pinyin DAT: lowercase only (no uppercase in pinyin)
        t9_emit(Dict::LetterPinyin, Dict::NumToLettersLower, 512, false);
        // English DAT: both cases
        t9_emit(Dict::LetterEn, Dict::NumToLetters, 512, true);
    }

    // Letter-side boundary expansion (mirrors InitNet): fire FWWPP at
    // every later boundary target whose adjacent left segment is not
    // terminal. English tail completion fires if the suffix contains
    // any non-terminal segment.
    if (expansion && p > 0) {
        for (std::size_t s_idx = 0; s_idx + 1 < letter_bounds.size(); ++s_idx) {
            std::size_t s = letter_bounds[s_idx];
            if (s >= p || start[s] == '\'') continue;

            bool saw_incomplete = false;
            for (std::size_t bi = s_idx + 1; bi < letter_bounds.size(); ++bi) {
                if (!letter_is_syllable[bi - 1]) saw_incomplete = true;
                if (!saw_incomplete) continue;
                std::size_t target = letter_bounds[bi];
                auto prefix = start.substr(s, target - s);
                expand_dat(s, Dict::LetterPinyin, prefix, target, 512, true, false);
            }
            if (saw_incomplete) {
                std::string_view suffix = start.substr(s);
                expand_dat(s, Dict::LetterEn, suffix, p, 1024, false, true);
            }
        }
    }

    // Digit-side boundary expansion (T9 mirror of letter-side). For each
    // digit boundary s, walk AdvanceT9States forward; latch on first
    // non-terminal segment; emit completions at every later boundary
    // target. Drops the old per-column outer loop and 8-char hard cap.
    if (expansion && d > 0) {
        for (std::size_t s_bi = 0; s_bi + 1 < digit_bounds.size(); ++s_bi) {
            std::size_t s_dpos = digit_bounds[s_bi];
            std::size_t s = p + s_dpos;
            if (s_dpos >= d || nums[s_dpos] == '\'') continue;

            auto py_states = dict_.Dat(Dict::LetterPinyin).StartPinyinStates();
            auto en_states = dict_.Dat(Dict::LetterEn).StartPinyinStates();
            bool saw_incomplete = false;
            std::size_t next_bi = s_bi + 1;

            for (std::size_t pos = s_dpos; pos < d; ++pos) {
                auto ch = static_cast<uint8_t>(nums[pos]);
                if (!py_states.empty()) {
                    dict_.Dat(Dict::LetterPinyin).AdvanceT9States(
                        py_states, ch, Dict::NumToLettersLower);
                }
                if (!en_states.empty()) {
                    dict_.Dat(Dict::LetterEn).AdvanceT9States(
                        en_states, ch, Dict::NumToLetters);
                }
                if (py_states.empty() && en_states.empty()) break;

                std::size_t target_dpos = pos + 1;
                if (next_bi >= digit_bounds.size()
                    || digit_bounds[next_bi] != target_dpos) continue;

                std::size_t seg_idx = next_bi - 1;
                if (!digit_is_syllable[seg_idx]) saw_incomplete = true;
                ++next_bi;
                if (!saw_incomplete) continue;

                std::size_t k = target_dpos - s_dpos;
                auto emit_cn = [&](const std::vector<trie::DoubleArray::PinyinState>& sts) {
                    auto comps = dict_.Dat(Dict::LetterPinyin)
                        .CollectCompletionsPinyin(sts, k, 1024, /*stop_at_sep=*/true);
                    for (const auto& r : comps) {
                        auto entry = dict_.GetEntry(Dict::LetterPinyin, r.value);
                        for (uint32_t i = 0; i < entry.count; ++i) {
                            net[s].es.push_back({s, s + k, entry.items[i].id,
                                                 entry.items[i].pieces, 0, true,
                                                 false});
                        }
                    }
                };
                auto emit_en = [&](const std::vector<trie::DoubleArray::PinyinState>& sts) {
                    auto comps = dict_.Dat(Dict::LetterEn)
                        .CollectCompletionsPinyin(sts, k, 1024, /*stop_at_sep=*/true);
                    for (const auto& r : comps) {
                        if (r.length <= k) continue;
                        auto entry = dict_.GetEntry(Dict::LetterEn, r.value);
                        for (uint32_t i = 0; i < entry.count; ++i) {
                            net[s].es.push_back({s, s + k, entry.items[i].id,
                                                 entry.items[i].pieces, 0, true,
                                                 true});
                        }
                    }
                };
                emit_cn(py_states);
                emit_en(en_states);
            }
        }
    }

    // Two-track per-bucket tier filter: CN edges (!english) and English
    // edges (english) are filtered on independent tracks, so an exact
    // edge on one track doesn't suppress edges on the other.
    //   tier 0: !expansion (exact)
    //   tier 1: expansion (abbrev / completion)
    // Keeps both CN exact and English exact alive when they share a
    // bucket (e.g. 466453 → 攻克 + google), and keeps CN expansion
    // alongside English exact (e.g. 539 → 可以 + key).
    auto tier_of = [](const Link& e) -> uint8_t {
        if (IsNoToken(e.id)) return 0;
        return e.expansion ? 1 : 0;
    };
    std::vector<uint8_t> best_py(total + 2, 0xFF);
    std::vector<uint8_t> best_en(total + 2, 0xFF);
    for (std::size_t i = 0; i < total; ++i) {
        auto& edges = net[i].es;
        if (edges.empty()) continue;
        for (const auto& e : edges) {
            uint8_t t = tier_of(e);
            auto& best = e.english ? best_en : best_py;
            if (t < best[e.end]) best[e.end] = t;
        }
        edges.erase(std::remove_if(edges.begin(), edges.end(),
            [&](const Link& e) {
                if (IsNoToken(e.id)) return false;
                const auto& best = e.english ? best_en : best_py;
                return tier_of(e) > best[e.end];
            }), edges.end());
        for (const auto& e : edges) {
            best_py[e.end] = 0xFF;
            best_en[e.end] = 0xFF;
        }
    }

    net[total].es.push_back({total, total + 1, NotToken});
}

std::string Sime::AbbreviatePieces(const char* full_pieces,
                                   std::string_view input) {
    // Parse syllables from full_pieces.
    struct Syl { const char* begin; std::size_t len; };
    std::vector<Syl> syls;
    for (const char* p = full_pieces; *p; ) {
        const char* s = p;
        while (*p && *p != '\'') ++p;
        if (p > s) syls.push_back({s, static_cast<std::size_t>(p - s)});
        if (*p == '\'') ++p;
    }
    if (syls.empty()) return full_pieces;

#if SIME_WEAVE_CASE_FOLD
    // weave case fold: a lowercase-en dictionary stores English pieces in
    // lowercase, so typed letters match piece letters ignoring ASCII case,
    // and the result below keeps the typed casing. Only English edges can
    // differ in case here: the pinyin DAT is lowercase and its search is
    // case-sensitive, so a pinyin edge never covers a typed capital.
    auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    auto caseMatch = [&](char ic, char sc) {
        return ic != sc && lower(ic) == lower(sc);
    };
#endif
    // Check if input char matches a syllable letter.
    auto charMatch = [&](char ic, char sc) -> bool {
        if (ic == sc) return true;
#if SIME_WEAVE_CASE_FOLD
        if (caseMatch(ic, sc)) return true;
#endif
        if (ic >= '2' && ic <= '9') {
            const char* letters = Dict::NumToLettersLower(
                static_cast<uint8_t>(ic));
            for (const char* l = letters; *l; ++l) {
                if (*l == sc) return true;
            }
        }
        return false;
    };

    // Find the max prefix length of syllable syl that matches input
    // starting at ipos. Returns 0 if first char doesn't match.
    auto maxMatch = [&](const Syl& syl, std::size_t ipos) -> std::size_t {
        std::size_t matched = 0;
        for (std::size_t k = 0; k < syl.len && ipos + k < input.size()
                 && input[ipos + k] != '\''; ++k) {
            if (!charMatch(input[ipos + k], syl.begin[k])) break;
            ++matched;
        }
        return matched;
    };

    // Recursive search: for each syllable, try lengths 1..max (prefer
    // shorter = more abbreviated). Return true if all syllables can be
    // covered consuming all input non-separator chars.
    std::vector<std::size_t> chosen(syls.size(), 0);
    std::vector<std::size_t> chosen_pos(syls.size(), 0);  // input offset of each

    std::function<bool(std::size_t, std::size_t)> solve =
        [&](std::size_t si, std::size_t ipos) -> bool {
        // Skip input separators.
        while (ipos < input.size() && input[ipos] == '\'') ++ipos;
        if (si == syls.size()) return ipos >= input.size();
        if (ipos >= input.size()) return false;
        std::size_t mm = maxMatch(syls[si], ipos);
        if (mm == 0) return false;
        for (std::size_t len = 1; len <= mm; ++len) {
            chosen[si] = len;
            chosen_pos[si] = ipos;
            // Skip any input separator after the consumed digits.
            std::size_t next_ipos = ipos + len;
            if (solve(si + 1, next_ipos)) return true;
        }
        return false;
    };

    if (!solve(0, 0)) {
        // Fallback: greedy (original behavior) — shouldn't normally happen.
        return full_pieces;
    }

    // Build result from chosen lengths.
    std::string result;
    for (std::size_t i = 0; i < syls.size(); ++i) {
        if (i > 0) result += '\'';
#if SIME_WEAVE_CASE_FOLD
        // A letter matched ignoring case is written as typed, so the units
        // keep the typed casing as upstream's cased pieces did.
        for (std::size_t k = 0; k < chosen[i]; ++k) {
            const char ic = input[chosen_pos[i] + k];
            result += caseMatch(ic, syls[i].begin[k]) ? ic : syls[i].begin[k];
        }
#else
        result.append(syls[i].begin, chosen[i]);
#endif
    }
    return result;
}

std::string Sime::ExtractUnits(const std::vector<Link>& path,
                               std::string_view input) {
    std::string py;
    for (const auto& link : path) {
        if (IsNoToken(link.id)) continue;
#if SIME_WEAVE_LITERAL_EDGE
        if (link.id == LiteralToken) {
            if (!py.empty()) py += '\'';
            py += input.substr(link.start, link.end - link.start);
            continue;
        }
#endif
        if (!link.pieces || link.pieces[0] == '\0') continue;
        if (!py.empty()) py += '\'';
        py += AbbreviatePieces(link.pieces,
                               input.substr(link.start, link.end - link.start));
    }
    return py;
}

std::string Sime::ExtractText(const std::vector<Link>& path,
                              std::string_view input) const {
    std::u32string u32;
    for (const auto& link : path) {
        u32 += ToText(link, input);
    }
    return TextFromU32(u32);
}

std::string Sime::TextFromU32(std::u32string& u32) {
    for (auto& ch : u32) {
        if (ch == 0x2581) ch = U' ';
    }
    return ustr::FromU32(u32);
}

std::vector<TokenID> Sime::ExtractTokens(
    const std::vector<Link>& path) const {
    std::vector<TokenID> ids;
    for (const auto& link : path) {
        if (IsNoToken(link.id)) continue;
#if SIME_WEAVE_LITERAL_EDGE
        // A literal edge is no vocabulary token: keep it out of the LM context.
        if (link.id == LiteralToken) continue;
#endif
        ids.push_back(link.id);
    }
    return ids;
}

void Sime::FillSegment(const Link& link, std::string_view input, Segment& seg) const {
    std::u32string shown = ToText(link, input);
    seg.text = TextFromU32(shown);
    const std::string_view typed = link.end <= input.size() && link.start <= link.end
        ? input.substr(link.start, link.end - link.start) : std::string_view();
    seg.expansion = link.expansion;
#if SIME_WEAVE_LITERAL_EDGE
    if (link.id == LiteralToken) {
        seg.kind = SegmentKind::kLiteral;
        seg.typed_text = seg.canonical_text = seg.text;
        seg.layer = seg.sources = 0;
        seg.token = NotToken;
        seg.units.assign(typed);  // as ExtractUnits
        return;
    }
#endif
    seg.kind = link.english ? SegmentKind::kEnglish : SegmentKind::kPinyin;
    if (link.pieces && link.pieces[0] != '\0') seg.units = AbbreviatePieces(link.pieces, typed);
    std::u32string canonical;
#if SIME_WEAVE_OVERLAY
    if (const auto* t = overlay_.TokenOf(link.id)) {
        canonical = t->spelling32;
        std::u32string as_typed = AsTyped(canonical, typed);
        seg.canonical_text = TextFromU32(canonical);
        seg.typed_text = TextFromU32(as_typed);
        seg.layer = 1u << (1 + t->slot);
        seg.sources = t->sources << 1;
        seg.token = NotToken;
        return;
    }
#endif
    if (const char32_t* chars = dict_.TokenAt(link.id)) {
        for (std::size_t i = 0; chars[i] != 0 && i < 64; ++i) canonical.push_back(chars[i]);
    }
    std::u32string as_typed = AsTyped(canonical, typed);  // ToText under kAsTyped
    seg.layer = kSourceBase;
    seg.sources = kSourceBase;
    seg.token = link.id;
#if SIME_WEAVE_OVERLAY
    if (!overlay_.Empty()) {
        if (const auto* c = overlay_.CanonicalForL0(link.id)) canonical = *c;
        seg.sources |= overlay_.L0Sources(link.id) << 1;
    }
#endif
    seg.canonical_text = TextFromU32(canonical);
    seg.typed_text = TextFromU32(as_typed);
}

std::vector<Segment> Sime::ExtractSegments(const std::vector<Link>& path, std::string_view input,
                                           const std::vector<std::size_t>& raw_off,
                                           std::size_t raw_size) const {
    auto raw = [&](std::size_t b) { return b >= input.size() ? raw_size : raw_off[b]; };
    std::vector<Segment> segs;
    for (const auto& link : path) {
        if (IsNoToken(link.id)) {  // apostrophe / typed space: part of the segment before it
            if (!segs.empty()) segs.back().end = raw(link.end);
            continue;
        }
        Segment seg;
        seg.start = segs.empty() ? 0 : segs.back().end;
        seg.end = raw(link.end);
        FillSegment(link, input, seg);
        segs.push_back(std::move(seg));
    }
    return segs;
}

std::vector<DecodeResult> Sime::DecodeNumSentence(
    std::string_view nums,
    std::string_view start,
    std::size_t extra) const {
    return DecodeNumSentence(nums, start, {}, extra);
}

std::vector<DecodeResult> Sime::DecodeNumSentence(
    std::string_view nums,
    std::string_view start,
    const std::vector<TokenID>& context,
    std::size_t extra) const {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    std::vector<DecodeResult> results;
    if (!ready_) return results;
    MaybeTrimCaches();
    for (char c : nums) {
        if (c == '\'') continue;
        if (c < '2' || c > '9') return results;
    }
    if (nums.empty() && start.empty()) return results;

    const std::size_t p = start.size();
    const std::size_t d = nums.size();
    const std::size_t total = p + d;
    std::string combined_input(start);
    combined_input += nums;

    std::vector<Node> net;
    InitNumNet(start, nums, net, /*expansion=*/true);
    ComputeEdgePenalties(net, combined_input);
    for (auto& col : net) PruneNode(col.es);

    for (auto& col : net) col.states.SetMaxTop(BeamSize);
    State init = InitialState(context);
    net[0].states.Insert(init);
    Process(net);

    // See DecodeSentence: Layer 1 dedups with a local set so beam members
    // that don't make the 1+extra cut remain eligible for Layer 2.
    std::unordered_set<std::string> dedup;
    const float_t penalty_per_unit =
        std::abs(scorer_.UnknownPenalty()) / DistancePenalty;

    // === Layer 1: Full sentence N-best ===
    // (frag_penalty is already in beam scores via edge.penalty)
    {
        const auto tail = net[total + 1].states.GetStates();
        const std::size_t scan = std::min<std::size_t>(BeamSize, tail.size());
        const std::size_t full_cnt = start.size() + d;
        std::vector<DecodeResult> l1;
        l1.reserve(scan);
        std::unordered_set<std::string> l1_seen;
        for (std::size_t rank = 0; rank < scan; ++rank) {
            auto path = Backtrace(tail[rank], total + 1);
            std::string text = ExtractText(path);
            if (text.empty() || !l1_seen.insert(text).second) continue;
            std::string py = ExtractUnits(path, combined_input);

            l1.push_back({std::move(text), std::move(py),
                          ExtractTokens(path),
                          -tail[rank].score, full_cnt});
        }
        std::sort(l1.begin(), l1.end(),
                  [](const DecodeResult& a, const DecodeResult& b) {
                      return a.score > b.score;
                  });
        RerankWithGru(l1, gru_.get(), true);
        const std::size_t full_limit = 1 + extra;
        for (std::size_t i = 0; i < l1.size() && results.size() < full_limit;
             ++i) {
            dedup.insert(l1[i].text);
            results.push_back(std::move(l1[i]));
        }
    }

    // === Layer 2: unigram alternatives at column 0 ===
    // All entries (exact + expansion) compete on score; penalty is
    // already in the score so no separate exact/abbrev tiering.
    const std::size_t l2_start = results.size();
    std::vector<DecodeResult> best_l2;
    std::unordered_map<std::string, std::size_t> l2_index_by_text;

    // Skip leading apostrophe-only columns (those carry only a NotToken
    // sentinel edge). Without this, an input that starts with `'` (e.g.
    // a leftover after partial commit) leaves Layer 2 with zero candidates.
    std::size_t l2_col = 0;
    while (l2_col < total && net[l2_col].es.size() == 1 &&
           net[l2_col].es[0].id == NotToken) {
        ++l2_col;
    }
    for (const auto& edge : net[l2_col].es) {
        if (edge.id == NotToken) continue;

        std::u32string text_u32 = ToText(edge);
        if (text_u32.empty()) continue;

        std::string text_utf8 = TextFromU32(text_u32);
        if (dedup.contains(text_utf8)) continue;

        std::size_t distance = (total > edge.end) ? (total - edge.end) : 0;
        float_t dist_penalty =
            static_cast<float_t>(distance) * penalty_per_unit;

        auto slice = std::string_view(combined_input).substr(
            edge.start, edge.end - edge.start);

        // Use edge.penalty (English + expansion, set by
        // ComputeEdgePenalties) so L2 score sits on the same coordinate
        // system as L1: -(LM unigram + edge.penalty) - dist_penalty.
        Scorer::Pos epos{};
        Scorer::Pos enext{};
        float_t score = -scorer_.ScoreMove(epos, edge.id, enext)
                        - dist_penalty
                        - edge.penalty;

        std::string edge_py = edge.pieces
            ? AbbreviatePieces(edge.pieces, slice)
            : "";
        std::size_t cnt = edge.end;
        PushBestLayer2Entry(
            best_l2,
            l2_index_by_text,
            {std::move(text_utf8), std::move(edge_py),
             ExtractTokens({edge}), score, cnt});
    }

    for (auto& entry : best_l2) {
        results.push_back(std::move(entry));
    }
    auto by_score = [](const DecodeResult& a, const DecodeResult& b) {
        return a.score > b.score;
    };
    // Sort the L2 tail (everything after the L1 sentence head) by score —
    // exact and expansion entries compete on equal footing.
    std::sort(results.begin() + l2_start, results.end(), by_score);

    return results;
}

std::vector<DecodeResult> Sime::DecodeNumStr(
    std::string_view nums,
    std::string_view start,
    std::size_t num) const {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    std::vector<DecodeResult> results;
    if (!ready_) return results;
    MaybeTrimCaches();
    for (char c : nums) {
        if (c == '\'') continue;
        if (c < '2' || c > '9') return results;
    }
    if (nums.empty() && start.empty()) return results;

    const std::size_t max_top = num == 0 ? 1 : num;
    const std::size_t total = start.size() + nums.size();
    std::string combined_input(start);
    combined_input += nums;

    std::vector<Node> net;
    InitNumNet(start, nums, net, /*expansion=*/false);
    ComputeEdgePenalties(net, combined_input);
    for (auto& col : net) PruneNode(col.es);

    for (auto& col : net) col.states.SetMaxTop(BeamSize);
    State init = InitialState();
    net[0].states.Insert(init);
    Process(net);

    auto tail_states = net.back().states.GetStates();
    std::unordered_set<std::string> dedup;
    for (std::size_t rank = 0;
         rank < tail_states.size() && results.size() < max_top; ++rank) {
        auto path = Backtrace(tail_states[rank], total + 1);
        std::string text = ExtractText(path);
        if (text.empty() || !dedup.insert(text).second) continue;
        std::string py = ExtractUnits(path, combined_input);
        results.push_back({std::move(text), std::move(py),
                           ExtractTokens(path),
                           -tail_states[rank].score,
                           start.size() + nums.size()});
    }

    return results;
}

namespace {

// raw_off (optional, weave): raw_off[i] = index in `input` of the character
// that produced byte i of the result.
std::string NormalizeInput(std::string_view input,
                           std::vector<std::size_t>* raw_off = nullptr) {
    std::string lower;
    lower.reserve(input.size());
    for (std::size_t raw = 0; raw < input.size(); ++raw) {
        const char c = input[raw];
        if (c == '\'') {
            lower.push_back(c);
#if SIME_WEAVE_SPACE_BOUNDARY
        } else if (c == ' ') {
            // weave: a typed space stays one byte; InitNet makes it a hard
            // boundary (no edge crosses it, no text, no LM token).
            lower.push_back(c);
#else
        } else if (c == ' ') {
            // Space → ▁ (U+2581, UTF-8: E2 96 81)
            lower.push_back(static_cast<char>(0xE2));
            lower.push_back(static_cast<char>(0x96));
            lower.push_back(static_cast<char>(0x81));
#endif
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            lower.push_back(c);
        }
        if (raw_off) raw_off->resize(lower.size(), raw);
    }
    return lower;
}

} // namespace

std::vector<DecodeResult> Sime::DecodeStr(
    std::string_view input,
    std::size_t num) const {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    std::vector<DecodeResult> results;
    if (!ready_ || input.empty()) return results;
    MaybeTrimCaches();

    std::string lower = NormalizeInput(input);
    if (lower.empty()) return results;

    std::vector<Node> net;
#if SIME_WEAVE_OVERLAY
    std::vector<std::vector<Link>> ov_edges;
    InitNet(lower, net, /*expansion=*/false, &ov_edges);
#else
    InitNet(lower, net, /*expansion=*/false);
#endif
    ComputeEdgePenalties(net, lower);
    for (auto& col : net) PruneNode(col.es);
#if SIME_WEAVE_OVERLAY
    AppendOverlayEdges(net, ov_edges, lower);
#endif

    const std::size_t max_top = num == 0 ? 1 : num;
    for (auto& col : net) col.states.SetMaxTop(BeamSize);
    State init = InitialState();
    net[0].states.Insert(init);
    Process(net);

    const auto tail_states = net.back().states.GetStates();
    std::unordered_set<std::string> dedup;
    for (std::size_t rank = 0;
         rank < tail_states.size() && results.size() < max_top; ++rank) {
        auto path = Backtrace(tail_states[rank], net.size() - 1);
        if (path.empty()) continue;
        std::string text = ExtractText(path, lower);
#if SIME_WEAVE_OVERLAY
        if (text.empty() || !dedup.insert(DedupKey(text)).second) continue;
#else
        if (text.empty() || !dedup.insert(text).second) continue;
#endif
        std::string py = ExtractUnits(path, lower);
        results.push_back({std::move(text), std::move(py),
                           ExtractTokens(path),
                           -tail_states[rank].score, input.size()});
    }
    return results;
}

void Sime::InitNet(std::string_view input,
                      std::vector<Node>& net,
                      bool expansion
#if SIME_WEAVE_OVERLAY
                      , std::vector<std::vector<Link>>* overlay_edges
#endif
                      ) const {
    const std::size_t total = input.size();
    net.clear();
    net.resize(total + 2);
#if SIME_WEAVE_OVERLAY
    // weave overlay: matching always folds case (D5); the edges go to *overlay_edges, which
    // AppendOverlayEdges adds after this net's own filter and pruning.
    const bool overlay_on = overlay_edges && overlay_.HasEdges();
    const std::string ov_input = overlay_on ? FoldAsciiLower(input) : std::string();
    if (overlay_edges) {
        overlay_edges->assign(total + 2, {});
    }
#endif
#if SIME_WEAVE_CASE_FOLD
    const std::string en_input = FoldEnKey(input);
#endif
#if SIME_WEAVE_SPACE_BOUNDARY
    // weave: word_end[i] = index of the first typed space at or after i, so no
    // edge starting in a word reaches past it.
    std::vector<std::size_t> word_end(total + 1, total);
    for (std::size_t i = total; i-- > 0;) {
        word_end[i] = input[i] == ' ' ? i : word_end[i + 1];
    }
    auto word_len = [&](std::size_t s) { return word_end[s] - s; };
    auto is_sep = [](char c) { return c == '\'' || c == ' '; };
#else
    auto word_len = [&](std::size_t s) { return total - s; };
    auto is_sep = [](char c) { return c == '\''; };
#endif

    auto emit = [&](std::size_t s, std::size_t new_col,
                    TokenID tid, const char* pieces,
                    bool en = false) {
        net[s].es.push_back({s, new_col, tid, pieces, 0, false, en});
    };

    // Greedy syllable walk. seg_is_syllable[i] is true iff segment i is a
    // *terminal* (non-extendable) known pinyin syllable. Extendable
    // syllables like "xu" (→ xue/xun/xuan) are flagged false so the
    // expansion gate fires through them — the user may have abbreviated a
    // longer syllable to its prefix. best=1 fallback and apostrophe
    // segments are also incomplete.
    std::vector<std::size_t> seg_bounds;
    std::vector<bool> seg_is_syllable;
    {
        seg_bounds.push_back(0);
        std::size_t pos = 0;
        while (pos < total) {
            if (is_sep(input[pos])) {
                ++pos;
                seg_bounds.push_back(pos);
                seg_is_syllable.push_back(false);
                continue;
            }
            bool terminal = false;
            std::size_t best = 1;
            for (std::size_t len = std::min(total - pos, std::size_t(6));
                 len >= 2; --len) {
                std::string p(input.substr(pos, len));
                if (Dict::IsKnownPinyin(p)) {
                    best = len;
                    terminal = !Dict::IsExtendablePinyin(p);
                    break;
                }
            }
            pos += best;
            seg_bounds.push_back(pos);
            seg_is_syllable.push_back(terminal);
        }
    }

    for (std::size_t s = 0; s < total; ++s) {
        if (is_sep(input[s])) {
#if SIME_WEAVE_SPACE_BOUNDARY
            // Apostrophe keeps upstream's NotToken (LM reset); a typed space
            // is SpaceToken, which Process passes through without touching
            // the LM state.
            net[s].es.push_back({s, s + 1, input[s] == ' ' ? SpaceToken : NotToken});
#else
            net[s].es.push_back({s, s + 1, NotToken});
#endif
            continue;
        }

        std::string_view suffix = input.substr(s, word_len(s));

        auto emit_results = [&](const std::vector<trie::SearchResult>& results,
                                Dict::DatType type, bool en) {
            for (const auto& r : results) {
                auto entry = dict_.GetEntry(type, r.value);
                for (uint32_t i = 0; i < entry.count; ++i) {
                    emit(s, s + r.length, entry.items[i].id,
                         entry.items[i].pieces, en);
                }
            }
        };

        emit_results(
            dict_.Dat(Dict::LetterPinyin).PrefixSearchPinyin(suffix, 512, 2),
            Dict::LetterPinyin, false);
#if SIME_WEAVE_CASE_FOLD
        ForEachFoldedEnItem(dict_, suffix,
                            std::string_view(en_input).substr(s, word_len(s)),
                            /*prefix=*/false, /*filter=*/true,
                            [&](std::string_view k) {
                                return dict_.Dat(Dict::LetterEn).PrefixSearch(k, 512);
                            },
                            [&](std::size_t length, const Dict::Item& item) {
            emit(s, s + length, item.id, item.pieces, true);
        });
#else
        emit_results(
            dict_.Dat(Dict::LetterEn).PrefixSearch(suffix, 512),
            Dict::LetterEn, true);
#endif
#if SIME_WEAVE_OVERLAY
        if (overlay_on) {
            for (const auto& r : overlay_.Dat().PrefixSearch(
                     std::string_view(ov_input).substr(s, word_len(s)), 512)) {
                for (std::uint32_t t : overlay_.TokensAt(r.value)) {
                    (*overlay_edges)[s].push_back({s, s + r.length, Overlay::kTokenBase + t,
                                                   overlay_.At(t).typed.c_str(), 0, false, true});
                }
            }
        }
#endif
    }

    // Boundary-driven expansion: walk forward; once any segment in the
    // span is incomplete (non-terminal pinyin or best=1 fallback), fire
    // FWWPP at every subsequent boundary target. Latching catches
    // "incomplete-then-terminal" spans like "qchuang" → 起床, where the
    // first seg (q) is incomplete but the trailing seg (chuang) is
    // terminal — without the latch, PSPinyin can't reach 起床 because
    // it drops fuzzy states. English tail completion fires per boundary
    // if any non-terminal segment remains in the suffix.
    if (expansion) {
        for (std::size_t s_idx = 0; s_idx + 1 < seg_bounds.size(); ++s_idx) {
            std::size_t s = seg_bounds[s_idx];
            if (s >= total || is_sep(input[s])) continue;

            bool saw_incomplete = false;
            for (std::size_t bi = s_idx + 1; bi < seg_bounds.size(); ++bi) {
#if SIME_WEAVE_SPACE_BOUNDARY
                if (input[seg_bounds[bi] - 1] == ' ') break;  // never cross a typed space
#endif
                if (!seg_is_syllable[bi - 1]) saw_incomplete = true;
                if (!saw_incomplete) continue;
                std::size_t target = seg_bounds[bi];
                auto raw_prefix = input.substr(s, target - s);
                std::string prefix;
                prefix.reserve(raw_prefix.size());
                for (char c : raw_prefix) {
                    if (c != '\'') prefix.push_back(c);
                }
                auto results = dict_.Dat(Dict::LetterPinyin)
                    .FindWordsWithPrefixPinyin(prefix, 512, 2);
                for (const auto& r : results) {
                    auto entry = dict_.GetEntry(Dict::LetterPinyin, r.value);
                    for (uint32_t i = 0; i < entry.count; ++i) {
                        net[s].es.push_back({s, target, entry.items[i].id,
                                             entry.items[i].pieces, 0, true,
                                             false});
                    }
                }
            }
            if (saw_incomplete) {
                std::string_view suffix = input.substr(s, word_len(s));
#if SIME_WEAVE_CASE_FOLD
                ForEachFoldedEnItem(dict_, suffix,
                                    std::string_view(en_input).substr(s, word_len(s)),
                                    /*prefix=*/true, /*filter=*/true,
                                    [&](std::string_view k) {
                                        return dict_.Dat(Dict::LetterEn)
                                            .FindWordsWithPrefix(k, 1024);
                                    },
                                    [&](std::size_t length, const Dict::Item& item) {
                    if (length <= suffix.size()) return;
                    net[s].es.push_back({s, s + suffix.size(), item.id,
                                         item.pieces, 0, true, true});
                });
#else
                auto results = dict_.Dat(Dict::LetterEn)
                    .FindWordsWithPrefix(suffix, 1024);
                for (const auto& r : results) {
                    if (r.length <= suffix.size()) continue;
                    auto entry = dict_.GetEntry(Dict::LetterEn, r.value);
                    for (uint32_t i = 0; i < entry.count; ++i) {
                        net[s].es.push_back({s, s + suffix.size(),
                                             entry.items[i].id,
                                             entry.items[i].pieces, 0, true,
                                             true});
                    }
                }
#endif
#if SIME_WEAVE_OVERLAY
                // Overlay completions, where L0 English completions fire.
                if (overlay_on) {
                    for (const auto& r : overlay_.Dat().FindWordsWithPrefix(
                             std::string_view(ov_input).substr(s, suffix.size()), 1024)) {
                        if (r.length <= suffix.size()) continue;
                        for (std::uint32_t t : overlay_.TokensAt(r.value)) {
                            (*overlay_edges)[s].push_back({s, s + suffix.size(),
                                                           Overlay::kTokenBase + t,
                                                           overlay_.At(t).typed.c_str(), 0, true,
                                                           true});
                        }
                    }
                }
#endif
            }
        }
    }

#if SIME_WEAVE_LITERAL_EDGE
    // weave literal edges: one per span [s, e) of ASCII letters with
    // e - s >= LiteralMinLen, at every column s where a dictionary (pinyin
    // or English) edge starts. Text = the typed letters; cost = the LM's
    // unknown-token cost + LiteralPenalty + LiteralPerLetter x letters
    // (ComputeEdgePenalties). With SIME_WEAVE_LITERAL_NONPINYIN, spans that
    // segment fully into complete pinyin syllables get none. English track,
    // lowest tier (below).
    for (std::size_t s = 0; s < total; ++s) {
        auto& edges = net[s].es;
        if (std::none_of(edges.begin(), edges.end(),
                         [](const Link& e) { return !IsNoToken(e.id); })) {
            continue;
        }
#if SIME_WEAVE_LITERAL_NONPINYIN
        // full_py[k]: input[s, s + k) segments fully into complete pinyin
        // syllables (Dict::IsKnownPinyin, 1-6 letters each, lowercased).
        std::vector<bool> full_py(1, true);
#endif
        for (std::size_t e = s; e < total; ++e) {
            const char c = input[e];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) break;
#if SIME_WEAVE_LITERAL_NONPINYIN
            const std::size_t k = e + 1 - s;
            bool covered = false;
            for (std::size_t len = 1; len <= std::min(k, std::size_t(6)) && !covered;
                 ++len) {
                if (!full_py[k - len]) continue;
                std::string syl(input.substr(e + 1 - len, len));
                for (char& ch : syl) {
                    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
                }
                covered = Dict::IsKnownPinyin(syl);
            }
            full_py.push_back(covered);
            if (covered) continue;  // pure pinyin span: no literal edge
#endif
            if (e + 1 - s >= LiteralMinLen) {
                edges.push_back({s, e + 1, LiteralToken, nullptr,
                                 LiteralPenalty, false, true});
            }
        }
    }
#endif

    // Two-track per-bucket tier filter: CN edges (!english) and English
    // edges (english) are filtered on independent tracks, so an exact
    // edge on one track doesn't suppress edges on the other.
    //   tier 0: !expansion (exact)
    //   tier 1: expansion (abbrev / completion)
    // weave (SIME_WEAVE_LITERAL_EDGE): tier 2: literal edge, so it never
    // suppresses a dictionary edge and is dropped when a dictionary English
    // edge covers the same span.
    auto tier_of = [](const Link& e) -> uint8_t {
        if (IsNoToken(e.id)) return 0;
#if SIME_WEAVE_LITERAL_EDGE
        if (e.id == LiteralToken) return 2;
#endif
        return e.expansion ? 1 : 0;
    };
    std::vector<uint8_t> best_py(total + 2, 0xFF);
    std::vector<uint8_t> best_en(total + 2, 0xFF);
    for (std::size_t i = 0; i < total; ++i) {
        auto& edges = net[i].es;
        if (edges.empty()) continue;
        for (const auto& e : edges) {
            uint8_t t = tier_of(e);
            auto& best = e.english ? best_en : best_py;
            if (t < best[e.end]) best[e.end] = t;
        }
        edges.erase(std::remove_if(edges.begin(), edges.end(),
            [&](const Link& e) {
                if (IsNoToken(e.id)) return false;
                const auto& best = e.english ? best_en : best_py;
                return tier_of(e) > best[e.end];
            }), edges.end());
        for (const auto& e : edges) {
            best_py[e.end] = 0xFF;
            best_en[e.end] = 0xFF;
        }
    }

    net[total].es.push_back({total, total + 1, NotToken});
}

void Sime::PruneNode(std::vector<Link>& edges,
                     std::unordered_map<TokenID, float_t>* score_cache) const {
    if (edges.size() <= NodeSize) return;

    // Group by span = end - start (bounded by max word length ≤ ~12).
    std::size_t max_span = 0;
    for (const auto& e : edges) {
        std::size_t span = e.end - e.start;
        if (span > max_span) max_span = span;
    }
    std::vector<std::vector<std::size_t>> groups(max_span + 1);
    for (std::size_t i = 0; i < edges.size(); ++i) {
        groups[edges[i].end - edges[i].start].push_back(i);
    }

    auto get_score = [&](TokenID id) -> float_t {
        if (IsNoToken(id)) return 0.0;
#if SIME_WEAVE_OVERLAY
        if (Overlay::IsToken(id)) return overlay_.Cost(id);
#endif
        if (score_cache) {
            auto it = score_cache->find(id);
            if (it != score_cache->end()) return it->second;
            Scorer::Pos ppos{};
            Scorer::Pos pnext{};
            float_t s = scorer_.ScoreMove(ppos, id, pnext);
            score_cache->emplace(id, s);
            return s;
        }
        Scorer::Pos ppos{};
        Scorer::Pos pnext{};
        return scorer_.ScoreMove(ppos, id, pnext);
    };

    std::vector<Link> pruned;
    pruned.reserve(edges.size());

    for (auto& indices : groups) {
        if (indices.empty()) continue;
        if (indices.size() <= NodeSize) {
            for (auto idx : indices) pruned.push_back(edges[idx]);
            continue;
        }

        // Single-key sort: estimate first-pass beam cost as
        //   unigram_score + edge.penalty
        // edge.penalty already encodes the English / expansion penalties
        // set by ComputeEdgePenalties.
        std::vector<std::pair<float_t, std::size_t>> scored;
        scored.reserve(indices.size());
        for (auto idx : indices) {
            float_t cost = get_score(edges[idx].id) + edges[idx].penalty;
            scored.emplace_back(cost, idx);
        }
        std::partial_sort(scored.begin(),
                          scored.begin() + static_cast<std::ptrdiff_t>(NodeSize),
                          scored.end());

        for (std::size_t i = 0; i < NodeSize; ++i) {
            pruned.push_back(edges[scored[i].second]);
        }
    }

    edges = std::move(pruned);
}

State Sime::InitialState(const std::vector<TokenID>& context) const {
    Scorer::Pos pos = scorer_.StartPos();
    TokenID last = NotToken;
    const std::size_t usable =
        static_cast<std::size_t>(std::max(scorer_.Num() - 1, 1));
    const std::size_t begin = context.size() > usable
                                  ? context.size() - usable
                                  : 0;
    for (std::size_t i = begin; i < context.size(); ++i) {
        TokenID token = context[i];
        if (token == NotToken) {
            continue;
        }
#if SIME_WEAVE_OVERLAY
        if (Overlay::IsToken(token)) {
            // weave overlay word: a history break (not skipped like NotToken, so the
            // words on either side never look adjacent).
            pos = Scorer::Pos{};
            last = token;
            continue;
        }
#endif
        Scorer::Pos next{};
        scorer_.ScoreMove(pos, token, next);
        scorer_.Back(next);
        pos = next;
        last = token;
    }
    return State(0.0, 0, pos, nullptr, last);
}

namespace {
// weave-ime candidate contract: the edge's flags travel with the state (Backtrace).
template <class L>
std::uint8_t EdgeFlags(const L& l) {
    return static_cast<std::uint8_t>((l.english ? State::kEdgeEnglish : 0) |
                                     (l.expansion ? State::kEdgeExpansion : 0));
}
}  // namespace

void Sime::Relax(const State& value, const Link& word, NetStates& target) const {
#if SIME_WEAVE_SPACE_BOUNDARY
    if (word.id == SpaceToken) {
        // weave: a typed space carries the LM state through
        // unchanged at zero cost (no token, no context reset).
        target.Insert(State(value.score, word.end, value.pos, &value,
                            word.id, word.pieces, EdgeFlags(word)));
        return;
    }
#endif
    Scorer::Pos cur_pos = value.pos;
    Scorer::Pos next_pos{};
#if SIME_WEAVE_OVERLAY
    float_t step = StepCost(cur_pos, word.id, next_pos);
#else
    float_t step = scorer_.ScoreMove(cur_pos, word.id, next_pos);
#endif
    scorer_.Back(next_pos);
    cur_pos = next_pos;

    float_t user_adjust = 0.0;
    if (user_sentence_enabled_) {
        user_adjust = user_sentence_.CostAdjustment(
            value.backtrace_token, word.id, step);
    }
    float_t next_cost =
        value.score + step + word.penalty + user_adjust;
    State next(next_cost, word.end, cur_pos, &value, word.id,
               word.pieces, EdgeFlags(word));
    target.Insert(next);
}

void Sime::Process(std::vector<Node>& net) const {
    for (std::size_t col = 0; col < net.size(); ++col) {
        auto& column = net[col];
        for (auto it = column.states.begin(); it != column.states.end(); ++it) {
            const auto& value = *it;
            for (const auto& word : column.es) {
                Relax(value, word, net[word.end].states);
            }
        }
    }
}

std::vector<Sime::Link> Sime::Backtrace(
    const State& tail_state,
    std::size_t end) {
    std::vector<Link> path;
    const State* state = &tail_state;
    while (state != nullptr && state->backtrace_state != nullptr) {
        const State* prev = state->backtrace_state;
        Link link{prev->now, state->now, state->backtrace_token,
                  state->backtrace_pieces};
        link.english = state->backtrace_flags & State::kEdgeEnglish;
        link.expansion = state->backtrace_flags & State::kEdgeExpansion;
        path.push_back(link);
        state = prev;
    }
    std::reverse(path.begin(), path.end());
    if (!path.empty() && path.back().end == end) {
        path.pop_back();
    }
    return path;
}

std::u32string Sime::ToText(const Link& n, std::string_view input) const {
    if (IsNoToken(n.id)) {
        return {};
    }
#if SIME_WEAVE_LITERAL_EDGE
    if (n.id == LiteralToken) {
        // The typed ASCII letters of the span, case preserved.
        std::string_view span = input.substr(n.start, n.end - n.start);
        return std::u32string(span.begin(), span.end());
    }
#else
    (void)input;
#endif
#if SIME_WEAVE_OVERLAY
    // The typed span of this link (empty on paths without the input, e.g. the T9 path).
    const std::string_view typed = n.end <= input.size() && n.start <= n.end
        ? input.substr(n.start, n.end - n.start) : std::string_view();
    if (const auto* t = overlay_.TokenOf(n.id)) {
        return casing_ == DisplayCasing::kAsTyped ? AsTyped(t->spelling32, typed) : t->spelling32;
    }
#endif
    std::u32string buffer;
    const char32_t* chars = dict_.TokenAt(n.id);
    if (chars) {
        for (std::size_t i = 0; chars[i] != 0 && i < 64; ++i) {
            buffer.push_back(chars[i]);
        }
    }
#if SIME_WEAVE_OVERLAY
    if (casing_ == DisplayCasing::kAsTyped) {
        return AsTyped(buffer, typed);
    }
    if (!overlay_.Empty()) {
        if (const auto* c = overlay_.CanonicalForL0(n.id)) return *c;
    }
#endif
    return buffer;
}

std::vector<DecodeResult> Sime::DecodeSentence(
    std::string_view input,
    std::size_t extra) const {
    return DecodeSentence(input, {}, extra);
}

std::vector<TokenID> Sime::TokenizeContext(std::string_view utf8,
                                           std::size_t max_code_points) const {
    std::vector<TokenID> ids;
    if (!ready_ || utf8.empty() || max_code_points == 0) return ids;
    // Start of the last max_code_points code points (a code point starts at
    // every byte that is not a UTF-8 continuation byte 10xxxxxx).
    std::size_t begin = utf8.size(), seen = 0;
    while (begin > 0 && seen < max_code_points) {
        --begin;
        if ((static_cast<unsigned char>(utf8[begin]) & 0xC0) != 0x80) ++seen;
    }
    std::call_once(cutter_once_, [this] {
        cutter_ = std::make_unique<Cutter>(dict_, scorer_);
    });
    for (const auto& ct : cutter_->Cut(utf8.substr(begin))) {
        ids.push_back(ct.is_unk ? NotToken : ct.id);
    }
    return ids;
}

std::vector<DecodeResult> Sime::DecodeSentence(
    std::string_view input,
    const std::vector<TokenID>& context,
    std::size_t extra) const {
    std::lock_guard<std::mutex> lock(decode_mutex_);
#if SIME_WEAVE_INCREMENTAL
    if (inc_on_) return DecodeSession(input, context, extra);
    ++inc_stats_.decodes;
    ++inc_stats_.full;
    last_info_ = {};
#endif
    return DecodeSentenceUnlocked(input, context, extra);
}

void Sime::BuildSentenceNet(std::string_view lower, std::vector<Node>& net) const {
#if SIME_WEAVE_OVERLAY
    std::vector<std::vector<Link>> ov_edges;
    InitNet(lower, net, /*expansion=*/true, &ov_edges);
#else
    InitNet(lower, net, /*expansion=*/true);
#endif
    ComputeEdgePenalties(net, lower);
    for (auto& col : net) PruneNode(col.es);
#if SIME_WEAVE_OVERLAY
    AppendOverlayEdges(net, ov_edges, lower);
#endif
}

std::vector<DecodeResult> Sime::DecodeSentenceUnlocked(
    std::string_view input,
    const std::vector<TokenID>& context,
    std::size_t extra) const {
    std::vector<DecodeResult> results;
    if (!ready_ || input.empty()) return results;
    MaybeTrimCaches();

    // raw_off: the raw offset of each normalised byte (SIME_WEAVE_RAW_CNT's Layer 2 cnt and the
    // candidate contract's segment spans).
    std::vector<std::size_t> raw_off;
    std::string lower = NormalizeInput(input, &raw_off);
    if (lower.empty()) return results;

    std::vector<Node> net;
    BuildSentenceNet(lower, net);

    for (auto& col : net) col.states.SetMaxTop(BeamSize);
    State init = InitialState(context);
    net[0].states.Insert(init);
    Process(net);

    return SentenceResults(input, lower, raw_off, net.back().states.GetStates(), net.size(),
                           [&net](std::size_t c) -> const std::vector<Link>& { return net[c].es; },
                           extra);
}

std::vector<DecodeResult> Sime::SentenceResults(
    std::string_view input,
    std::string_view lower,
    const std::vector<std::size_t>& raw_off,
    const std::vector<State>& tail,
    std::size_t net_size,
    const ColumnEdges& column,
    std::size_t extra) const {
    std::vector<DecodeResult> results;
    const std::size_t total = lower.size();
    // Layer 2 cnt: normalised edge end by default; with SIME_WEAVE_RAW_CNT the
    // raw-input bytes consumed (the whole input once the edge reaches the end
    // of the normalised input, matching Layer 1's input.size()).
    auto l2_cnt = [&](std::size_t norm_end) -> std::size_t {
#if SIME_WEAVE_RAW_CNT
        return norm_end >= total ? input.size() : raw_off[norm_end];
#else
        return norm_end;
#endif
    };

    // `dedup` tracks texts already emitted in `results`; Layer 2 checks
    // against it to avoid duplicating a candidate that made it into Layer 1.
    // Layer 1 uses its own local dedup during beam scan so that beam members
    // that don't make the final 1+extra cut remain eligible for Layer 2.
    std::unordered_set<std::string> dedup;
    const float_t penalty_per_unit =
        std::abs(scorer_.UnknownPenalty()) / DistancePenalty;

    // === Layer 1: Full sentence N-best ===
    // Scan the whole beam, sort by score, take top (1 + extra).
    // (penalties are already in beam scores via edge.penalty)
    // weave-ime contract: segments are filled only for the emitted candidates (texts are unique
    // among the scanned ones, so each candidate keeps its path by text).
    {
        const std::size_t scan =
            std::min<std::size_t>(BeamSize, tail.size());
        std::vector<DecodeResult> l1;
        l1.reserve(scan);
        std::unordered_set<std::string> l1_seen;
        std::unordered_map<std::string, std::vector<Link>> l1_path;
        for (std::size_t rank = 0; rank < scan; ++rank) {
            auto path = Backtrace(tail[rank], net_size - 1);
            if (path.empty()) continue;
            std::string text = ExtractText(path, lower);
#if SIME_WEAVE_OVERLAY
            if (text.empty() || !l1_seen.insert(DedupKey(text)).second) continue;
#else
            if (text.empty() || !l1_seen.insert(text).second) continue;
#endif
            std::string py = ExtractUnits(path, lower);

            l1.push_back({std::move(text), std::move(py),
                          ExtractTokens(path),
                          -tail[rank].score,
                          input.size(),
                          {},
                          ScoreBasis::kSentence});
            l1_path.emplace(l1.back().text, std::move(path));
        }
        std::sort(l1.begin(), l1.end(),
                  [](const DecodeResult& a, const DecodeResult& b) {
                      return a.score > b.score;
                  });
        RerankWithGru(l1, gru_.get(), false);
        const std::size_t full_limit = 1 + extra;
        for (std::size_t i = 0; i < l1.size() && results.size() < full_limit;
             ++i) {
#if SIME_WEAVE_OVERLAY
            dedup.insert(DedupKey(l1[i].text));
#else
            dedup.insert(l1[i].text);
#endif
            l1[i].segments = ExtractSegments(l1_path.at(l1[i].text), lower, raw_off, input.size());
            results.push_back(std::move(l1[i]));
        }
    }

    // === Layer 2: word/char alternatives at position 0 ===
    // All entries (exact + expansion) compete on score; penalty is
    // already in the score so no separate exact/abbrev tiering.
    const std::size_t l2_start = results.size();
    std::vector<DecodeResult> best_l2;
    std::vector<Link> best_l2_edge;  // the edge of each best_l2 entry (its segment is filled last)
    std::unordered_map<std::string, std::size_t> l2_index_by_text;
    // The best entry per text (per DedupKey with the overlay); BetterLayer2Entry decides.
    auto push_l2 = [&](std::string key, DecodeResult candidate, const Link& edge) {
        auto it = l2_index_by_text.find(key);
        if (it == l2_index_by_text.end()) {
            l2_index_by_text.emplace(std::move(key), best_l2.size());
            best_l2.push_back(std::move(candidate));
            best_l2_edge.push_back(edge);
            return;
        }
        if (BetterLayer2Entry(candidate, best_l2[it->second])) {
            best_l2[it->second] = std::move(candidate);
            best_l2_edge[it->second] = edge;
        }
    };

    // Skip leading apostrophe-only columns (see DecodeNumSentence).
    std::size_t l2_col = 0;
    while (l2_col < total && column(l2_col).size() == 1 &&
           IsNoToken(column(l2_col)[0].id)) {
        ++l2_col;
    }
    for (const auto& edge : column(l2_col)) {
        if (IsNoToken(edge.id)) continue;

        std::u32string text_u32 = ToText(edge, lower);
        if (text_u32.empty()) continue;
        std::string text_utf8 = TextFromU32(text_u32);
#if SIME_WEAVE_OVERLAY
        std::string text_key = DedupKey(text_utf8);
        if (dedup.contains(text_key)) continue;
#else
        if (dedup.contains(text_utf8)) continue;
        std::string text_key = text_utf8;
#endif

        std::size_t distance = (total > edge.end) ? (total - edge.end) : 0;
        float_t dist_penalty =
            static_cast<float_t>(distance) * penalty_per_unit;

        auto slice = std::string_view(lower).substr(
            edge.start, edge.end - edge.start);

        // Use edge.penalty (English + expansion) so L2 score is on the
        // same coordinate system as L1: -(LM unigram + edge.penalty)
        // - dist_penalty.
        Scorer::Pos epos{};
        Scorer::Pos enext{};
#if SIME_WEAVE_OVERLAY
        float_t score = -StepCost(epos, edge.id, enext)
                        - dist_penalty
                        - edge.penalty;
#else
        float_t score = -scorer_.ScoreMove(epos, edge.id, enext)
                        - dist_penalty
                        - edge.penalty;
#endif

        std::string edge_py = edge.pieces
            ? AbbreviatePieces(edge.pieces, slice)
            : "";
        push_l2(std::move(text_key),
                {std::move(text_utf8), std::move(edge_py),
                 ExtractTokens({edge}), score, l2_cnt(edge.end),
                 {}, ScoreBasis::kWordAlt},
                edge);
    }

    for (std::size_t i = 0; i < best_l2.size(); ++i) {
        const Link& edge = best_l2_edge[i];
        std::vector<Segment> edge_seg(1);  // weave-ime contract: [0, the edge's raw end)
        edge_seg[0].end = edge.end >= total ? input.size() : raw_off[edge.end];
        FillSegment(edge, lower, edge_seg[0]);
        best_l2[i].segments = std::move(edge_seg);
        results.push_back(std::move(best_l2[i]));
    }
    auto by_score = [](const DecodeResult& a, const DecodeResult& b) {
        return a.score > b.score;
    };
    std::sort(results.begin() + static_cast<std::ptrdiff_t>(l2_start), results.end(), by_score);

    return results;
}

#if SIME_WEAVE_INCREMENTAL
namespace {

template <class L>
bool SameLink(const L& a, const L& b) {
    return a.start == b.start && a.end == b.end && a.id == b.id && a.pieces == b.pieces &&
           std::memcmp(&a.penalty, &b.penalty, sizeof a.penalty) == 0 &&
           a.expansion == b.expansion && a.english == b.english;
}

template <class L>
bool SameEdges(const std::vector<L>& a, const std::vector<L>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!SameLink(a[i], b[i])) return false;
    return true;
}

bool SameSegment(const Segment& a, const Segment& b) {
    return a.start == b.start && a.end == b.end && a.text == b.text && a.typed_text == b.typed_text &&
           a.canonical_text == b.canonical_text && a.kind == b.kind && a.layer == b.layer &&
           a.sources == b.sources && a.token == b.token && a.units == b.units &&
           a.expansion == b.expansion;
}

// Every field of every candidate, scores bit for bit (the self-check).
bool SameResults(const std::vector<DecodeResult>& a, const std::vector<DecodeResult>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto& x = a[i];
        const auto& y = b[i];
        if (x.text != y.text || x.units != y.units || x.tokens != y.tokens || x.cnt != y.cnt ||
            x.basis != y.basis || std::memcmp(&x.score, &y.score, sizeof x.score) != 0 ||
            x.segments.size() != y.segments.size())
            return false;
        for (std::size_t s = 0; s < x.segments.size(); ++s)
            if (!SameSegment(x.segments[s], y.segments[s])) return false;
    }
    return true;
}

std::size_t CountStates(NetStates& states) {
    std::size_t n = 0;
    for (auto it = states.begin(); it != states.end(); ++it) ++n;
    return n;
}

}  // namespace

void Sime::BumpGeneration() const {
    ++inc_stats_.generation;
    chain_.clear();
    inc_stats_.snapshots = 0;
}

void Sime::BumpGenerationLocked() const {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    BumpGeneration();
}

void Sime::SetIncremental(bool on) {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    inc_on_ = on;
    BumpGeneration();
}

bool Sime::Incremental() const {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    return inc_on_;
}

void Sime::ResetIncremental() {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    BumpGeneration();
}

void Sime::SetIncrementalSelfCheck(bool on) {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    inc_self_check_ = on;
}

void Sime::SetIncrementalDiagnostics(bool on) {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    inc_diag_ = on;
}

IncrementalStats Sime::GetIncrementalStats() const {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    return inc_stats_;
}

DecodeInfo Sime::LastDecodeInfo() const {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    return last_info_;
}

void Sime::SetIncrementalFaultForTest(IncrementalFault fault) {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    inc_fault_ = fault;
}

std::vector<DecodeResult> Sime::DecodeSentenceFull(
    std::string_view input,
    const std::vector<TokenID>& context,
    std::size_t extra) const {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    return DecodeSentenceUnlocked(input, context, extra);
}

std::unique_ptr<Sime::Snapshot> Sime::FullSnapshot(const std::string& lower,
                                                   const std::vector<TokenID>& context) const {
    std::vector<Node> net;
    BuildSentenceNet(lower, net);
    auto snap = std::make_unique<Snapshot>();
    snap->norm = lower;
    snap->generation = inc_stats_.generation;
    snap->edges.resize(net.size());
    snap->states.resize(net.size());
    for (std::size_t c = 0; c < net.size(); ++c) {
        snap->edges[c] = std::make_shared<const std::vector<Link>>(std::move(net[c].es));
        snap->states[c] = std::make_shared<NetStates>();
        snap->states[c]->SetMaxTop(BeamSize);
    }
    snap->states[0]->Insert(InitialState(context));
    ProcessFrom(*snap, 0);
    return snap;
}

void Sime::ProcessFrom(Snapshot& snap, std::size_t k) const {
    // Columns before k are final and shared; replaying their edges that end after k, in Process's
    // order (column, state, edge), gives each later column the insertions a full decode makes first.
    for (std::size_t col = 0; col < k; ++col) {
        auto& states = *snap.states[col];
        const auto& edges = *snap.edges[col];
        for (auto it = states.begin(); it != states.end(); ++it) {
            for (const auto& word : edges) {
                if (word.end > k) Relax(*it, word, *snap.states[word.end]);
            }
        }
    }
    for (std::size_t col = k; col < snap.edges.size(); ++col) {
        auto& states = *snap.states[col];
        const auto& edges = *snap.edges[col];
        for (auto it = states.begin(); it != states.end(); ++it) {
            for (const auto& word : edges) {
                Relax(*it, word, *snap.states[word.end]);
            }
        }
    }
}

void Sime::ClassifyColumn(const std::vector<Link>& before, const std::vector<Link>& after,
                          std::size_t old_end) const {
    // The identity of an edge, without its penalty.
    auto same_id = [](const Link& a, const Link& b) {
        return a.start == b.start && a.end == b.end && a.id == b.id && a.pieces == b.pieces &&
               a.expansion == b.expansion && a.english == b.english;
    };
    bool append = false, old_end_changed = false, penalty = false, other = false;
    for (const auto& e : after) append = append || e.end > old_end;
    // Match each old edge to the first unused new edge with the same identity.
    std::vector<bool> used(after.size(), false);
    std::vector<std::size_t> match;  // the new index of each matched old edge, in old order
    for (const auto& e : before) {
        std::size_t j = 0;
        while (j < after.size() && (used[j] || !same_id(after[j], e))) ++j;
        if (j == after.size()) {
            (e.end >= old_end ? old_end_changed : other) = true;
            continue;
        }
        used[j] = true;
        match.push_back(j);
        if (std::memcmp(&e.penalty, &after[j].penalty, sizeof e.penalty) != 0) penalty = true;
    }
    for (std::size_t j = 0; j < after.size(); ++j)
        if (!used[j] && after[j].end <= old_end) (after[j].end == old_end ? old_end_changed : other) = true;
    const bool order = !std::is_sorted(match.begin(), match.end());
    inc_stats_.diff_append += append;
    inc_stats_.diff_old_end += old_end_changed;
    inc_stats_.diff_penalty += penalty;
    inc_stats_.diff_order += order;
    inc_stats_.diff_other += other;
}

std::unique_ptr<Sime::Snapshot> Sime::ResumeSnapshot(const Snapshot& base, const std::string& lower,
                                                     std::size_t& resume_col) const {
    const std::size_t old_total = base.norm.size();
    // Consistency checks on the snapshot: one edge list and one beam per column, none missing, its
    // input a proper prefix of the new one, and its column 0 holding just the initial state.
    if (base.edges.size() != old_total + 2 || base.states.size() != old_total + 2 ||
        lower.size() <= old_total || lower.compare(0, old_total, base.norm) != 0)
        return nullptr;
    for (std::size_t c = 0; c < base.edges.size(); ++c)
        if (!base.edges[c] || !base.states[c]) return nullptr;
    if (CountStates(*base.states[0]) != 1) return nullptr;

    std::vector<Node> net;
    BuildSentenceNet(lower, net);
    // k: the first column whose edges differ, at most old_total + 1. The old end column held only the
    // end sentinel; it usually differs, but an apostrophe typed where no earlier edge changes (after
    // a typed space) gives it exactly the sentinel's edge. Then every column up to the old end is the
    // same, and the beams up to old_total + 1 (the old tail) are reused: that column receives only
    // edges from the equal columns before it.
    std::size_t k = 0;
    while (k <= old_total && SameEdges(*base.edges[k], net[k].es)) ++k;
    if (inc_diag_) {
        for (std::size_t c = k; c < old_total; ++c)
            if (!SameEdges(*base.edges[c], net[c].es)) ClassifyColumn(*base.edges[c], net[c].es, old_total);
    }

    auto snap = std::make_unique<Snapshot>();
    snap->norm = lower;
    snap->generation = inc_stats_.generation;
    snap->edges.resize(net.size());
    snap->states.resize(net.size());
    for (std::size_t c = 0; c < net.size(); ++c) {
        if (c < k) {
            snap->edges[c] = base.edges[c];
        } else {
            snap->edges[c] = std::make_shared<const std::vector<Link>>(std::move(net[c].es));
        }
        if (c <= k) {
            snap->states[c] = base.states[c];
        } else {
            snap->states[c] = std::make_shared<NetStates>();
            snap->states[c]->SetMaxTop(BeamSize);
        }
    }
    ProcessFrom(*snap, k);
    if (inc_diag_) {
        for (std::size_t c = k + 1; c < snap->states.size(); ++c)
            inc_stats_.capped_redone += CountStates(*snap->states[c]) >= SIME_WEAVE_COLUMN_BEAM_SIZE;
    }
    resume_col = k;
    return snap;
}

std::vector<DecodeResult> Sime::DecodeSession(
    std::string_view input,
    const std::vector<TokenID>& context,
    std::size_t extra) const {
    ++inc_stats_.decodes;
    last_info_ = {};
    if (context != inc_context_) {  // a new decoder context: no snapshot applies
        BumpGeneration();
        inc_context_ = context;
    }
    std::vector<DecodeResult> results;
    if (!ready_ || input.empty()) {
        ++inc_stats_.full;
        return results;
    }
    MaybeTrimCaches();
    std::vector<std::size_t> raw_off;
    std::string lower = NormalizeInput(input, &raw_off);
    if (lower.empty()) {
        ++inc_stats_.full;
        return results;
    }

    // Q3: keep the snapshots that are prefixes of the new input when it extends or shortens the
    // composition; anything else (an edit inside it) drops them all.
    if (!chain_.empty() && chain_.back()->generation != inc_stats_.generation) chain_.clear();
    if (!chain_.empty()) {
        const std::string& top = chain_.back()->norm;
        const bool extends = lower.size() >= top.size() && lower.compare(0, top.size(), top) == 0;
        const bool shrinks = lower.size() < top.size() && top.compare(0, lower.size(), lower) == 0;
        if (!extends && !shrinks) chain_.clear();
        while (!chain_.empty() && chain_.back()->norm.size() > lower.size()) chain_.pop_back();
    }

    DecodeInfo info;
    const Snapshot* snap = nullptr;
    if (!chain_.empty() && chain_.back()->norm != lower && inc_fault_ == IncrementalFault::kBreakSnapshot) {
        inc_fault_ = IncrementalFault::kNone;
        chain_.back()->states.pop_back();  // fails the consistency check below
    }
    if (!chain_.empty() && chain_.back()->norm == lower) {
        snap = chain_.back().get();
        info.kind = DecodeKind::kSame;
        info.resume_col = snap->edges.size();
        ++inc_stats_.same;
    } else if (!chain_.empty()) {
        std::size_t k = 0;
        auto next = ResumeSnapshot(*chain_.back(), lower, k);
        if (next) {
            info.kind = DecodeKind::kResume;
            info.resume_col = k;
            ++inc_stats_.resumed;
            inc_stats_.columns += next->edges.size();
            inc_stats_.columns_redone += next->edges.size() - k;
            chain_.push_back(std::move(next));
        } else {
            info.kind = DecodeKind::kFallback;
            ++inc_stats_.fallbacks;
            chain_.clear();
        }
    }
    if (chain_.empty()) {  // cold: a full decode, kept as the first snapshot
        if (info.kind != DecodeKind::kFallback) {
            info.kind = DecodeKind::kFull;
            ++inc_stats_.full;
        }
        chain_.push_back(FullSnapshot(lower, context));
    }
    if (chain_.size() > kMaxSnapshots) chain_.erase(chain_.begin());
    inc_stats_.snapshots = chain_.size();
    if (!snap) snap = chain_.back().get();
    info.warm = info.kind == DecodeKind::kSame || info.kind == DecodeKind::kResume;
    info.columns = snap->edges.size();

    results = SentenceResults(input, lower, raw_off, snap->states.back()->GetStates(),
                              snap->edges.size(),
                              [snap](std::size_t c) -> const std::vector<Link>& { return *snap->edges[c]; },
                              extra);
    if (info.warm && inc_fault_ == IncrementalFault::kPerturbResult) {
        inc_fault_ = IncrementalFault::kNone;
        if (!results.empty()) results[0].score = std::nextafter(results[0].score, 1e300);
    }
    if (info.warm && inc_self_check_) {
        ++inc_stats_.self_checks;
        auto full = DecodeSentenceUnlocked(input, context, extra);
        if (!SameResults(results, full)) {
            ++inc_stats_.fallbacks;
            if (info.kind == DecodeKind::kSame) --inc_stats_.same; else --inc_stats_.resumed;
            info.kind = DecodeKind::kFallback;
            info.warm = false;
            BumpGeneration();
            results = std::move(full);
        }
    }
    last_info_ = info;
    return results;
}
#endif

std::vector<DecodeResult> Sime::NextTokens(
    const std::vector<TokenID>& context,
    std::size_t num,
    bool en) const {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    std::vector<DecodeResult> results;
    if (!ready_ || num == 0) return results;
    MaybeTrimCaches();

    // Only the last (num-1) context tokens matter — that's the maximum
    // history a num-gram model can condition on.  Walking from root with
    // at most (num-1) steps also guarantees we never reach leaf level.
    Scorer::Pos pos{};
    const auto tail = static_cast<std::size_t>(std::max(scorer_.Num() - 1, 1));
    std::size_t start = context.size() > tail ? context.size() - tail : 0;
    for (std::size_t i = start; i < context.size(); ++i) {
        Scorer::Pos next{};
        scorer_.ScoreMove(pos, context[i], next);
        pos = next;
    }

    // When filtering to English only, widen the scorer pool — many top
    // predictions will be Chinese and get dropped.
    const std::size_t pool = en ? num * 16 : num * 4;
    auto next_tokens = scorer_.NextTokens(pos, pool);

    // English tokens in Sime's corpora are ASCII-letter words (possibly
    // containing an apostrophe, e.g. don't, 's), or SentencePiece-style
    // pieces prefixed with U+2581 (word boundary). Contraction pieces like
    // "'s" / "'re" start with the apostrophe, so accept it too.
    auto is_english = [](const char32_t* chars) {
        char32_t c = chars[0];
        if (c == 0x2581) return true;
        if (c == U'\'') return true;
        return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z');
    };

    std::unordered_set<std::string> seen;
    for (const auto& [tid, pro] : next_tokens) {
        if (results.size() >= num) break;
        if (tid < StartToken) continue;

        const char32_t* chars = dict_.TokenAt(tid);
        if (!chars || chars[0] == 0) continue;
        if (en && !is_english(chars)) continue;

        std::u32string u32;
        for (std::size_t i = 0; chars[i] != 0; ++i)
            u32.push_back(chars[i]);
        std::string text = TextFromU32(u32);
        if (!seen.insert(text).second) continue;

        results.push_back({std::move(text), {}, {tid}, -pro, 0});
    }

    return results;
}

std::vector<DecodeResult> Sime::GetTokens(
    std::string_view prefix,
    std::size_t num,
    bool en) const {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    std::vector<DecodeResult> results;
    if (!ready_ || num == 0 || prefix.empty()) return results;
    MaybeTrimCaches();

    struct Candidate {
        std::string text;
        TokenID id;
        float_t score;
    };
    std::vector<Candidate> candidates;

    auto collect = [&](Dict::DatType type,
                       const std::vector<trie::SearchResult>& matches) {
        for (const auto& r : matches) {
            auto entry = dict_.GetEntry(type, r.value);
            for (uint32_t i = 0; i < entry.count; ++i) {
                TokenID tid = entry.items[i].id;
                const char32_t* chars = dict_.TokenAt(tid);
                if (!chars || chars[0] == 0) continue;

                std::u32string u32;
                for (std::size_t j = 0; chars[j] != 0; ++j)
                    u32.push_back(chars[j]);
                std::string text = TextFromU32(u32);

                Scorer::Pos pos{};
                Scorer::Pos next{};
                float_t cost = scorer_.ScoreMove(pos, tid, next);

                candidates.push_back({std::move(text), tid, cost});
            }
        }
    };

#if SIME_WEAVE_CASE_FOLD
    // Case-folded English lookup (and the prefix as typed when it has
    // capitals, see ForEachFoldedEnItem); a typed uppercase letter keeps only
    // the completions with that casing, when there are any.
    const std::string en_prefix = FoldEnKey(prefix);
    ForEachFoldedEnItem(dict_, prefix, en_prefix, /*prefix=*/true, /*filter=*/true,
                        [&](std::string_view k) {
                            return dict_.Dat(Dict::LetterEn).FindWordsWithPrefix(k, num * 4);
                        },
                        [&](std::size_t, const Dict::Item& item) {
        const char32_t* chars = dict_.TokenAt(item.id);
        if (!chars || chars[0] == 0) return;
        std::u32string u32(chars);
        Scorer::Pos pos{};
        Scorer::Pos next{};
        candidates.push_back({TextFromU32(u32), item.id,
                              scorer_.ScoreMove(pos, item.id, next)});
    });
#else
    collect(Dict::LetterEn,
            dict_.Dat(Dict::LetterEn).FindWordsWithPrefix(prefix, num * 4));
#endif
    if (!en) {
        // Mixed mode: also search the pinyin DAT. Plain prefix match is
        // exactly what we want — user typed a literal pinyin prefix (possibly
        // with '\'' syllable separators) and wants every token whose stored
        // pinyin starts with it, including deeper multi-syllable completions.
        collect(Dict::LetterPinyin,
                dict_.Dat(Dict::LetterPinyin)
                    .FindWordsWithPrefix(prefix, num * 4));
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.score < b.score; });

    std::unordered_set<std::string> seen;
    for (auto& c : candidates) {
        if (results.size() >= num) break;
        if (!seen.insert(c.text).second) continue;
        results.push_back({std::move(c.text), {}, {c.id}, -c.score,
                           prefix.size()});
    }

    return results;
}

} // namespace sime
