# weave-ime modifications

This is a fork of Ismantic/Sime (Apache-2.0), branch `weave`, maintained by
the weave-ime project. Upstream LICENSE and attribution are unchanged.
Per Apache-2.0 section 4, files modified in this fork carry a header line
`Modified by the weave-ime project (2026): ...`.

## Default behaviour (changed in weave-ime Phase 2, Task 8)

Each change below is a `SIME_WEAVE_*` macro guarded by `#ifndef`. Until
weave-ime Phase 2 Task 8 every default reproduced upstream. Since then seven
defaults differ from upstream, since Phase 4.5 Task 8c nine, since Phase 5 unfreeze Task 4 twelve, since Phase 5 unfreeze 2 (2026-11-13) fourteen, and on the p5u3 candidate (local weave, 2026-11-15) sixteen, so **a build with no `-D` defines
no longer behaves like upstream Sime**:

| macro | weave default | upstream value | effect of the weave default |
|---|---|---|---|
| `SIME_WEAVE_PENALTY_RULE` (`include/sime.h`) | 1 | 0 | scoped English penalty (whole-input words keep the upstream penalties) |
| `SIME_WEAVE_EN_PENALTY_MIXED` (`include/sime.h`) | 0.0 | 8.0 | English edges inside longer input pay no extra penalty ... |
| `SIME_WEAVE_EN_SHORT_EDGE_LEN` (`include/sime.h`) | 2 | 0 | ... unless they are at most 2 letters long (then `EnglishPenaltyShort`, 4.0) |
| `SIME_WEAVE_EN_PENALTY_SHORT` (`include/sime.h`) | 4.0 | 8.0 | the penalty for those short edges (and for edges over digits only) |
| `SIME_WEAVE_CASE_FOLD` (`include/sime.h`) | 1 | 0 | English lookups are case-insensitive with a lowercase-key dictionary; typed capitals still select their casing (see below for each dictionary) |
| `SIME_WEAVE_RAW_CNT` (`include/sime.h`) | 1 | 0 | `DecodeSentence` reports `DecodeResult::cnt` in raw input bytes (Layer 2 changes; `DecodeNumSentence` and `DecodeStr` are unchanged) |
| `SIME_WEAVE_BEAM_EXACT` (`include/state.h`) | 1 | 0 | exact column beam (fixes upstream's stale bucket keys and state-count drift) |
| `SIME_WEAVE_OVERLAY` (`include/overlay.h`; Phase 4.5 Task 8c) | 1 | 0 (no overlay) | the lexicon overlay is compiled in; it stays empty, and decoding unchanged, until `LoadOverlay` |
| `SIME_WEAVE_INCREMENTAL` (`include/sime.h`; Phase 4.5 Task 8c) | 1 | 0 (no incremental decoding) | `DecodeSentence` reuses the previous decode of the composition (output equal to a full decode); `SetIncremental(false)` switches it off |
| `SIME_WEAVE_LEARNING` (`include/user.h`; Phase 5 unfreeze Task 4) | 1 | 0 (no learning) | the learning store's knobs, strict file and merge are compiled in; with every knob at its default the model is unchanged |
| `SIME_WEAVE_SWAP_EDGE` (`include/sime.h`; Phase 5 unfreeze Task 4) | 1 | 0 (no swap edges) | swapped-letter edges are compiled in; none are added until `SetSwapCost` gets a cost of 0 or more |
| `SIME_WEAVE_READING_PRIOR` (`include/sime.h`; Phase 5 unfreeze Task 4) | 1 | 0 (no table) | the rare-reading table is compiled in; nothing changes until `LoadReadingPrior` gets a table |
| `SIME_WEAVE_FUZZY` (`include/sime.h`; Phase 5 unfreeze 2) | 1 | 0 (no fuzzy edges) | front/back-nasal fuzzy edges are compiled in; none are added until `SetFuzzyCost` gives a pair a cost |
| `SIME_WEAVE_RIGHT_CONTEXT` (`include/sime.h`; Phase 5 unfreeze 2) | 1 | 0 (no `TextCost`) | `Sime::TextCost` is compiled in; nothing calls it but the host |
| `SIME_WEAVE_DUP_EDGE` (`include/sime.h`; p5u3 candidate) | 1 | 0 (no doubled-letter edges) | doubled-letter edges are compiled in; none are added until `SetDupCost` gets a cost of 0 or more |
| `SIME_WEAVE_UE_ALIAS` (`include/sime.h`; p5u3 candidate) | 1 | 0 (no lue/nue alias) | lue/nue alias edges are compiled in; none are added until `SetUeAliasCost` gets a cost of 0 or more |
| `SIME_WEAVE_NEIGHBOUR_EDGE` (`include/sime.h`; p5u4 candidate) | 1 | 0 (no neighbour edges) | QWERTY-neighbour edges are compiled in; none are added until `SetNeighbourCost` gets a cost of 0 or more |
| `SIME_WEAVE_EN_TYPO` (`include/sime.h`; experiment, local branch `weave-nb`) | 0 | 0 (no English slip edges) | with 1, English slip edges are compiled in; none are added until `SetEnTypoCost` gives a kind a cost of 0 or more |
| `SIME_WEAVE_EXTRA_EDGE` (`include/sime.h`; p5u4 candidate) | 1 | 0 (no stray-key edges) | stray-key edges are compiled in; none are added until `SetExtraCost` gets a cost of 0 or more |

**Swapped-letter edges (local branch `weave-unfreeze`, weave-ime Phase 5 unfreeze Task 2; default 1 since Task 4, was 0).**
`SIME_WEAVE_SWAP_EDGE` (`include/sime.h`, default 1: compiled in, and no edge is added until `SetSwapCost` gets a cost of 0 or more): `Sime::AppendSwapEdges`, at the end of
`BuildSentenceNet`, adds extra pinyin edges for words reached with two adjacent typed letters swapped, at the runtime
cost of `SetSwapCost(cost, min_span)` (a negative cost, the initial value, is off; `min_span` is the shortest swap
edge in typed bytes, default 2). Doubled letters are not corrected. The swap members compile with the overlay and
incremental blocks on or off. A swap edge takes no learning adjustment, and neither does the step after it (`Relax`;
`State::kEdgeSwap`): a picked correction teaches the pairs around its word, and a path through them would otherwise
beat the clean reading of a neighbouring input by more than the swap cost. Swap edges also skip the reading table
(`ApplyReadingPrior` runs before they are added): a listed reading typed with swapped letters pays nothing, and a
`drop` reading can come back through a swap edge. The shipped table has no `drop` line.
Exact edges are not changed or filtered. English lookups, `DecodeStr`, `GetTokens` and the T9 path are not touched.
Touches `include/sime.h`, `src/sime.cc`.

**Front/back-nasal fuzzy pinyin (local branch `weave-fuzzy`, weave-ime Phase 5 WP4.3; default 1 since Phase 5 unfreeze 2, was 0).**
`SIME_WEAVE_FUZZY` (`include/sime.h`, default 1: compiled in, and no edge is added until `SetFuzzyCost` gives a pair a cost; needs `SIME_WEAVE_SWAP_EDGE`): `Sime::AppendFuzzyEdges`, after
`AppendSwapEdges` at the end of `BuildSentenceNet`, adds extra pinyin edges for words reached with one nasal final read as
its partner: typed an/en/in reaching stored ang/eng/ing, and typed ang/eng/ing reaching stored an/en/in where the stored
syllable ends after the n. Each pair (`FuzzyPair::kAnAng`, `kEnEng`, `kInIng`) has its own runtime cost per flip
(`SetFuzzyCost`; negative, the initial value, is off); `SetFuzzyLimits(min_span, max_flips)` (defaults 3 and 1). A fuzzy
edge never swaps letters, abbreviates or completes, and a token already reached over the same span by an exact, overlay
or swap edge gets none. It pays a reading-prior line on its reading (swap edges do not). It takes no learning adjustment,
nor does the step after it (`State::kEdgeFuzzy`). Provenance: `Backtrace` restores the swap and fuzzy flags, and
`Segment::correction` is 0 (as typed), 1 (swap) or 2 (fuzzy); the incremental self-check compares it. Exact edges are not
changed or filtered; with every pair off the lattice is the one without the macro.

**Doubled-letter edges (local branch `weave-dup`, weave-ime Phase 5 owner feedback of 2026-11-15; default 1 on the p5u3 candidate, was 0).**
`SIME_WEAVE_DUP_EDGE` (`include/sime.h`, default 1: compiled in, and no edge is added until `SetDupCost` gets a cost; needs `SIME_WEAVE_FUZZY`): `Sime::AppendDupEdges`,
after `AppendFuzzyEdges` at the end of `BuildSentenceNet`, adds extra pinyin edges for words reached with one copy of
two equal adjacent typed letters skipped (`wooxiangchifan` reaching `woxiang`). The edge covers the typed span with both
copies, at the runtime cost of `SetDupCost(cost, min_span)` (negative, the initial value, is off; `min_span` the shortest
edge in typed bytes, default 3) plus a reading-prior line on its reading. At most one skip per edge; it never swaps
letters, flips a nasal, abbreviates or completes, and a token already reached over the same span by any edge gets none.
English lookups are not touched. It takes no learning adjustment, nor does the step after it (`State::kEdgeDup`).
Provenance: `Backtrace` restores the flag and `Segment::correction` is 3; the incremental self-check compares it.

**lue/nue alias edges (local branch `weave-dup`, weave-ime Phase 5 owner feedback of 2026-11-15; default 1 on the p5u3 candidate, was 0).**
`SIME_WEAVE_UE_ALIAS` (`include/sime.h`, default 1: compiled in, and no edge is added until `SetUeAliasCost` gets a cost; needs `SIME_WEAVE_FUZZY`): `Sime::AppendUeAliasEdges`,
after the overlay edges and before `AppendSwapEdges` in `BuildSentenceNet`, adds extra pinyin edges for words reached
with a typed `u` read as the stored `v` where a typed `l` or `n` starts the syllable and `e` follows (`lue` reaching
`lve`, `nue` reaching `nve`; `lu` alone never reads `lv`). Cost per alias from `SetUeAliasCost` (negative, the initial
value, is off) plus a reading-prior line. Exact edges are not changed or filtered (`lu|e` stays), and a token already
reached over the same span by an exact or overlay edge gets none. It learns like an exact edge. Provenance: `Backtrace`
restores `State::kEdgeUeAlias` and `Segment::correction` is 4; the incremental self-check compares it.

**QWERTY-neighbour substitution edges (local branch `weave-nb`, weave-ime Phase 5 owner feedback of 2026-11-15; default 1 on the p5u4 candidate, was 0).**
`SIME_WEAVE_NEIGHBOUR_EDGE` (`include/sime.h`, default 1: compiled in, and no edge is added until `SetNeighbourCost` gets a cost; needs `SIME_WEAVE_FUZZY`): `Sime::AppendNeighbourEdges`, after
`AppendDupEdges` at the end of `BuildSentenceNet`, adds extra pinyin edges for words reached with one typed letter read as
one of its neighbours on a staggered QWERTY layout (`zhonhguo` reaching `zhongguo`). The edge covers the typed span, at the
runtime cost of `SetNeighbourCost(cost, cross_cost, min_span)` for a neighbour in the same row (`cost`) or in the row above
or below (`cross_cost`; negative drops those pairs), with `SetNeighbourPairCost(typed, intended, cost)` replacing one
pair, plus a reading-prior line on its reading. A negative `cost` (the initial state) is off. At most one substitution
per edge; it never swaps letters, flips a nasal, skips a doubled letter, aliases, abbreviates or completes, and a token
already reached over the same span by any edge gets none. English lookups are not touched. It takes no learning
adjustment, nor does the step after it (`State::kEdgeNeighbour`). Provenance: `Backtrace` restores the flag and
`Segment::correction` is 5; the incremental self-check compares it.

**English slip edges (local branch `weave-nb`, weave-ime Phase 5 owner feedback of 2026-11-15; experiment, default 0).**
`SIME_WEAVE_EN_TYPO` (`include/sime.h`, default 0; needs `SIME_WEAVE_NEIGHBOUR_EDGE`): `Sime::AppendEnTypoEdges`, after
`AppendNeighbourEdges` at the end of `BuildSentenceNet`, adds extra English edges for words of the L0 English DAT and the
overlay DAT reached with one slip of the typed letters: a QWERTY neighbour read for a typed letter, a typed letter skipped
(never the first of the edge), a letter supplied, or two adjacent letters read swapped. Cost per kind from
`SetEnTypoCost(kind, cost)` (negative, the initial value, is off) plus the English penalty `ComputeEdgePenalties` gives an
exact edge over the same span; `SetEnTypoLimits(min_len, gate)`: corrected words have at least `min_len` letters (default
4), and with `gate` (default on) a typed span that is itself an English word gets none. A token already reached over the
same span by an English edge gets none. Pinyin lookups are not touched. It takes no learning adjustment, nor does the
step after it (`State::kEdgeEnTypo`). Provenance: `Backtrace` restores the flag and `Segment::correction` is 6; the
incremental self-check compares it.

**Stray-key (extra letter) edges (local branch `weave-nb`, weave-ime Phase 5 owner feedback of 2026-11-15; default 1 on the p5u4 candidate, was 0).**
`SIME_WEAVE_EXTRA_EDGE` (`include/sime.h`, default 1: compiled in, and no edge is added until `SetExtraCost` gets a cost; needs `SIME_WEAVE_NEIGHBOUR_EDGE`): `Sime::AppendExtraEdges`, after
`AppendEnTypoEdges` at the end of `BuildSentenceNet`, adds extra pinyin edges for words reached with one typed letter
skipped (`woxianmgchifan` reaching `woxiangchifan`), never the edge's first letter. `SetExtraCost(cost, other_cost,
min_span)`: the skip costs `cost` when the letter is a QWERTY neighbour of the typed letter before or after it,
`other_cost` otherwise (negative: none); a letter equal to its neighbour is left to the doubled-letter edge; a negative
`cost` (the initial state) is off; `min_span` the shortest edge in typed bytes. Plus a reading-prior line. At most one
skip per edge, never chained, a token already reached over the same span by any edge gets none; English lookups are not
touched. No learning adjustment into or out of it (`State::kEdgeExtra`; `State::backtrace_flags` is 16 bits for it, in
the same padding, so `sizeof(State)` does not change). Provenance: `Backtrace` restores the flag and
`Segment::correction` is 7; the incremental self-check compares it.

**Rare-reading penalty table (local branch `weave-unfreeze`, weave-ime Phase 5 unfreeze Task 2; from `weave-reading-prior` and `weave-reading-prior2`, F12; default 1 since Task 4, was 0).**
`SIME_WEAVE_READING_PRIOR` (`include/sime.h`, default 1: compiled in, and nothing changes until `LoadReadingPrior` gets a table): `Sime::LoadReadingPrior` reads a
table of (character, reading, penalty), and a single-character pinyin edge on a listed pair pays that penalty
in addition to its other penalties (`ApplyReadingPrior`, after `ComputeEdgePenalties` and before `PruneNode`); a line
whose penalty is `drop` removes the edge (`weave-reading-prior2` only).
Multi-character words are not touched. A table line is a plain decimal penalty in [0, 1000] (read with `from_chars`, so
the C locale does not matter) or `drop`; an empty penalty, a hex or out-of-range number and a pair listed twice are refused
at load. `overlay.cc` reads the L2 Zipf column with `from_chars` for the same reason. The table is an input, not part of Sime: weave-ime keeps its table at
`benchmarks/results/phase5/polyphone/reading_r.tsv`. **That table derives from the Unihan database (Unicode 18.0.0,
Unicode License) and from Luna Pinyin (rime-luna-pinyin, LGPL-3.0)**, and `LUNA_LICENSE.txt` stays beside it.
Touches `include/sime.h`, `src/sime.cc`.

**Dictionary.** `SIME_WEAVE_CASE_FOLD=1` looks every typed English span up
by its lowercased spelling and, when the span has capitals, also as typed;
the items both lookups find for one folded key form one group, and a typed
capital keeps only the group's items whose token has exactly the typed
casing (as a prefix, for completions), when there are any. What that does
depends on the dictionary:

- With a dictionary whose English keys are lowercase (the intended pairing),
  any typed casing finds the word: `unity`, `Unity` and `UNITY` all reach
  key `unity`, whose items are every casing of it (`Unity`, `unity`). Typed
  `Unity` keeps `Unity`; a casing with no exact match (lowercase `unity`,
  or `UNITY` when no `UNITY` token exists) leaves every casing to compete on
  LM score, so lowercase input can display a capitalised word (`hello` ->
  `Hello`). weave-ime builds this dictionary from `save/sime.dict` with
  Sime's own `sime-dump`/`sime-converter`
  (`benchmarks/engines/sime/rebuild_dict.sh --transform lowercase-en` in
  weave-ime): token table and pinyin items unchanged, English keys
  lowercased, colliding keys merged. It is derived from `save/sime.dict` and
  is not committed here.
- With upstream's `save/sime.dict` (cased English keys such as `Unity`,
  `HTTP`, `unity`), a lowercase span finds only lowercase keys, exactly as
  upstream. A span typed with capitals finds its exact casing as upstream
  does (`Unity`, `HTTP`, `iPhone` stay as typed) and, through the folded
  lookup, lowercase keys too: when the typed casing exists it wins its group
  (`Unity` over `unity`), otherwise the lowercase word is a candidate
  upstream would not have offered (typed `iPHone` can give `iphone`), and
  completions of a capitalised prefix include lowercase-only words (`Uni`
  also completes to `uniforms`). Keys cased differently from both the typed
  span and lowercase (`iPhone` from typed `IPHONE`) are not found, as
  upstream.

`DecodeResult::units` match the typed letters to the dictionary pieces
ignoring ASCII case and keep the typed casing (`Unity` -> `Unity`, not the
lowercase-en pieces' `unity`).

Unchanged defaults: `SIME_WEAVE_EN_PENALTY` 2.5, `SIME_WEAVE_BEAM_SIZE` 20, `SIME_WEAVE_COLUMN_BEAM_SIZE` 48,
`SIME_WEAVE_LITERAL_EDGE` 0 (and `SIME_WEAVE_LITERAL_MIN_LEN` 3,
`SIME_WEAVE_LITERAL_PENALTY` 12.0, `SIME_WEAVE_LITERAL_PER_LETTER` 0.0,
`SIME_WEAVE_LITERAL_NONPINYIN` 0), `SIME_WEAVE_SPACE_BOUNDARY` 0, the
debug-only `SIME_WEAVE_BEAM_CHECK` 0; all equal upstream. (`SIME_WEAVE_OVERLAY` and
`SIME_WEAVE_INCREMENTAL` were 0 here until Phase 4.5 Task 8c; see the table above.)

**Upstream behaviour** is reproduced exactly by defining every macro to its
upstream value: `PENALTY_RULE=0 EN_PENALTY_MIXED=8.0 EN_SHORT_EDGE_LEN=0
EN_PENALTY_SHORT=8.0 CASE_FOLD=0 RAW_CNT=0 BEAM_EXACT=0 OVERLAY=0 INCREMENTAL=0` plus the unchanged values above (each name
prefixed `SIME_WEAVE_`), with upstream's `save/sime.dict`. weave-ime's
regression gate checks that build against its Phase 1 reference runs.

**Phase 4.5 Task 2 (2026-10-26).** The owner chose `rc_e2_p4.0` (D1): the short-English
edge length went 3 -> 2 and `SIME_WEAVE_EN_PENALTY_SHORT` 8.0 -> 4.0 (the old pair is
`rc_e3_p8.0`; define both macros to get it back). Evidence in weave-ime:
`benchmarks/results/recall/penalty/summary.md` and `benchmarks/results/phase45/recall_gate/decision.md`.
Nothing else changed; no left context is fed to the decoder (D2).

**Phase 4.5 Task 8c (2026-10-27).** The owner chose (D7) to ship the lexicon overlay with the
vocabulary proposal `l12_f12` and incremental decoding on, so `SIME_WEAVE_OVERLAY` and
`SIME_WEAVE_INCREMENTAL` now default to 1 (were 0). Only the defaults changed; no code did. The overlay
*data* and its settings are not in this fork: weave-ime defines them in one file
(`benchmarks/engines/sime/weave_defaults.conf` there). weave-ime checks that a default build with no
overlay loaded and incremental decoding switched off at runtime decodes every allowed set exactly like the
previous default, and that with the shipped settings it equals the measured proposal
(`benchmarks/results/phase45/defaults/` there). Define both macros to 0 for the previous default.

**Phase 4.5 Task 3 (2026-10-26): the lexicon overlay, `SIME_WEAVE_OVERLAY` (default 0; 1 since Task 8c).** With
0 nothing below is compiled (`src/overlay.cc` compiles to nothing) and the engine is unchanged.
With 1, `Sime::LoadOverlay` reads versioned lexicon layer files (weave-ime
`Docs/plans/phase-4.5-lexicon.md`: L1 ESDB words, L2 wordfreq frequencies, L3 a term pack, L4
user words), merges them across layers (same merge key, or the same typed key with the same
folded spelling; display spelling from the highest layer) and builds an in-memory English DAT
over their typed keys. A typed key whose L0 English entry already has a token with the same
folded spelling adds no edge (the L0 token keeps its LM cost); only an L3 or L4 spelling may
then change how that L0 token is displayed (L1 only adds words L0 lacks). `InitNet` collects overlay edges
(exact matches, and tail completions where L0 English completions fire) into a separate list;
`DecodeSentence`/`DecodeStr` give them the English edge penalties, filter (exact over completion)
and prune them among themselves and append them after L0's own filtered and pruned edges, so no
L0 edge is removed or re-costed. `Process`, `PruneNode` and Layer 2 score an overlay edge at its
band cost `max(floor, floor + band * clamp((zipf_hi - z) / (zipf_hi - zipf_lo), 0, 1) - bonus)`
(runtime `OverlayConfig`; defaults 18.0, 6.0, 1.0, 6.0, user bonus 6.0) with a history break:
the next word is scored from the LM root, as after `NotToken`. Overlay tokens carry IDs from
`Overlay::kTokenBase` (0xE0000000) in `DecodeResult::tokens`; `InitialState` treats one in a
context vector as the same history break (not skipped like `NotToken`); `LearnUserSentence`
ignores sentences that contain one; `TokenText` returns its spelling. Runtime switches:
`SetOverlayLayerEnabled` per layer (off equals loading without that layer),
`SetDisplayCasing` (`kCanonical` default: L0 words as before unless the term pack or user
dictionary has the word, overlay words in their canonical spelling; `kAsTyped`: every English word in the typed
casing, which changes L0 output and so is off by default), `SetDedupByKey` (default off:
de-duplicate the candidate lists by ASCII-lowercased, space-free text instead of exact text).
With the macro on and empty layers (or every layer switched off) decoding is byte-identical. The
T9 path, `GetTokens` and `NextTokens` do not use the overlay.

**Phase 4.5 Task 5 (2026-10-26): candidate-contract provenance (additive, no macro).** weave-ime's
candidate contract (`Docs/plans/phase-4.5-candidate-contract.md` there) needs to know, per
candidate, where each piece came from. `State` gains `backtrace_flags` (the English and expansion
flags of the edge that led to it; it fills padding, so `sizeof(State)` is unchanged on 64-bit
targets), `Process` sets it and `Backtrace` restores `Link::english` / `Link::expansion` from it.
`DecodeResult` gains `segments` (per token link: raw-input byte span, text, typed and canonical
casing, kind pinyin / English / literal, source layer and merged sources, L0 token, units) and
`basis` (`kSentence` for Layer 1, `kWordAlt` for Layer 2); only `DecodeSentence` fills them, which
now always records the raw offsets `NormalizeInput` already computed for `SIME_WEAVE_RAW_CNT`.
With `SIME_WEAVE_OVERLAY=1`, `Overlay::Token` records the layer slot its spelling came from and
`Overlay::L0Sources` the L3/L4 layers that list an L0 word. None of this feeds back into the
search, the scores, the text, `units`, `tokens`, `cnt` or the order of the list, so like
`TokenizeContext` it needs no `SIME_WEAVE_*` macro: weave-ime checks that every allowed set decodes
byte-identically to the previous runs with the overlay compiled out and with it compiled in but
empty (`benchmarks/results/phase45/contract/identity.md` there). The IDs and hashes of the contract
are computed outside the fork (weave-ime `src/contract`).

**Phase 4.5 Task 6 (2026-10-26): incremental decoding, `SIME_WEAVE_INCREMENTAL` (default 0; 1 since Task 8c).** With 0
nothing below the refactor is compiled and every call decodes from scratch. With 1, `DecodeSentence` keeps
a chain of snapshots for the current composition (normalised input, generation, per column a shared edge
list and a shared beam). When the normalised input equals the last one, the lattice is reused; when it
extends it, the edges are rebuilt with the unchanged code, compared column by column with the snapshot's,
and `Process` resumes at the first column whose edges differ: the columns before it keep their beams, and
their edges that end after it are replayed in `Process`'s order (column, state, edge), so every later
column receives exactly the insertions of a full decode. A backspace pops the chain back to the longest
snapshot that is a prefix; any other change (an edit inside the composition) is a cold full decode. The
output (Layer 1, Layer 2, segments, counts) is rebuilt on every call. A generation counter drops every
snapshot on `LoadOverlay`, `ClearOverlay`, `SetOverlayLayerEnabled`, `SetDisplayCasing`, `SetDedupByKey`,
`SetUserSentenceEnabled`, `LoadUserSentence`, `LearnUserSentence`, `MutableUserSentence` (now defined in
`src/sime.cc`), `SetIncremental`, `ResetIncremental` and a change of the decoder context. Runtime API:
`SetIncremental` (on by default in a build with the macro; off = every call through the unchanged full
path), `SetIncrementalSelfCheck` (each warm call also decoded in full and compared; a difference returns
the full result), `SetIncrementalDiagnostics`, `GetIncrementalStats`, `LastDecodeInfo` (cold/warm and
the resume column, for the candidate contract's `cache_state`), `DecodeSentenceFull` (a full decode that
leaves the snapshots alone) and the test hook `SetIncrementalFaultForTest`. A failed consistency check on
a snapshot (column counts, missing columns, its input a prefix of the new one, column 0's initial state) falls back
to a full decode and is counted. weave-ime checks that every decode equals a full re-decode on every
prefix and on seeded random typing sessions of every allowed set (`benchmarks/results/phase45/incremental/`
there).

**Refactor with no output change (Phase 4.5 Task 6, no macro).** `DecodeSentence` is split into
`BuildSentenceNet` (InitNet, penalties, pruning, overlay edges), `DecodeSentenceUnlocked` and
`SentenceResults` (Layer 1 and Layer 2 from a processed lattice), and `Process`'s inner step into `Relax`,
so the full and the incremental paths share one implementation. Segments are now filled only for the
candidates that are emitted (Layer 1 after the cut, Layer 2 once per kept entry) instead of for every
scanned path and column-0 edge; candidate choice never reads segments. `PushBestLayer2EntryKeyed` is
folded into `SentenceResults`. weave-ime checks that every allowed set decodes field for field like the
previous (rc_e2_p4.0) runs with the macro compiled out and in, and that the contract runs carry the same plain fields
and valid messages (`benchmarks/results/phase45/incremental/identity.md` there). It also compares the contract
output (segments included) of fork 692b874 with that of this refactor on 7 sets, 28,420 messages and
2,693,228 candidates: every field is equal except the fork-derived `cand_id`, `decoder_id` and `list_hash`,
`decoder`, and the timing `decode_us` (`benchmarks/results/phase45/incremental/cross_fork_contract.md` there).

Modified files:

- `include/sime.h`: search constants `EnglishPenalty`, `EnglishPenaltyShort`
  and `BeamSize` are now backed by `SIME_WEAVE_*` macros whose defaults equal
  the upstream values; adds `SIME_WEAVE_PENALTY_RULE` (upstream 0, weave default 1),
  `SIME_WEAVE_EN_PENALTY_MIXED` and `SIME_WEAVE_EN_SHORT_EDGE_LEN`; adds
  `SIME_WEAVE_CASE_FOLD` (upstream 0, weave default 1); adds `SIME_WEAVE_LITERAL_EDGE`
  (default 0 = upstream), `SIME_WEAVE_LITERAL_MIN_LEN` and
  `SIME_WEAVE_LITERAL_PENALTY`, `SIME_WEAVE_LITERAL_PER_LETTER` (default 0.0)
  and `SIME_WEAVE_LITERAL_NONPINYIN` (default 0); adds `SIME_WEAVE_SPACE_BOUNDARY` and
  `SIME_WEAVE_RAW_CNT` (upstream 0, weave default 1); `ToText`/`ExtractText` take the normalized
  input (unused unless `SIME_WEAVE_LITERAL_EDGE=1`).
- `src/sime.cc`: `ComputeEdgePenalties` applies the scoped English penalty when
  `SIME_WEAVE_PENALTY_RULE=1` (edge length counts ASCII letters only); with 0 it is upstream's code.
  With `SIME_WEAVE_CASE_FOLD=1`, English DAT lookups (`InitNet`, the letter
  side of `InitNumNet` including its letters+digits cross-boundary search,
  `GetTokens`) use the lowercased typed span and, when the span has
  capitals, the span as typed; results are grouped by match length and
  folded key, and a typed uppercase letter keeps only the items of a group
  whose token has exactly that casing, when there are any (the cross-boundary
  search emits every item, as before). `AbbreviatePieces` matches typed
  letters to pieces ignoring ASCII case and writes such letters as typed.
  With 0 it is upstream's code.
  With `SIME_WEAVE_LITERAL_EDGE=1`, `InitNet` adds a literal-letters edge for
  every span of at least `SIME_WEAVE_LITERAL_MIN_LEN` ASCII letters starting at
  a column where a dictionary edge starts. It displays the typed letters, is
  scored as the LM's unknown token (`Scorer::ScoreMove` on an out-of-vocabulary
  ID: back-off cost, empty next context) plus `SIME_WEAVE_LITERAL_PENALTY` plus
  `SIME_WEAVE_LITERAL_PER_LETTER` per letter; with
  `SIME_WEAVE_LITERAL_NONPINYIN=1` no literal edge is added for a span that
  segments fully into complete pinyin syllables (`Dict::IsKnownPinyin`). It is
  kept out of `DecodeResult::tokens`, and sits on the English track of the tier
  filter below dictionary edges. With the default 0 it is upstream's code.
  With `SIME_WEAVE_SPACE_BOUNDARY=1`, `NormalizeInput` keeps a typed space as one byte and
  `InitNet` makes it a `SpaceToken` edge (the apostrophe keeps `NotToken` and its LM reset;
  `Process` passes the LM state through a space unchanged at zero cost) and
  searches every dictionary, expansion and English-tail edge only within the
  space-delimited word, so no edge spans a typed space and the space adds no
  text or token. `ComputeEdgePenalties` evaluates `full_cover` and `short_input`
  per space-delimited word. `ComputeEdgePenalties` is shared with
  `DecodeNumSentence`, so the scoped penalty rule (`SIME_WEAVE_PENALTY_RULE=1`
  with the weave defaults) applies to the nine-key (T9) path too: an English
  edge that does not cover the whole letters+digits input and covers at least
  4 typed letters of the `start` prefix (digits are not counted) pays
  `SIME_WEAVE_EN_PENALTY_MIXED` (0.0) instead of upstream's 8.0; edges over
  digits only, or with at most 3 letters, keep 8.0. weave-ime has not
  measured the T9 path.
  With `SIME_WEAVE_RAW_CNT=1`, `DecodeSentence` Layer 2 reports `cnt` as
  raw-input bytes consumed (`NormalizeInput` records the raw offset of each
  normalised byte; an edge ending at the end of the normalised input reports
  `input.size()`, like Layer 1). With 0 each is upstream's code; `SIME_WEAVE_SPACE_BOUNDARY`
  defaults to 0 and `SIME_WEAVE_RAW_CNT` to 1 (see the table above).
- `include/sime.h`, `src/sime.cc` (additive, no macro): a public method
  `Sime::TokenizeContext(utf8, max_code_points)` returns the token IDs of the
  last `max_code_points` code points of committed text, cut by Sime's own
  `Cutter` over the loaded dictionary and LM (built once, on first use); a
  piece with no dictionary token is `NotToken`. It is meant to feed
  `DecodeSentence`'s existing `context` argument. It changes no decoding by
  itself (nothing in the engine calls it), so it needs no `SIME_WEAVE_*`
  macro, and a build with or without it decodes every input identically.
- `include/state.h`, `src/state.cc`: `NetStates`' column cap (upstream 48) is
  backed by `SIME_WEAVE_COLUMN_BEAM_SIZE` (default 48). With
  `SIME_WEAVE_BEAM_EXACT=1`, `NetStates::Insert` re-keys a bucket in the
  score-heap to its current worst score after every push (sifting up or down),
  keeps every bucket's cached heap index current, and counts a push that
  replaces a full bucket's worst state as no growth, so eviction runs only
  over the cap and always removes the globally worst state; with 0 it is
  upstream's code (weave default 1, see the table above). Adds `NetStates::BeamInvariantHolds()` and the
  debug-only `SIME_WEAVE_BEAM_CHECK` (default 0; needs `SIME_WEAVE_BEAM_EXACT=1`),
  which aborts when the invariant fails after an `Insert`.

- `include/sime.h`, `src/sime.cc`, `CMakeLists.txt` (Phase 4.5 Task 3, `SIME_WEAVE_OVERLAY`,
  default 0, 1 since Task 8c): the overlay API and its use in `InitNet`, `PruneNode`, `InitialState`, `Process`,
  `ToText`, `DecodeSentence`, `DecodeStr`, `LearnUserSentence` and `TokenText` as described
  above; `CMakeLists.txt` adds `src/overlay.cc` to `sime_core`.

- `include/sime.h`, `src/sime.cc`, `include/state.h`, `src/state.cc`, `include/overlay.h`,
  `src/overlay.cc` (Phase 4.5 Task 5, additive, no macro): `SegmentKind`, `ScoreBasis`, the
  `kSource*` layer bits, `Segment`, `DecodeResult::segments` / `::basis`, `State::backtrace_flags`,
  `Sime::FillSegment` / `ExtractSegments`, `Overlay::Token::slot` and `Overlay::L0Sources`, as
  described above. `AsTyped` (the D5 as-typed casing helper) is now compiled without the overlay
  too, for a segment's typed casing.

- `include/sime.h`, `src/sime.cc` (Phase 4.5 Task 6): `SIME_WEAVE_INCREMENTAL` (default 0, 1 since Task 8c) and its API, types
  (`DecodeKind`, `DecodeInfo`, `IncrementalStats`, `IncrementalFault`) and snapshot chain as described
  above; the no-output-change refactor (`BuildSentenceNet`, `DecodeSentenceUnlocked`, `SentenceResults`,
  `Relax`, segments for emitted candidates only).

- `.gitattributes`: upstream's single rule `dict/*.bin filter=lfs diff=lfs
  merge=lfs -text` is replaced by `* -text`, so checkouts are byte-identical to
  upstream regardless of `core.autocrlf`. The LFS rule was dropped because
  this fork tracks no `dict/*` files.

- `include/user.h`, `src/user.cc` (weave-ime Phase 5 F14, Docs/plans/phase-5-unfreeze.md Task 1):
  `SIME_WEAVE_LEARNING`, default 1 since Phase 5 unfreeze Task 4 (was 0), adds a commit clock, lazily decayed per-entry counts, a bigram-only switch, a
  fixed bound on the adjustment, a counts-only file with strict loading, pruning, and a merge of what an engine
  taught. With the macro at 0, or at 1 with every knob at its default, the model is unchanged.

- `include/sime.h`, `src/sime.cc` (weave-ime Phase 5 owner feedback 7, insertion points): `SIME_WEAVE_RIGHT_CONTEXT`,
  default 1 since Phase 5 unfreeze 2, was 0 (upstream has no such code), adds `Sime::TextCost`: the language model's cost of a piece of text
  under TokenizeContext's Cutter, scored from the start state (ScoreMove then Back per token, an unknown character at the
  back-off cost). It changes no decoding; a host may compare how well each candidate runs into the text right of the caret.

Added files:

- `NOTICE.weave.md`: this file.
- `include/overlay.h`, `src/overlay.cc` (Phase 4.5 Task 3): the lexicon overlay
  (`SIME_WEAVE_OVERLAY`, default 1 since Task 8c, was 0; an empty translation unit with 0): layer file parsing,
  cross-layer merge, bounded costs, L0 coverage and the overlay English DAT.
