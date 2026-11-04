// Modified by the weave-ime project (2026): experiment branch weave-learning only: SIME_WEAVE_LEARNING adds a commit clock, lazily decayed per-entry counts, a bigram-only switch, a fixed bound on the adjustment and a counts-only file (Phase 5 F14).
#include "user.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <istream>
#include <limits>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace sime {

namespace {

constexpr const char* kHeader = "SIME_USER_SENTENCE_V4";
constexpr std::string_view kVocabPrefix = "VOCAB ";
// Ney's absolute-discount values, mirroring the main LM's defaults in
// construct.cc (DefaultDiscounts = {0.0005, 0.5, 0.5}). Subtracted
// from each raw count; lost mass is redistributed via gamma to the
// lower-order distribution.
constexpr float_t kBigramDiscount = 0.5;
constexpr float_t kUnigramDiscount = 0.0005;

void IncCount(std::unordered_map<TokenID, std::uint32_t>& m,
              TokenID key, std::uint32_t delta) {
    m[key] += delta;
}

void DecCount(std::unordered_map<TokenID, std::uint32_t>& m,
              TokenID key, std::uint32_t delta) {
    auto it = m.find(key);
    if (it == m.end()) return;
    if (it->second <= delta) m.erase(it);
    else it->second -= delta;
}

// Bigram inc/dec also tracks N1+(prev, *) — distinct-successor count.
// We need to know when count crosses 0 in either direction to update
// the successor count atomically.
void IncBigramTrackSucc(
    std::unordered_map<std::uint64_t, std::uint32_t>& bigram,
    std::unordered_map<TokenID, std::uint32_t>& succ,
    TokenID prev, std::uint64_t key, std::uint32_t delta) {
    auto& count = bigram[key];  // creates entry with 0 if absent
    bool was_zero = (count == 0);
    count += delta;
    if (was_zero && count > 0) {
        ++succ[prev];
    }
}

void DecBigramTrackSucc(
    std::unordered_map<std::uint64_t, std::uint32_t>& bigram,
    std::unordered_map<TokenID, std::uint32_t>& succ,
    TokenID prev, std::uint64_t key, std::uint32_t delta) {
    auto it = bigram.find(key);
    if (it == bigram.end()) return;
    bool was_nonzero = (it->second > 0);
    bool became_zero;
    if (it->second <= delta) {
        bigram.erase(it);
        became_zero = was_nonzero;
    } else {
        it->second -= delta;
        became_zero = false;
    }
    if (became_zero) {
        auto sit = succ.find(prev);
        if (sit != succ.end()) {
            if (sit->second <= 1) succ.erase(sit);
            else --sit->second;
        }
    }
}

} // namespace

void UserSentence::Clear() {
    sentences_.clear();
    unigram_.clear();
    bigram_.clear();
    distinct_successors_.clear();
    total_unigram_count_ = 0;
#if SIME_WEAVE_LEARNING
    clock_ = 0;
    aged_uni_.clear();
    aged_bi_.clear();
    aged_succ_.clear();
    aged_total_ = {};
#endif
}

bool UserSentence::Load(const std::filesystem::path& path,
                       std::string_view vocab_sig) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }
    return LoadText(in, vocab_sig);
}

