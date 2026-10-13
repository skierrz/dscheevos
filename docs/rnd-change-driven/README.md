# Change-driven evaluation: skip rates and semantics (rnd, 2026-10-13)

Question (top priority from developer-db): how much of rcheevos' per-frame trigger evaluation could
a minimal, provably identical rcheevos patch skip when a trigger's inputs didn't change? eval-speed
designs the patch (evalspeed-team/change-driven.md); rnd measured it.

- [semantics.md](semantics.md): which rcheevos semantics stop a trigger from being skippable, every
  state write site, and the verdict: S0 (strict fixed point) and S1 (fixed point modulo plain
  hit tallies) are exact with five corrections (C1-C5).
- [skiprates.md](skiprates.md): measured skip rates on 2x200 consecutive gameplay frames per game
  (stock rcheevos 683197d, rc_client-default activation). Every skip was validated against stock
  evaluation with 0 violations, and the validator catches deliberately broken rules.

Headline: MKDS (a real race) skips ~37% of achievement conditions per frame, worth ~100-165k of
~764k cycles, or ~400k if leaderboards are skipped too (exact on this data). The other windows were
mostly intros with near-static RAM, so their high numbers are upper bounds. Real-play windows are
being recorded. Change-driven evaluation alone doesn't make TWEWY or HG/SS fit on the ARM9: HG/SS
runs ~5,000 pointer-chain memrefs per frame that are never skippable, and TWEWY needs a lazy
hit-count catch-up.
