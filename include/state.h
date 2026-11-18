// Modified by the weave-ime project (2026): adds SIME_WEAVE_BEAM_EXACT (exact column eviction), SIME_WEAVE_COLUMN_BEAM_SIZE (column state cap, default 48) and the SIME_WEAVE_BEAM_CHECK invariant check; weave default BEAM_EXACT=1; State keeps the edge's flags (backtrace_flags, additive, no macro) (see NOTICE.weave.md); adds State::kEdgeFuzzy (SIME_WEAVE_FUZZY); adds State::kEdgeDup (SIME_WEAVE_DUP_EDGE) and State::kEdgeUeAlias (SIME_WEAVE_UE_ALIAS); adds State::kEdgeNeighbour (SIME_WEAVE_NEIGHBOUR_EDGE, experiment); adds State::kEdgeEnTypo (SIME_WEAVE_EN_TYPO, experiment); backtrace_flags widened to 16 bits (same padding, sizeof(State) unchanged) for State::kEdgeExtra (SIME_WEAVE_EXTRA_EDGE, experiment). Adds SIME_WEAVE_TAIL_BEAM_SIZE (NetStates::SetMaxColumn, the last column's cap; weave-ime Phase 5 Task 21, default 0).
#pragma once

#include "common.h"
#include "score.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

// SIME_WEAVE_BEAM_EXACT=1: NetStates::Insert keeps each bucket's key in the
// score-heap equal to the bucket's current worst score (and every bucket's
// heap_idx current), and counts a push that replaces a full bucket's worst
// state as no growth, so the column keeps its SIME_WEAVE_COLUMN_BEAM_SIZE best
// states: eviction runs only when the column is over the cap and always
// removes the globally worst state. 0 = upstream (a push into an existing
// bucket leaves its key stale, so eviction can remove a better state from
// another bucket; a replacement increments the state count, so the column
// evicts states it has room for). Default 1 since weave-ime Phase 2 (Task 8).
#ifndef SIME_WEAVE_BEAM_EXACT
#define SIME_WEAVE_BEAM_EXACT 1  // upstream: 0
#endif
// SIME_WEAVE_COLUMN_BEAM_SIZE: states kept per lattice column (NetStates'
// cap across all LM-state buckets). Default 48 = upstream.
#ifndef SIME_WEAVE_COLUMN_BEAM_SIZE
#define SIME_WEAVE_COLUMN_BEAM_SIZE 48
#endif
// SIME_WEAVE_TAIL_BEAM_SIZE (weave-ime Phase 5 Task 21, the p5u5 freeze): with N > 0 the
// sentence decoder's last column (the end of the input) keeps N states per LM-state bucket
// and max(N, SIME_WEAVE_COLUMN_BEAM_SIZE) in all (NetStates::SetMaxColumn), and its N-best
// scan reads the first N, so up to N whole readings come out instead of
// SIME_WEAVE_BEAM_SIZE. Full and incremental decodes both (src/sime.cc). Every other column,
// and DecodeStr, the T9 path and GetTokens, are unchanged. 0 (the default) = upstream: the
// last column is capped like the others. N must be 0 or at least SIME_WEAVE_BEAM_SIZE.
#ifndef SIME_WEAVE_TAIL_BEAM_SIZE
#define SIME_WEAVE_TAIL_BEAM_SIZE 0
#endif
// SIME_WEAVE_BEAM_CHECK=1 (debug builds only; needs SIME_WEAVE_BEAM_EXACT=1):
// Insert aborts when BeamInvariantHolds() fails after it.
#ifndef SIME_WEAVE_BEAM_CHECK
#define SIME_WEAVE_BEAM_CHECK 0
#endif
#if SIME_WEAVE_BEAM_CHECK && !SIME_WEAVE_BEAM_EXACT
#error "SIME_WEAVE_BEAM_CHECK=1 checks the exact-beam invariant; it needs SIME_WEAVE_BEAM_EXACT=1"
#endif

namespace sime {

struct State {
    float_t score = 0.0;
    std::size_t now = 0;
    Scorer::Pos pos{};
    const State* backtrace_state = nullptr;
    TokenID backtrace_token = 0;
    // weave-ime (additive, no macro): the flags of the edge that led here
    // (kEdgeEnglish, kEdgeExpansion), so Backtrace can rebuild them for the
    // candidate contract. Fills padding after backtrace_token: sizeof(State)
    // is unchanged on 64-bit targets.
    std::uint16_t backtrace_flags = 0;
    const char* backtrace_pieces = nullptr;