bool UserSentence::Save(const std::filesystem::path& path,
                       std::string_view vocab_sig,
                       const TokenTextFn& token_text) const {
    if (auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        if (!out.is_open()) {
            return false;
        }
        if (!SaveText(out, vocab_sig, token_text)) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool UserSentence::LoadText(std::istream& in, std::string_view vocab_sig) {
    std::string line;
    if (!std::getline(in, line) || line != kHeader) {
        return false;
    }

    if (!std::getline(in, line)) {
        return false;
    }
    if (line.size() < kVocabPrefix.size() ||
        std::string_view(line).substr(0, kVocabPrefix.size()) != kVocabPrefix) {
        return false;
    }
    std::string_view stored_sig =
        std::string_view(line).substr(kVocabPrefix.size());
    if (!vocab_sig.empty() && stored_sig != vocab_sig) {
        return false;
    }

    std::deque<Entry> loaded;
    Entry current;
    auto flush = [&]() {
        if (!current.selections.empty()) {
            loaded.push_back(std::move(current));
            current = Entry{};
        }
    };

    while (std::getline(in, line)) {
        if (line.empty()) {
            flush();
            if (loaded.size() >= max_sentence_count_) {
                break;
            }
            continue;
        }

        // Per-selection line: "<id1> <id2> ...[\t<annotation>]"
        // The annotation is informational only — we ignore it on load.
        auto tab = line.find('\t');
        std::string_view ids_part = (tab == std::string::npos)
                                        ? std::string_view(line)
                                        : std::string_view(line).substr(0, tab);

        std::istringstream ss{std::string(ids_part)};
        std::uint64_t raw = 0;
        Sentence tokens;
        while (ss >> raw) {
            if (raw > std::numeric_limits<TokenID>::max()) {
                tokens.clear();
                break;
            }
            tokens.push_back(static_cast<TokenID>(raw));
        }
        tokens = CleanSentence(tokens);
        if (tokens.empty()) {
            continue;
        }
        current.selections.push_back({std::move(tokens)});
    }
    flush();

    sentences_ = std::move(loaded);
    Rebuild();
    return true;
}

bool UserSentence::LoadAndMigrate(const std::filesystem::path& path,
                                  const TokenizeFn& tokenize) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }

    std::string line;
    if (!std::getline(in, line) || line != kHeader) {
        return false;
    }
    // Skip the stored VOCAB line — caller's vocab_sig will be written
    // on the next save.
    if (!std::getline(in, line) ||
        line.size() < kVocabPrefix.size() ||
        std::string_view(line).substr(0, kVocabPrefix.size()) != kVocabPrefix) {
        return false;
    }

    std::deque<Entry> migrated;
    Entry current;

    auto flush_paragraph = [&]() {
        if (!current.selections.empty()) {
            migrated.push_back(std::move(current));
            current = Entry{};
        }
    };

    while (std::getline(in, line)) {
        if (line.empty()) {
            flush_paragraph();
            if (migrated.size() >= max_sentence_count_) {
                break;
            }
            continue;
        }
        // Take only the text column (after tab); old IDs are useless
        // under the new vocab. Strip the per-token spaces we wrote at
        // save time so Cutter sees an unbroken run.
        auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        std::string text = line.substr(tab + 1);
        text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
        if (text.empty()) {
            continue;
        }

        // Per-line tokenize: each original selection becomes one new
        // Selection in the migrated file, preserving the user's pick
        // grouping. If any token in the slice is OOV under the new
        // vocab, drop the whole selection.
        auto cuts = tokenize(text);
        Sentence sel_tokens;
        bool has_unk = false;
        for (const auto& [id, txt] : cuts) {
            if (id == NotToken || id == static_cast<TokenID>(-1)) {
                has_unk = true;
                break;
            }
            sel_tokens.push_back(id);
        }
        if (has_unk || sel_tokens.empty()) {
            continue;
        }
        current.selections.push_back({std::move(sel_tokens)});
    }
    flush_paragraph();

    sentences_ = std::move(migrated);
    Rebuild();
    return true;
}

bool UserSentence::SaveText(std::ostream& out,
                           std::string_view vocab_sig,
                           const TokenTextFn& token_text) const {
    out << kHeader << '\n';
    out << kVocabPrefix << vocab_sig << '\n';
    bool first_entry = true;
    for (const auto& entry : sentences_) {
        if (!first_entry) {
            out << '\n';
        }
        first_entry = false;
        for (const auto& sel : entry.selections) {
            for (std::size_t i = 0; i < sel.tokens.size(); ++i) {
                if (i != 0) {
                    out << ' ';
                }
                out << sel.tokens[i];
            }
            if (token_text) {
                bool any = false;
                std::string annotation;
                for (std::size_t i = 0; i < sel.tokens.size(); ++i) {
                    auto t = token_text(sel.tokens[i]);
                    if (i != 0) {
                        annotation.push_back(' ');
                    }
                    annotation.append(t);
                    if (!t.empty()) {
                        any = true;
                    }
                }
                if (any) {
                    out << '\t' << annotation;
                }
            }
            out << '\n';
        }
    }
    return static_cast<bool>(out);
}

void UserSentence::SetMaxSentenceCount(std::size_t count) {
    max_sentence_count_ = count;
    while (sentences_.size() > max_sentence_count_) {
        AdjustEntryCounts(sentences_.back(), -1);
        sentences_.pop_back();
    }
}

void UserSentence::SetWeight(float_t weight) {
    if (weight < 0.0) {
        weight_ = 0.0;
    } else {
        weight_ = weight;
    }
}

