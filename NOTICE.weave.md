# weave-ime modifications

This is a fork of Ismantic/Sime (Apache-2.0), branch `weave`, maintained by
the weave-ime project. Upstream LICENSE and attribution are unchanged.
Per Apache-2.0 section 4, files modified in this fork carry a header line
`Modified by the weave-ime project (2026): ...`.

Modified files:

- `include/sime.h`: search constants `EnglishPenalty`, `EnglishPenaltyShort`
  and `BeamSize` are now backed by `SIME_WEAVE_*` macros whose defaults equal
  the upstream values; adds `SIME_WEAVE_PENALTY_RULE` (default 0 = upstream),
  `SIME_WEAVE_EN_PENALTY_MIXED` and `SIME_WEAVE_EN_SHORT_EDGE_LEN`.
- `src/sime.cc`: `ComputeEdgePenalties` applies the scoped English penalty when
  `SIME_WEAVE_PENALTY_RULE=1` (edge length counts ASCII letters only); with the default 0 it is upstream's code.

Added files:

- `.gitattributes`: `* -text`, so checkouts are byte-identical to upstream
  regardless of `core.autocrlf`.
- `NOTICE.weave.md`: this file.
