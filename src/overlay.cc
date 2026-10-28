// Added by the weave-ime project (2026): the lexicon overlay behind SIME_WEAVE_OVERLAY (default 1 since Task 8c; compiles to nothing when built with -DSIME_WEAVE_OVERLAY=0): layer files, cross-layer merge, bounded costs, L0 coverage and the overlay English DAT; the contract's spelling slot and L0 cover sources (additive) (see NOTICE.weave.md).
#include "overlay.h"

#if SIME_WEAVE_OVERLAY

#include "ustr.h"

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>

namespace sime {

namespace {

const char* const kSlotNames[kLexLayers] = {"L1", "L2", "L3", "L4"};

// Folded spelling for the engine-side comparisons (ASCII letters lowercased, spaces and
// U+2581 removed; other characters compared as they are). The merge key itself (NFC,
// full case folding) comes from the build tool.
std::u32string FoldSpelling(std::u32string_view s) {
    std::u32string out;
    for (char32_t c : s) {
        if (c == U' ' || c == 0x2581) continue;
        out.push_back((c >= U'A' && c <= U'Z') ? c - U'A' + U'a' : c);
    }
    return out;
}

bool PlainLetters(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::size_t b = 0;
    for (;;) {
        const auto e = line.find('\t', b);
        out.push_back(line.substr(b, e == std::string::npos ? std::string::npos : e - b));
        if (e == std::string::npos) return out;
        b = e + 1;
    }
}

bool KindAllowed(int slot, const std::string& kind) {
    switch (slot) {
    case 0: return kind == "word";
    case 2: return kind == "tech" || kind == "brand" || kind == "new";
    case 3: return kind == "user";
    default: return false;
    }
}

}  // namespace

void Overlay::Clear() {
    config_ = OverlayConfig{};
    for (int i = 0; i < kLexLayers; ++i) {
        rows_[i].clear();
        version_[i].clear();
    }
    zipf_.clear();
    tokens_.clear();
    by_key_.clear();
    dat_ = trie::DoubleArray();
    l0_canonical_.clear();
    l0_sources_.clear();
    stats_ = OverlayStats{};
}

double Overlay::Bonus(const std::string& kind) const {
    if (kind == "word") return config_.bonus_word;
    if (kind == "tech") return config_.bonus_tech;
    if (kind == "brand") return config_.bonus_brand;
    if (kind == "new") return config_.bonus_new;
    return config_.bonus_user;  // "user"
}

bool Overlay::ReadLayer(int slot, std::string* error) {
    const auto& path = config_.files[slot];
    auto fail = [&](std::size_t line, const std::string& what) {
        if (error) {
            *error = path.string() + (line ? ":" + std::to_string(line) : std::string()) + ": " + what;
        }
        return false;
    };
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail(0, "cannot open");
    std::string line;
    std::size_t n = 0;
    bool format = false, layer = false;
    while (std::getline(in, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line == "#weave-lexicon-layer 1") format = true;
            else if (line.rfind("#layer ", 0) == 0) {
                if (line.substr(7) != kSlotNames[slot]) {
                    return fail(n, "is a " + line.substr(7) + " file, loaded as " + kSlotNames[slot]);
                }
                layer = true;
            } else if (line.rfind("#version ", 0) == 0) {
                version_[slot] = line.substr(9);
            }
            continue;
        }
        if (!format || !layer || version_[slot].empty()) {
            return fail(n, "missing #weave-lexicon-layer 1 / #layer / #version header");
        }
        const auto cols = SplitTabs(line);
        if (slot == static_cast<int>(LexLayer::L2)) {
            if (cols.size() != 2 || cols[0].empty()) return fail(n, "an L2 row is key<TAB>zipf");
            errno = 0;
            char* end = nullptr;
            const double z = std::strtod(cols[1].c_str(), &end);
            if (errno || end == cols[1].c_str() || *end != '\0') return fail(n, "bad zipf " + cols[1]);
            auto& cur = zipf_[cols[0]];
            cur = std::max(cur, z);
            ++stats_.rows[slot];
            continue;
        }
        if (cols.size() < 4) return fail(n, "a row is key<TAB>typed<TAB>spelling<TAB>kind");
        if (cols[0].empty()) return fail(n, "empty key");
        if (!PlainLetters(cols[1])) return fail(n, "typed key '" + cols[1] + "' is not lowercase a-z");
        if (cols[2].empty()) return fail(n, "empty spelling");
        if (!KindAllowed(slot, cols[3])) {
            return fail(n, "kind '" + cols[3] + "' is not allowed in " + kSlotNames[slot]);
        }
        rows_[slot].push_back({cols[0], cols[1], cols[2], cols[3]});
        ++stats_.rows[slot];
    }
    if (!format || !layer || version_[slot].empty()) {
        return fail(0, "missing #weave-lexicon-layer 1 / #layer / #version header");
    }
    return true;
}

bool Overlay::Load(const OverlayConfig& config, const Dict& l0, std::string* error) {
    Clear();
    config_ = config;
    for (int slot = 0; slot < kLexLayers; ++slot) {
        if (config_.files[slot].empty()) continue;
        if (!ReadLayer(slot, error)) {
            Clear();
            return false;
        }
        stats_.version[slot] = version_[slot];
    }
    Rebuild(l0);
    return true;
}

bool Overlay::SetEnabled(LexLayer layer, bool enabled, const Dict& l0) {
    const int slot = static_cast<int>(layer);
    if (slot < 0 || slot >= kLexLayers) return false;
    config_.enabled[slot] = enabled;
    Rebuild(l0);
    return true;
}