void UserSentence::Add(const Sentence& context, const Sentence& tokens) {
    Sentence clean = CleanSentence(tokens);
    if (clean.empty()) {
        return;
    }
    Selection sel{std::move(clean)};

    Sentence clean_context = CleanSentence(context);
    if (!clean_context.empty() && !sentences_.empty()) {
        Sentence latest_flat = Flatten(sentences_.front());
        if (EndsWith(latest_flat, clean_context)) {
            AppendToLatest(std::move(sel));
            return;
        }
    }
    StartParagraph(std::move(sel));
}

std::uint32_t UserSentence::UnigramCount(TokenID token) const {
    if (token == NotToken) {
        return 0;
    }
    auto iter = unigram_.find(token);
    if (iter == unigram_.end()) {
        return 0;
    }
    return iter->second;
}

std::uint32_t UserSentence::BigramCount(TokenID prev, TokenID cur) const {
    if (prev == NotToken || cur == NotToken) {
        return 0;
    }
    auto iter = bigram_.find(BigramKey(prev, cur));
    if (iter == bigram_.end()) {
        return 0;
    }
    return iter->second;
}

std::uint32_t UserSentence::DistinctSuccessors(TokenID prev) const {
    if (prev == NotToken) {
        return 0;
    }
    auto it = distinct_successors_.find(prev);
    return it == distinct_successors_.end() ? 0 : it->second;
}

float_t UserSentence::CostAdjustment(TokenID prev, TokenID cur,
                                     float_t lm_step) const {
#if SIME_WEAVE_LEARNING
    ++adjust_calls_;
    if (learn_.half_life_uni > 0.0 || learn_.half_life_bi > 0.0) {
        return AgedAdjustment(prev, cur, lm_step);
    }
    if (learn_.bigram_only && (prev == NotToken || BigramCount(prev, cur) == 0)) {
        return 0.0;
    }
#endif
    if (cur == NotToken || weight_ <= 0.0 || weight_ >= 1.0 ||
        total_unigram_count_ == 0) {
        return 0.0;
    }
    auto cur_count = UnigramCount(cur);
    if (cur_count == 0) {
        return 0.0;
    }
#if SIME_WEAVE_LEARNING
    ++adjust_hits_;
#endif

    // Interpolated Absolute Discounting at both levels — same family
    // and same per-order D values as the main LM (construct.cc):
    //   gamma(h)    = D · N1+(h, *) / c(h)
    //   P(w|h)      = max(c(h,w) - D, 0) / c(h)  +  gamma(h) · P(w|h')
    // The unigram tail "terminates at zero" rather than backing off
    // to a uniform base. Numerically near-equivalent to the main LM
    // (D_unigram = 0.0005 is tiny) and conceptually clean: the outer
    // mixture-with-LM provides the real fallback for tokens the user
    // has never picked.
    const float_t total = static_cast<float_t>(total_unigram_count_);
    float_t pr_unigram =
        std::max(static_cast<float_t>(cur_count) - kUnigramDiscount, 0.0) /
        total;

    float_t pr_user = pr_unigram;
    if (prev != NotToken) {
        auto prev_count = UnigramCount(prev);
        if (prev_count > 0) {
            auto bigram_count = BigramCount(prev, cur);
            auto n_succ = DistinctSuccessors(prev);
            const float_t denom = static_cast<float_t>(prev_count);
            const float_t alpha =
                std::max(static_cast<float_t>(bigram_count) - kBigramDiscount,
                         0.0) /
                denom;
            const float_t gamma =
                kBigramDiscount * static_cast<float_t>(n_succ) / denom;
#if SIME_WEAVE_LEARNING
            pr_user = learn_.bigram_only ? alpha : alpha + gamma * pr_unigram;
#else
            pr_user = alpha + gamma * pr_unigram;
#endif
        }
    }
    pr_user = std::min<float_t>(pr_user, 1.0);
    if (pr_user <= 0.0) {
        return 0.0;
    }

    // Mixture with LM in probability space, max-floored at lm_log_prob
    // so user history can only improve the score, never worsen it.
    const float_t lm_log_prob = -lm_step;
    const float_t user_log_prob = std::log(pr_user);
    const float_t a = lm_log_prob + std::log(1.0 - weight_);
    const float_t b = user_log_prob + std::log(weight_);
    float_t mix;
    if (a > b) {
        mix = a + std::log1p(std::exp(b - a));
    } else {
        mix = b + std::log1p(std::exp(a - b));
    }
    const float_t mixed_log_prob = std::max(lm_log_prob, mix);
#if SIME_WEAVE_LEARNING
    if (learn_.max_adjust > 0.0) {
        return std::max(lm_log_prob - mixed_log_prob, -learn_.max_adjust);
    }
#endif
    return lm_log_prob - mixed_log_prob;
}

