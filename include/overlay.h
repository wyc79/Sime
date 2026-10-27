// Added by the weave-ime project (2026): the lexicon overlay (SIME_WEAVE_OVERLAY, default 0): extra English words from versioned layer files (L1 ESDB, L2 wordfreq, L3 term pack, L4 user), merged across layers, at a bounded cost; tokens keep the layer of their spelling and L0 words the L3/L4 layers that list them, for the candidate contract (additive) (see NOTICE.weave.md).
#pragma once

// SIME_WEAVE_OVERLAY=1: Sime can load lexicon layer files (weave-ime
// Docs/plans/phase-4.5-lexicon.md) and adds their words as English edges next to
// the dictionary's own, at a cost held in a band set from the L2 frequency. With 0
// (the default) none of this is compiled.
#ifndef SIME_WEAVE_OVERLAY
#define SIME_WEAVE_OVERLAY 0
#endif

#if SIME_WEAVE_OVERLAY

#include "common.h"
#include "dict.h"
#include "trie.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace sime {

// Layer slots, lowest priority first. L2 is a cost (frequencies), not a word source.
enum class LexLayer : int { L1 = 0, L2 = 1, L3 = 2, L4 = 3 };
constexpr int kLexLayers = 4;

struct OverlayConfig {
    // One layer file per slot; an empty path leaves the slot unloaded.
    std::filesystem::path files[kLexLayers];
    bool enabled[kLexLayers] = {true, true, true, true};
    // cost = max(floor, floor + band * clamp((zipf_hi - z) / (zipf_hi - zipf_lo), 0, 1) - bonus),
    // z = the entry's best L2 Zipf frequency (0 when none); an entry takes the largest bonus of
    // its sources' kinds. In the LM's cost unit; English edge penalties come on top.
    double floor = 18.0;
    double band = 6.0;
    double zipf_lo = 1.0;
    double zipf_hi = 6.0;
    double bonus_word = 0.0;   // L1
    double bonus_tech = 0.0;   // L3 kinds
    double bonus_brand = 0.0;
    double bonus_new = 0.0;
    double bonus_user = 6.0;   // L4: a user word sits at the floor
};

struct OverlayStats {
    std::string version[kLexLayers];  // each loaded layer's #version ("" = not loaded)
    std::size_t rows[kLexLayers] = {};
    std::size_t entries = 0;          // merged entries from the enabled word layers
    std::size_t multi_source = 0;     // entries with more than one source layer
    std::size_t forms = 0;            // typed keys over all entries
    std::size_t covered_by_l0 = 0;    // typed keys L0 already has with the same spelling (no edge)
    std::size_t overlay_forms = 0;    // typed keys that add edges
};

class Overlay {
public:
    // Overlay token IDs: kTokenBase + index. Outside the dictionary and the LM.
    static constexpr TokenID kTokenBase = 0xE0000000u;
    static constexpr TokenID kTokenEnd = 0xF0000000u;
    static bool IsToken(TokenID id) { return id >= kTokenBase && id < kTokenEnd; }

    struct Token {
        std::string typed;          // the typed key (lowercase a-z); also the edge's pieces
        std::string spelling;       // canonical display (UTF-8), from the highest layer
        std::u32string spelling32;
        float_t cost = 0.0;
        std::uint32_t entry = 0;
        unsigned sources = 0;       // bit i = layer slot i contributed
        int slot = 0;               // the layer slot this typed key's spelling came from
    };

    // Reads and merges the layer files. On failure returns false, sets *error and leaves
    // the overlay empty.
    bool Load(const OverlayConfig& config, const Dict& l0, std::string* error);
    bool SetEnabled(LexLayer layer, bool enabled, const Dict& l0);
    void Clear();

    bool Empty() const { return tokens_.empty() && l0_canonical_.empty(); }
    bool HasEdges() const { return !tokens_.empty(); }
    const trie::DoubleArray& Dat() const { return dat_; }
    const std::vector<std::uint32_t>& TokensAt(std::uint32_t value) const { return by_key_[value]; }
    const Token& At(std::uint32_t index) const { return tokens_[index]; }
    const Token* TokenOf(TokenID id) const {
        return IsToken(id) && id - kTokenBase < tokens_.size() ? &tokens_[id - kTokenBase] : nullptr;
    }
    float_t Cost(TokenID id) const { return tokens_[id - kTokenBase].cost; }
    // Canonical display of an L0 token that an L3 or L4 spelling covers (nullptr if none; L1
    // never re-cases an L0 word).
    const std::u32string* CanonicalForL0(TokenID id) const {
        auto it = l0_canonical_.find(id);
        return it == l0_canonical_.end() ? nullptr : &it->second;
    }
    // Layer-slot bits (L3, L4) of the merged entries whose spelling covers an L0 token (0 if none):
    // the candidate contract's sources for that L0 word.
    unsigned L0Sources(TokenID id) const {
        auto it = l0_sources_.find(id);
        return it == l0_sources_.end() ? 0u : it->second;
    }
    const OverlayStats& Stats() const { return stats_; }

private:
    struct Row {
        std::string key, typed, spelling, kind;
    };
    bool ReadLayer(int slot, std::string* error);
    void Rebuild(const Dict& l0);
    double Bonus(const std::string& kind) const;

    OverlayConfig config_;
    std::vector<Row> rows_[kLexLayers];
    std::unordered_map<std::string, double> zipf_;  // L2
    std::string version_[kLexLayers];

    std::vector<Token> tokens_;
    std::vector<std::vector<std::uint32_t>> by_key_;  // DAT value -> token indices
    trie::DoubleArray dat_;
    std::unordered_map<TokenID, std::u32string> l0_canonical_;
    std::unordered_map<TokenID, unsigned> l0_sources_;
    OverlayStats stats_;
};

} // namespace sime

#endif  // SIME_WEAVE_OVERLAY
