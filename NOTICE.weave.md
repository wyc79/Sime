# weave-ime modifications

This is a fork of Ismantic/Sime (Apache-2.0), branch `weave`, maintained by
the weave-ime project. Upstream LICENSE and attribution are unchanged.
Per Apache-2.0 section 4, files modified in this fork carry a header line
`Modified by the weave-ime project (2026): ...`.

Modified files:

- `include/sime.h`: search constants `EnglishPenalty`, `EnglishPenaltyShort`
  and `BeamSize` are now backed by `SIME_WEAVE_*` macros whose defaults equal
  the upstream values.

Added files:

- `.gitattributes`: `* -text`, so checkouts are byte-identical to upstream
  regardless of `core.autocrlf`.
- `NOTICE.weave.md`: this file.