#if SIME_WEAVE_LEARNING
float_t UserSentence::Effective(const Aged& e, std::uint64_t now, float_t half_life) {
    if (half_life <= 0.0 || now <= e.last) {
        return e.count;
    }
    return e.count * std::pow(2.0, -static_cast<float_t>(now - e.last) / half_life);
}

void UserSentence::Bump(Aged& e, std::uint64_t now, float_t half_life, float_t delta) {
    e.count = Effective(e, now, half_life) + delta;
    e.last = now;
}

// One learned token after `prev`. N1+(prev, *) counts a successor again once its count had faded to the bigram
// discount or below: from there on it adds nothing to the numerator, so it behaves as new.
void UserSentence::AgeToken(TokenID prev, TokenID token) {
    Bump(aged_uni_[token], clock_, learn_.half_life_uni, 1.0);
    Bump(aged_total_, clock_, learn_.half_life_uni, 1.0);
    if (prev == NotToken) {
        return;
    }
    Aged& bi = aged_bi_[BigramKey(prev, token)];
    const bool fresh = Effective(bi, clock_, learn_.half_life_bi) <= kBigramDiscount;
    Bump(bi, clock_, learn_.half_life_bi, 1.0);
    if (fresh) {
        Bump(aged_succ_[prev], clock_, learn_.half_life_uni, 1.0);
    }
}

void UserSentence::NoteCommit(std::size_t unlearned_tokens) {
    ++clock_;
    if (unlearned_tokens > 0 && (learn_.half_life_uni > 0.0 || learn_.half_life_bi > 0.0)) {
        Bump(aged_total_, clock_, learn_.half_life_uni, static_cast<float_t>(unlearned_tokens));
    }
}

float_t UserSentence::EffectiveUnigram(TokenID token) const {
    auto it = aged_uni_.find(token);
    return it == aged_uni_.end() ? 0.0 : Effective(it->second, clock_, learn_.half_life_uni);
}

float_t UserSentence::EffectiveBigram(TokenID prev, TokenID cur) const {
    auto it = aged_bi_.find(BigramKey(prev, cur));
    return it == aged_bi_.end() ? 0.0 : Effective(it->second, clock_, learn_.half_life_bi);
}

float_t UserSentence::EffectiveTotal() const {
    return Effective(aged_total_, clock_, learn_.half_life_uni);
}

std::size_t UserSentence::LiveUnigrams(float_t floor) const {
    std::size_t n = 0;
    for (const auto& [k, e] : aged_uni_) {
        n += Effective(e, clock_, learn_.half_life_uni) >= floor;
    }
    return n;
}

std::size_t UserSentence::LiveBigrams(float_t floor) const {
    std::size_t n = 0;
    for (const auto& [k, e] : aged_bi_) {
        n += Effective(e, clock_, learn_.half_life_bi) >= floor;
    }
    return n;
}

float_t UserSentence::Mix(float_t pr_user, float_t lm_step) const {
    pr_user = std::min<float_t>(pr_user, 1.0);
    if (pr_user <= 0.0) {
        return 0.0;
    }
    const float_t lm_log_prob = -lm_step;
    const float_t a = lm_log_prob + std::log(1.0 - weight_);
    const float_t b = std::log(pr_user) + std::log(weight_);
    const float_t mix = a > b ? a + std::log1p(std::exp(b - a)) : b + std::log1p(std::exp(a - b));
    const float_t adjust = lm_log_prob - std::max(lm_log_prob, mix);
    return learn_.max_adjust > 0.0 ? std::max(adjust, -learn_.max_adjust) : adjust;
}