void Overlay::Rebuild(const Dict& l0) {
    tokens_.clear();
    by_key_.clear();
    dat_ = trie::DoubleArray();
    l0_canonical_.clear();
    l0_sources_.clear();
    stats_.entries = stats_.multi_source = stats_.forms = 0;
    stats_.covered_by_l0 = stats_.overlay_forms = 0;

    struct Form {
        std::string spelling;
        int slot;
    };
    struct Ent {
        std::string key;
        std::vector<std::string> keys;
        unsigned sources = 0;
        double bonus = -std::numeric_limits<double>::infinity();
        std::map<std::string, Form> forms;  // typed -> spelling of the highest layer
    };
    std::vector<Ent> ents;
    std::vector<std::size_t> parent;
    std::unordered_map<std::string, std::size_t> by_key, by_form;
    auto find = [&](std::size_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    auto unite = [&](std::size_t a, std::size_t b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    };

    // Word layers in priority order, so a later (higher) layer's spelling overwrites.
    for (int slot : {0, 2, 3}) {
        if (!config_.enabled[slot]) continue;
        for (const auto& r : rows_[slot]) {
            auto [it, fresh] = by_key.emplace(r.key, ents.size());
            if (fresh) {
                ents.push_back({r.key, {r.key}, 0, -std::numeric_limits<double>::infinity(), {}});
                parent.push_back(parent.size());
            }
            auto& e = ents[it->second];
            e.sources |= 1u << slot;
            e.bonus = std::max(e.bonus, Bonus(r.kind));
            e.forms[r.typed] = {r.spelling, slot};
            // Two entries sharing a typed key with the same folded spelling are one word.
            std::string fk = r.typed + '\t' + ustr::FromU32(FoldSpelling(ustr::ToU32(r.spelling)));
            auto [f, ffresh] = by_form.emplace(std::move(fk), it->second);
            if (!ffresh) unite(f->second, it->second);
        }
    }
    for (std::size_t i = 0; i < ents.size(); ++i) {
        const std::size_t r = find(i);
        if (r == i) continue;
        auto& root = ents[r];
        auto& e = ents[i];
        root.sources |= e.sources;
        root.bonus = std::max(root.bonus, e.bonus);
        root.keys.insert(root.keys.end(), e.keys.begin(), e.keys.end());
        for (auto& [typed, form] : e.forms) {
            auto [it, fresh] = root.forms.emplace(typed, form);
            if (!fresh && form.slot >= it->second.slot) it->second = form;
        }
    }
    std::vector<std::size_t> roots;
    for (std::size_t i = 0; i < ents.size(); ++i) {
        if (find(i) == i) roots.push_back(i);
    }
    std::sort(roots.begin(), roots.end(),
              [&](std::size_t a, std::size_t b) { return ents[a].key < ents[b].key; });

    const bool use_l2 = config_.enabled[static_cast<int>(LexLayer::L2)] && !zipf_.empty();
    const double span = config_.zipf_hi - config_.zipf_lo;
    std::map<std::string, std::vector<std::uint32_t>> dat_keys;
    for (std::size_t n = 0; n < roots.size(); ++n) {
        const auto& e = ents[roots[n]];
        double z = 0.0;
        if (use_l2) {
            for (const auto& k : e.keys) {
                auto it = zipf_.find(k);
                if (it != zipf_.end()) z = std::max(z, it->second);
            }
        }
        const double pos = span > 0 ? std::clamp((config_.zipf_hi - z) / span, 0.0, 1.0) : 1.0;
        const double base = config_.floor + config_.band * pos;
        const double cost = std::max(config_.floor, base - e.bonus);
        ++stats_.entries;
        if (std::bitset<kLexLayers>(e.sources).count() > 1) ++stats_.multi_source;
        for (const auto& [typed, form] : e.forms) {
            ++stats_.forms;
            const std::u32string sp32 = ustr::ToU32(form.spelling);
            const std::u32string folded = FoldSpelling(sp32);
            bool covered = false;
            std::uint32_t v = 0;
            if (l0.Dat(Dict::LetterEn).Get(typed, v)) {
                const auto entry = l0.GetEntry(Dict::LetterEn, v);
                std::vector<TokenID> ids;
                for (std::uint32_t i = 0; i < entry.count; ++i) ids.push_back(entry.items[i].id);
                for (TokenID id : ids) {
                    const char32_t* t = l0.TokenAt(id);
                    if (t && FoldSpelling(t) == folded) {
                        covered = true;
                        // Only the term pack and the user dictionary may re-case an L0 word;
                        // ESDB's casing (lowercase when any) marks a common noun, not a
                        // canonical form, so L1 only adds words L0 lacks.
                        if (form.slot >= static_cast<int>(LexLayer::L3)) {
                            l0_canonical_[id] = sp32;
                            l0_sources_[id] |= e.sources & ((1u << static_cast<int>(LexLayer::L3)) |
                                                            (1u << static_cast<int>(LexLayer::L4)));
                        }
                    }
                }
            }
            if (covered) {
                ++stats_.covered_by_l0;
                continue;
            }
            dat_keys[typed].push_back(static_cast<std::uint32_t>(tokens_.size()));
            tokens_.push_back({typed, form.spelling, sp32, static_cast<float_t>(cost),
                               static_cast<std::uint32_t>(n), e.sources, form.slot});
        }
    }
    stats_.overlay_forms = tokens_.size();
    if (tokens_.empty()) return;
    std::vector<std::string> keys;
    std::vector<std::uint32_t> values;
    for (auto& [k, ids] : dat_keys) {
        keys.push_back(k);
        values.push_back(static_cast<std::uint32_t>(by_key_.size()));
        by_key_.push_back(std::move(ids));
    }
    dat_.Build(keys, values);
}

} // namespace sime

#endif  // SIME_WEAVE_OVERLAY
