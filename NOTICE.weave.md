# weave-ime modifications

This is a fork of Ismantic/Sime (Apache-2.0), branch `weave`, maintained by
the weave-ime project. Upstream LICENSE and attribution are unchanged.
Per Apache-2.0 section 4, files modified in this fork carry a header line
`Modified by the weave-ime project (2026): ...`.

Modified files:

- `include/sime.h`: search constants `EnglishPenalty`, `EnglishPenaltyShort`
  and `BeamSize` are now backed by `SIME_WEAVE_*` macros whose defaults equal
  the upstream values; adds `SIME_WEAVE_PENALTY_RULE` (default 0 = upstream),
  `SIME_WEAVE_EN_PENALTY_MIXED` and `SIME_WEAVE_EN_SHORT_EDGE_LEN`; adds
  `SIME_WEAVE_CASE_FOLD` (default 0 = upstream); adds `SIME_WEAVE_LITERAL_EDGE`
  (default 0 = upstream), `SIME_WEAVE_LITERAL_MIN_LEN` and
  `SIME_WEAVE_LITERAL_PENALTY`, `SIME_WEAVE_LITERAL_PER_LETTER` (default 0.0)
  and `SIME_WEAVE_LITERAL_NONPINYIN` (default 0); adds `SIME_WEAVE_SPACE_BOUNDARY` and
  `SIME_WEAVE_RAW_CNT` (default 0 = upstream); `ToText`/`ExtractText` take the normalized
  input (unused unless `SIME_WEAVE_LITERAL_EDGE=1`).
- `src/sime.cc`: `ComputeEdgePenalties` applies the scoped English penalty when
  `SIME_WEAVE_PENALTY_RULE=1` (edge length counts ASCII letters only); with the default 0 it is upstream's code.
  With `SIME_WEAVE_CASE_FOLD=1`, English DAT lookups (`InitNet`, the letter
  side of `InitNumNet`, `GetTokens`) use the lowercased typed span and a typed
  uppercase letter keeps only the items whose token has exactly that casing,
  when there are any; with the default 0 it is upstream's code.
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
  per space-delimited word. With `SIME_WEAVE_RAW_CNT=1`, `DecodeSentence` Layer 2 reports `cnt` as
  raw-input bytes consumed (`NormalizeInput` records the raw offset of each
  normalised byte; an edge ending at the end of the normalised input reports
  `input.size()`, like Layer 1). Both default to 0 = upstream.
- `include/state.h`, `src/state.cc`: `NetStates`' column cap (upstream 48) is
  backed by `SIME_WEAVE_COLUMN_BEAM_SIZE` (default 48). With
  `SIME_WEAVE_BEAM_EXACT=1`, `NetStates::Insert` re-keys a bucket in the
  score-heap to its current worst score after every push (sifting up or down),
  keeps every bucket's cached heap index current, and counts a push that
  replaces a full bucket's worst state as no growth, so eviction runs only
  over the cap and always removes the globally worst state; with the default 0
  it is upstream's code. Adds `NetStates::BeamInvariantHolds()` and the
  debug-only `SIME_WEAVE_BEAM_CHECK` (default 0; needs `SIME_WEAVE_BEAM_EXACT=1`),
  which aborts when the invariant fails after an `Insert`.

Added files:

- `.gitattributes`: `* -text`, so checkouts are byte-identical to upstream
  regardless of `core.autocrlf`.
- `NOTICE.weave.md`: this file.