// The model of CostAdjustment on effective counts: same discounts, same mixture.
float_t UserSentence::AgedAdjustment(TokenID prev, TokenID cur, float_t lm_step) const {
    if (cur == NotToken || weight_ <= 0.0 || weight_ >= 1.0) {
        return 0.0;
    }
    const float_t cur_count = EffectiveUnigram(cur);
    const float_t total = EffectiveTotal();
    if (cur_count <= 0.0 || total <= 0.0) {
        return 0.0;
    }
    ++adjust_hits_;
    const float_t pr_unigram = std::max(cur_count - kUnigramDiscount, 0.0) / total;
    float_t pr_user = learn_.bigram_only ? 0.0 : pr_unigram;
    if (prev != NotToken) {
        const float_t prev_count = EffectiveUnigram(prev);
        if (prev_count > 0.0) {
            const float_t bi = EffectiveBigram(prev, cur);
            auto sit = aged_succ_.find(prev);
            const float_t n_succ = sit == aged_succ_.end() ? 0.0 : Effective(sit->second, clock_, learn_.half_life_uni);
            const float_t alpha = std::max(bi - kBigramDiscount, 0.0) / prev_count;
            const float_t gamma = kBigramDiscount * n_succ / prev_count;
            pr_user = learn_.bigram_only ? alpha : alpha + gamma * pr_unigram;
        }
    }
    return Mix(pr_user, lm_step);
}

bool UserSentence::SaveCounts(const std::filesystem::path& path, std::string_view vocab_sig) const {
    std::ofstream out(path);
    if (!out.is_open()) {
        return false;
    }
    out.precision(17);
    out << "SIME_USER_COUNTS_EXP1\n" << kVocabPrefix << vocab_sig << '\n';
    out << "CLOCK " << clock_ << '\n';
    out << "TOTAL " << total_unigram_count_ << ' ' << aged_total_.count << ' ' << aged_total_.last << '\n';
    std::vector<std::pair<std::uint64_t, std::string>> lines;
    auto row = [](auto key, std::uint32_t n, const Aged& a) {
        std::ostringstream s;
        s.precision(17);
        s << key << ' ' << n << ' ' << a.count << ' ' << a.last;
        return s.str();
    };
    for (const auto& [k, n] : unigram_) {
        auto it = aged_uni_.find(k);
        lines.emplace_back(k, "U " + row(k, n, it == aged_uni_.end() ? Aged{} : it->second));
    }
    for (const auto& [k, n] : distinct_successors_) {
        auto it = aged_succ_.find(k);
        lines.emplace_back(k, "S " + row(k, n, it == aged_succ_.end() ? Aged{} : it->second));
    }
    for (const auto& [k, n] : bigram_) {
        auto it = aged_bi_.find(k);
        lines.emplace_back(k, "B " + row(k, n, it == aged_bi_.end() ? Aged{} : it->second));
    }
    std::sort(lines.begin(), lines.end());
    for (const auto& l : lines) {
        out << l.second << '\n';
    }
    return static_cast<bool>(out);
}

bool UserSentence::LoadCounts(const std::filesystem::path& path, std::string_view vocab_sig) {
    std::ifstream in(path);
    std::string line;
    if (!in.is_open() || !std::getline(in, line) || line != "SIME_USER_COUNTS_EXP1" || !std::getline(in, line) ||
        (!vocab_sig.empty() && line != std::string(kVocabPrefix) + std::string(vocab_sig))) {
        return false;
    }
    LearnConfig keep = learn_;
    Clear();
    learn_ = keep;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string tag;
        s >> tag;
        if (tag == "CLOCK") {
            s >> clock_;
        } else if (tag == "TOTAL") {
            s >> total_unigram_count_ >> aged_total_.count >> aged_total_.last;
        } else {
            std::uint64_t key = 0, last = 0;
            std::uint32_t n = 0;
            float_t count = 0.0;
            s >> key >> n >> count >> last;
            const Aged a{count, last};
            if (tag == "U") {
                unigram_[static_cast<TokenID>(key)] = n;
                aged_uni_[static_cast<TokenID>(key)] = a;
            } else if (tag == "S") {
                distinct_successors_[static_cast<TokenID>(key)] = n;
                aged_succ_[static_cast<TokenID>(key)] = a;
            } else if (tag == "B") {
                bigram_[key] = n;
                aged_bi_[key] = a;
            }
        }
    }
    return true;
}
#endif  // SIME_WEAVE_LEARNING

std::uint64_t UserSentence::BigramKey(TokenID prev, TokenID cur) {
    return (static_cast<std::uint64_t>(prev) << 32U) |
           static_cast<std::uint64_t>(cur);
}