    static constexpr std::uint8_t kEdgeEnglish = 1;
    static constexpr std::uint8_t kEdgeExpansion = 2;
    static constexpr std::uint8_t kEdgeSwap = 4;  // SIME_WEAVE_SWAP_EDGE: an edge that reads swapped letters
    static constexpr std::uint8_t kEdgeFuzzy = 8;  // SIME_WEAVE_FUZZY: an edge that reads a nasal final as its partner
    static constexpr std::uint8_t kEdgeDup = 16;   // SIME_WEAVE_DUP_EDGE: an edge that reads a doubled letter once
    static constexpr std::uint8_t kEdgeUeAlias = 32;  // SIME_WEAVE_UE_ALIAS: an edge that reads typed lue/nue as lve/nve
    static constexpr std::uint8_t kEdgeNeighbour = 64;  // SIME_WEAVE_NEIGHBOUR_EDGE: an edge that reads a letter as its QWERTY neighbour
    static constexpr std::uint8_t kEdgeEnTypo = 128;  // SIME_WEAVE_EN_TYPO: an English edge read with one keyboard slip
    static constexpr std::uint16_t kEdgeExtra = 256;  // SIME_WEAVE_EXTRA_EDGE: an edge that skips one stray letter

    State() = default;

    State(float_t score,
          std::size_t now,
          Scorer::Pos pos,
          const State* backtrace_state,
          TokenID backtrace_token,
          const char* backtrace_pieces = nullptr,
          std::uint16_t backtrace_flags = 0);

    bool operator<(const State& r) const {
        return score < r.score;
    }
};

class TopStates {
public:
    explicit TopStates(std::size_t t);

    bool Push(const State& state);
    void Pop();

    const State& Top() const { return heap_.front(); }
    std::size_t Size() const { return heap_.size(); }

    using iterator = std::vector<State>::iterator;
    using const_iterator = std::vector<State>::const_iterator;

    iterator begin() { return heap_.begin(); }
    iterator end() { return heap_.end(); }
    const_iterator begin() const { return heap_.begin(); }
    const_iterator end() const { return heap_.end(); }

    // Cached position of this bucket inside NetStates::top_score_; updated by
    // NetStates as it shuffles the score-heap. Avoids a second map lookup
    // per Insert (was: top_index_ map keyed on Scorer::Pos).
    std::size_t heap_idx = 0;
    Scorer::Pos pos{};

private:
    std::vector<State> heap_;
    std::size_t size_;
};

class NetStates {
public:
    NetStates();

    void SetMaxTop(std::size_t max_top) { max_top_ = max_top; }
#if SIME_WEAVE_TAIL_BEAM_SIZE
    // The column's state cap across all buckets (default SIME_WEAVE_COLUMN_BEAM_SIZE); the
    // sentence decoder raises it for the last column only.
    void SetMaxColumn(std::size_t max_column) { max_column_ = max_column; }
#endif
    void Clear();
    void Insert(const State& state);

    std::vector<State> GetStates() const;

    // Exact-beam invariant: top_score_ is a max-heap on its keys, holds one
    // entry per bucket, each key equals its bucket's worst (largest) score,
    // each bucket's heap_idx points at its entry, and state_size_ is the
    // total state count, at most the column cap. Holds after every Insert when
    // SIME_WEAVE_BEAM_EXACT=1; upstream keeps only the heap property.
    bool BeamInvariantHolds() const;

private:
    using PosMap = std::map<Scorer::Pos, TopStates>;

public:
    class iterator {
    public:
        iterator() = default;
        iterator(PosMap::iterator outer, PosMap::iterator outer_end);

        iterator& operator++();
        bool operator!=(const iterator& rhs) const;
        State& operator*() const;
        State* operator->() const;

    private:
        PosMap::iterator outer_{};
        PosMap::iterator outer_end_{};
        TopStates::iterator inner_{};

        void SkipEmpty();
    };

    iterator begin();
    iterator end();

private:
    void PushScoreHeap(float_t score, TopStates* bucket);
    void PopScoreHeap();
    void RefreshTopIndex(std::size_t index);
    void AdjustUp(std::size_t node);
    void AdjustDown(std::size_t node);

private:
    static constexpr std::size_t BeamSize = SIME_WEAVE_COLUMN_BEAM_SIZE;
#if SIME_WEAVE_TAIL_BEAM_SIZE
    std::size_t max_column_ = BeamSize;
#endif

    PosMap pos_map_;
    std::size_t state_size_ = 0;
    std::size_t max_top_ = 2;

    // Score-heap of (worst-score-in-bucket, bucket-ptr). Bucket pointers are
    // stable because std::map node addresses don't move on insert/erase of
    // other keys. Each bucket caches its index here in TopStates::heap_idx.
    std::vector<std::pair<float_t, TopStates*>> top_score_;
};

} // namespace sime
