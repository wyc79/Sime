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
  `SIME_WEAVE_LITERAL_PENALTY`; `ToText`/`ExtractText` take the normalized
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
  ID: back-off cost, empty next context) plus `SIME_WEAVE_LITERAL_PENALTY`, is
  kept out of `DecodeResult::tokens`, and sits on the English track of the tier
  filter below dictionary edges. With the default 0 it is upstream's code.

Added files:

- `.gitattributes`: `* -text`, so checkouts are byte-identical to upstream
  regardless of `core.autocrlf`.
- `NOTICE.weave.md`: this file.