UserSentence::Sentence UserSentence::CleanSentence(const Sentence& sentence) {
    Sentence clean;
    clean.reserve(sentence.size());
    for (auto token : sentence) {
        if (token != NotToken) {
            clean.push_back(token);
        }
    }
    return clean;
}

UserSentence::Sentence UserSentence::Flatten(const Entry& entry) {
    Sentence flat;
    std::size_t total = 0;
    for (const auto& sel : entry.selections) {
        total += sel.tokens.size();
    }
    flat.reserve(total);
    for (const auto& sel : entry.selections) {
        flat.insert(flat.end(), sel.tokens.begin(), sel.tokens.end());
    }
    return flat;
}

bool UserSentence::EndsWith(const Sentence& sentence, const Sentence& suffix) {
    if (suffix.size() > sentence.size()) {
        return false;
    }
    return std::equal(suffix.rbegin(), suffix.rend(), sentence.rbegin());
}

void UserSentence::StartParagraph(Selection selection) {
    if (max_sentence_count_ == 0) {
        return;
    }
    Entry entry;
    entry.selections.push_back(std::move(selection));
    AdjustEntryCounts(entry, +1);
    sentences_.push_front(std::move(entry));
    while (sentences_.size() > max_sentence_count_) {
        AdjustEntryCounts(sentences_.back(), -1);
        sentences_.pop_back();
    }
}

void UserSentence::AppendToLatest(Selection selection) {
    if (sentences_.empty()) {
        StartParagraph(std::move(selection));
        return;
    }
    auto& latest = sentences_.front();

    // Incremental count update: tail of existing flattened tokens is
    // the "previous" for the first token of the new selection. After
    // that, internal bigrams of the new selection contribute too.
    TokenID prev_tail = NotToken;
    if (!latest.selections.empty()) {
        const auto& last_sel = latest.selections.back();
        if (!last_sel.tokens.empty()) {
            prev_tail = last_sel.tokens.back();
        }
    }
#if SIME_WEAVE_LEARNING
    {
        TokenID prev = prev_tail;
        for (auto token : selection.tokens) {
            AgeToken(prev, token);
            prev = token;
        }
    }
#endif
    for (auto token : selection.tokens) {
        IncCount(unigram_, token, 1);
        ++total_unigram_count_;
    }
    if (prev_tail != NotToken && !selection.tokens.empty()) {
        IncBigramTrackSucc(bigram_, distinct_successors_, prev_tail,
                           BigramKey(prev_tail, selection.tokens.front()), 1);
    }
    for (std::size_t i = 1; i < selection.tokens.size(); ++i) {
        IncBigramTrackSucc(bigram_, distinct_successors_,
                           selection.tokens[i - 1],
                           BigramKey(selection.tokens[i - 1],
                                     selection.tokens[i]), 1);
    }

    latest.selections.push_back(std::move(selection));
}

void UserSentence::AdjustEntryCounts(const Entry& entry, int delta) {
    if (delta == 0) {
        return;
    }
    Sentence flat = Flatten(entry);
    if (flat.empty()) {
        return;
    }
    if (delta > 0) {
        auto d = static_cast<std::uint32_t>(delta);
#if SIME_WEAVE_LEARNING
        for (std::size_t i = 0; i < flat.size(); ++i) {
            AgeToken(i ? flat[i - 1] : NotToken, flat[i]);
        }
#endif
        for (auto token : flat) {
            IncCount(unigram_, token, d);
            total_unigram_count_ += d;
        }
        for (std::size_t i = 1; i < flat.size(); ++i) {
            IncBigramTrackSucc(bigram_, distinct_successors_, flat[i - 1],
                               BigramKey(flat[i - 1], flat[i]), d);
        }
    } else {
        auto d = static_cast<std::uint32_t>(-delta);
        for (auto token : flat) {
            DecCount(unigram_, token, d);
            if (total_unigram_count_ >= d) {
                total_unigram_count_ -= d;
            } else {
                total_unigram_count_ = 0;
            }
        }
        for (std::size_t i = 1; i < flat.size(); ++i) {
            DecBigramTrackSucc(bigram_, distinct_successors_, flat[i - 1],
                               BigramKey(flat[i - 1], flat[i]), d);
        }
    }
}

void UserSentence::Rebuild() {
    unigram_.clear();
    bigram_.clear();
    distinct_successors_.clear();
    total_unigram_count_ = 0;
    for (const auto& entry : sentences_) {
        AdjustEntryCounts(entry, +1);
    }
}

} // namespace sime
