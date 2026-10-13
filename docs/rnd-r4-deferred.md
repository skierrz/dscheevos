# R4: deferred evaluation, quick measurement (rnd, 2026-10-12)

The owner rated deferred (late) evaluation LOW priority (the ARM7 route comes first), so this is
a measurement only: no design, no prototype.

## Verdict
Deferral does not rescue the sets that need rescuing. It helps only sets already close to the line.

- A set costing more than one frame (1,120,380 ARM9 cycles at 67 MHz) can never be kept up with,
  even in a session that is all menus. That covers Platinum full (1.50-1.60M measured), HG/SS
  (5.1-7.0M est.), TWEWY (11-15M est.), Digimon (2.6-3.6M est.), and on estimates Sonic Colors and
  MvDK (1.14-1.56M).
- MKDS fails even across the whole session: the set costs 706-777k against a mean idle of 387k
  in gameplay and 502k over the whole run. To break even it would need 3.3 menu frames per race
  frame (-O2 build) or 15 (base build).
- Pokemon Black fails by 13-21% (gameplay 716k, menus 723k, set 812-864k). Phoenix Wright is at
  the line.
- Golden Sun DD, Sonic Rush and Resident Evil DS give the module no idle in play, so there is
  nothing to defer into.

## Numbers
Session idle: 21 melonDS idle recordings (memplan idle/out/<game>/run/prof.csv). Gotcha: the raw
prof.csv cycle columns are 2x 67 MHz cycles (tests/perf/idle_report.py:50 halves them), and
segments.tsv starts are row indexes.

Longest run of frames with zero idle (a lower bound on queue depth): MKDS 72 (loads before the
race), Layton 1,455 (opening movie), MvDK 2,260 (intro video). Today's queue holds 1,024 frames
(dscheevos.c:51).

What unmodified rcheevos reads per frame. Every real set was loaded into unmodified rcheevos
683197d and every read-callback byte was logged on real RAM dumps. Reads equal direct + indirect
memrefs exactly; trigger evaluation adds no reads.

| set | read calls | unique bytes | aligned words | 32 B lines |
|---|---|---|---|---|
| MKDS 12711 | 124 | 156 | 56 | 26 |
| Platinum 11732 | 656 | 553 | 189 | 77 |
| SoulSilver 7212 | 2086 | 1587 (2367) | 562 (849) | 195 (274) |
| TWEWY 4887 | 1507 | 2442 | 948 | 147 |
| Digimon 16153 | 616 | 885 | 345 | 214 |

None of these sets reads the DTCM. Pointer-chain targets move: 782 SoulSilver bytes read at frame
8000 were not read at 10000.

MKDS queue for a whole race (7,200-10,800 frames):

| storage | queue size |
|---|---|
| today's full snapshot per frame | 2.7-4.1 MB |
| changed words only, 2 changed/frame | 84-127 KB |
| changed words only, 8 changed/frame | 288-432 KB |
| changed words only, all 56 changed/frame | 1.6-2.5 MB |

The real number of changed words per frame while racing is unmeasured: the RAM dumps are
100-2,000 frames apart.

## Compliance facts (for whoever picks this up)
- Whether a trigger fires depends only on the bytes the read callback returns per frame, plus
  frame order and count. Wall-clock time affects only side effects: unlock_time, the timing of
  rich presence and pings (first after 30 s, then every 120 s, and only if frames were
  processed), retry backoff and the indicator hide timer (rc_client.c).
- RA's hardcore requirements say nothing on unlock latency. The server stamps an unlock when the
  request arrives minus the `o` offset. Unmodified rc_client sends `o` only on retries, so a
  deferred live unlock is stamped at evaluation time, not at the frame's real time.
- The core is already a bounded deferred evaluator (a 1,024-slot ring; frames past it are
  dropped with DSC_EVENT_DEGRADED). The journal stamps an unlock at evaluation, not at capture.

(Re-checked 2026-10-13 under the new rcheevos rule: a minimal patch could make rc_client send `o`
on the first request, so the unlock is stamped at the frame's real time. That removes the
timestamp objection but not the cost verdict. Deferral still can't keep up with sets over one
frame, and it still fails MKDS and Black. R4 stays closed.)

## Flag for the capture lane
The estimate says the current core may store one 12-byte chain-target record per indirect read,
with no merging. That is about 25 KB/frame for SoulSilver and 7.7 KB for Platinum, so the 256 KB
arena would hold only about 10 SoulSilver frames. To check, read chains.target_count on
SoulSilver.

Data and scripts: rnd scratch r4/ (not in the repo).
