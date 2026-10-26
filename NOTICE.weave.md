# weave-ime modifications

This is a fork of Ismantic/Sime (Apache-2.0), branch `weave`, maintained by
the weave-ime project. Upstream LICENSE and attribution are unchanged.
Per Apache-2.0 section 4, files modified in this fork carry a header line
`Modified by the weave-ime project (2026): ...`.

## Default behaviour (changed in weave-ime Phase 2, Task 8)

Each change below is a `SIME_WEAVE_*` macro guarded by `#ifndef`. Until
weave-ime Phase 2 Task 8 every default reproduced upstream. Since then seven
defaults differ from upstream, so **a build with no `-D` defines no longer
behaves like upstream Sime**:

| macro | weave default | upstream value | effect of the weave default |
|---|---|---|---|
| `SIME_WEAVE_PENALTY_RULE` (`include/sime.h`) | 1 | 0 | scoped English penalty (whole-input words keep the upstream penalties) |
| `SIME_WEAVE_EN_PENALTY_MIXED` (`include/sime.h`) | 0.0 | 8.0 | English edges inside longer input pay no extra penalty ... |
| `SIME_WEAVE_EN_SHORT_EDGE_LEN` (`include/sime.h`) | 2 | 0 | ... unless they are at most 2 letters long (then `EnglishPenaltyShort`, 4.0) |
| `SIME_WEAVE_EN_PENALTY_SHORT` (`include/sime.h`) | 4.0 | 8.0 | the penalty for those short edges (and for edges over digits only) |
| `SIME_WEAVE_CASE_FOLD` (`include/sime.h`) | 1 | 0 | English lookups are case-insensitive with a lowercase-key dictionary; typed capitals still select their casing (see below for each dictionary) |
| `SIME_WEAVE_RAW_CNT` (`include/sime.h`) | 1 | 0 | `DecodeSentence` reports `DecodeResult::cnt` in raw input bytes (Layer 2 changes; `DecodeNumSentence` and `DecodeStr` are unchanged) |
| `SIME_WEAVE_BEAM_EXACT` (`include/state.h`) | 1 | 0 | exact column beam (fixes upstream's stale bucket keys and state-count drift) |

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
debug-only `SIME_WEAVE_BEAM_CHECK` 0 and `SIME_WEAVE_OVERLAY` 0 (Phase 4.5 Task 3, below);
all equal upstream.

**Upstream behaviour** is reproduced exactly by defining every macro to its
upstream value: `PENALTY_RULE=0 EN_PENALTY_MIXED=8.0 EN_SHORT_EDGE_LEN=0
EN_PENALTY_SHORT=8.0 CASE_FOLD=0 RAW_CNT=0 BEAM_EXACT=0` plus the unchanged values above (each name
prefixed `SIME_WEAVE_`), with upstream's `save/sime.dict`. weave-ime's
regression gate checks that build against its Phase 1 reference runs.

**Phase 4.5 Task 2 (2026-10-26).** The owner chose `rc_e2_p4.0` (D1): the short-English
edge length went 3 -> 2 and `SIME_WEAVE_EN_PENALTY_SHORT` 8.0 -> 4.0 (the old pair is
`rc_e3_p8.0`; define both macros to get it back). Evidence in weave-ime:
`benchmarks/results/recall/penalty/summary.md` and `benchmarks/results/phase45/recall_gate/decision.md`.
Nothing else changed; no left context is fed to the decoder (D2).

**Phase 4.5 Task 3 (2026-10-26): the lexicon overlay, `SIME_WEAVE_OVERLAY` (default 0).** With
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
  default 0): the overlay API and its use in `InitNet`, `PruneNode`, `InitialState`, `Process`,
  `ToText`, `DecodeSentence`, `DecodeStr`, `LearnUserSentence` and `TokenText` as described
  above; `CMakeLists.txt` adds `src/overlay.cc` to `sime_core`.

- `.gitattributes`: upstream's single rule `dict/*.bin filter=lfs diff=lfs
  merge=lfs -text` is replaced by `* -text`, so checkouts are byte-identical to
  upstream regardless of `core.autocrlf`. The LFS rule was dropped because
  this fork tracks no `dict/*` files.

Added files:

- `NOTICE.weave.md`: this file.
- `include/overlay.h`, `src/overlay.cc` (Phase 4.5 Task 3): the lexicon overlay
  (`SIME_WEAVE_OVERLAY`, default 0; empty translation unit then): layer file parsing,
  cross-layer merge, bounded costs, L0 coverage and the overlay English DAT.
