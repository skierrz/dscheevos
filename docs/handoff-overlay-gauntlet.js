export const meta = {
  name: 'dsi-overlay-gauntlet',
  description: 'Brainstorm DSi in-game achievement overlays from 4 angles, attack them with 4 critic lenses, synthesize a ranked shortlist',
  phases: [
    { title: 'Ideate', detail: '4 agents, distinct angles' },
    { title: 'Gauntlet', detail: '4 critics, one lens each, over all ideas' },
    { title: 'Synthesize', detail: 'ranked shortlist with kill/confirm experiments' },
  ],
}

const BRIEF = `
CONTEXT. DScheevos adds RetroAchievements (RA) to retail Nintendo DS games running natively (no emulator) on a Nintendo DSi via nds-bootstrap.
Primary target: DSi / DSi XL in DS mode (TWiLight Menu++ -> DScheevos launcher -> nds-bootstrap -> game). Later: DS via flashcard (B4DS mode, 4 MB RAM only), then 3DS (where a TwlBg overlay already solves this - ignore 3DS here).
Read-only research: do NOT edit, commit or push anything.

WHAT EXISTS (read as needed):
- /home/user/dscheevos/docs/architecture.md - where our hooks run: ARM9 IPC-sync IRQ at each VBlank (snapshot, now ~113k ARM9 instructions/frame for a typical set), evaluation in the game's idle thread (OS_Halt hook), ARM7 cardengine VBlank (LED blink via I2C, SDIO shim). Module lives at 0x02D80000 (DSi extra RAM), 2 MB.
- /home/user/nds-bootstrap (branch dscheevos): retail/cardenginei/arm9_igm/source/inGameMenu.c (the in-game menu takes over both screens, saving and restoring VRAM/registers - proven safe but it PAUSES the game), retail/cardenginei/arm7/source/cardengine.c, retail/cardenginei/arm9/source/cardengine.c.
- /home/user/bakkerrrs/retroachievements-ds-on-3ds - another project that tried an in-game overlay on real hardware. Read docs/retroachievements.md (search: overlay, sprite, OAM, BG1, Ketsui, TwlBg, corrupt) and docs/devlogs/ and retail/cardenginei/arm9_ra/source/ra_overlay.c. Their findings: drawing on 'free' game sprites with a borrowed BG layer fallback; every 'is this free?' inference failed in turn; on Ketsui a parked sprite bank and an in-use BG1 block got corrupted; the game chooses which physical screen each engine lands on.
- /tmp/claude-0/gbatek/gbatek.txt (and .htm): full DS/DSi hardware reference. Relevant: 2D engines A/B, OAM (128 entries/engine), sprite VRAM, BG modes, windows, blending, master brightness, display capture (DISPCAPCNT), main-memory display mode, VRAM bank mapping (VRAMCNT A-I), DSi-only registers (SCFG), LCD swap (POWCNT1 bit 15), extended palettes.
- /home/user/dscheevos/tools/melonDS (our melonDS fork) with a headless runner (tools/dscheevos/headless) that can script input, dump RAM, screenshot - usable as a bench to observe what a game touches.

ACCEPTANCE CRITERIA (verbatim from RA's Hardcore Compliance Requirements):
"Achievements: Triggers must evaluate correctly. Measured and Trigger flags must work properly. Both must be visible in the achievement list as well as displayed during gameplay when appropriate conditions are met."
"Rich Presence and Leaderboards: Must function correctly." "Disabling leaderboard popups is okay to support for either mode."
"Hardcore state must be visibly indicated in the UI during play. This is normally done when the player starts a game - they can see a brief informational message saying what mode they're in."
So the things to show: unlock notice, measured progress (e.g. 7/10), challenge indicator (achievement 'primed'), leaderboard start/tracker/submit-or-fail, the mode at game start. Owner wants it graphical, ideally RetroArch-like, badge images a stretch goal.

HARD CONSTRAINTS from the owner: no pausing the game, ever. No slowdown. No graphics corruption. No path by which the overlay changes game state or gives the player information RA wouldn't (cheating). "We don't want to end up with some crap that causes gameplay issues." Thinking outside the box is welcome.
`

const IDEA_SCHEMA = {
  type: 'object',
  properties: {
    ideas: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          name: { type: 'string' },
          mechanism: { type: 'string', description: 'exactly how it puts pixels (or other output) in front of the player: registers, VRAM banks, OAM entries, timing, which CPU, cite gbatek sections' },
          resources_borrowed: { type: 'string', description: 'every hardware resource it touches that the game might also use, and how it avoids conflict' },
          what_it_can_show: { type: 'string' },
          cost: { type: 'string', description: 'CPU time per frame, where it runs (IRQ/idle/ARM7), memory' },
          known_risks: { type: 'string' },
          evidence: { type: 'string', description: 'file:line or gbatek section references backing the key claims' },
        },
        required: ['name', 'mechanism', 'resources_borrowed', 'what_it_can_show', 'cost', 'known_risks', 'evidence'],
      },
    },
  },
  required: ['ideas'],
}

const ANGLES = [
  { key: 'oam', text: 'ANGLE: sprite/OAM injection. E.g. write our own entries into OAM after the game has done its OAM update for the frame, choosing entries and VRAM tiles safely. State precisely where tile data and palettes live, how you know the game will not see/overwrite them, timing relative to the game\'s own OAM DMA/copy, and what happens with games that use all 128 entries or all sprite VRAM. Also consider the sub engine vs main engine and games that swap screens.' },
  { key: 'vram', text: 'ANGLE: VRAM, display mode and display-capture tricks. E.g. DSi-specific VRAM or registers the DS game cannot know about, unused VRAM banks (VRAMCNT H/I etc), display capture blending, main-memory display mode, windows + blending to darken a strip, master brightness, extended palettes, the 3D engine (rear plane, a single textured quad on top). For each, say exactly which banks/registers and how a DS-mode game could or could not be using them.' },
  { key: 'bench', text: 'ANGLE: data-driven per-game safety. Use our melonDS bench to record, per game, every OAM entry, VRAM region, BG layer, palette entry and register a game touches during scripted play (or at runtime on the console: watch the game\'s own usage live and only draw where it is provably unused, with an instant fallback). Design the allowlist / runtime detector, how it would be generated and shipped (per game ID), and how a game that has no profile behaves. Be concrete about what the bench must log and how coverage of all game states would be argued.' },
  { key: 'nondisplay', text: 'ANGLE: outside the box, including non-display channels and negotiating the requirement. E.g. power/Wi-Fi LED patterns, sound cues mixed into a spare sound channel, the DSi camera LED, showing notices only in moments the game itself blanks the screen (loading, fades, pause), the top/bottom screen the game is not using, ASKING RetroAchievements what \'displayed during gameplay\' means for a hardware client and proposing an alternative they could accept. Treat \'negotiate the requirement\' as a real candidate to evaluate, not a cop-out, but also say what RA would most likely accept.' },
]

const CRITIC_SCHEMA = {
  type: 'object',
  properties: {
    verdicts: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          idea: { type: 'string', description: 'idea name as given' },
          fatal: { type: 'boolean', description: 'true if this lens finds a flaw that cannot be engineered around' },
          problems: { type: 'string' },
          fixable_how: { type: 'string' },
          evidence: { type: 'string' },
        },
        required: ['idea', 'fatal', 'problems', 'fixable_how', 'evidence'],
      },
    },
  },
  required: ['verdicts'],
}

const LENSES = [
  { key: 'contention', text: 'LENS: VRAM/OAM/register contention and corruption. Can the game overwrite our output, or can we overwrite something the game uses (now or later in the game)? Check against gbatek and the other project\'s documented failures. Assume games use hardware in every legal way, including reading VRAM back, using all OAM, remapping VRAM banks mid-game, swapping LCDs, and using display capture.' },
  { key: 'budget', text: 'LENS: timing and CPU budget. Where does each idea run (IRQ, idle thread, ARM7), how many cycles per frame, does it risk delaying the game\'s own VBlank work (the other project measured that overrunning the VBlank window caused tearing and a freeze in Chrono Trigger), DMA/bus contention, and does it stay correct when the game gives no idle time for seconds.' },
  { key: 'cheating', text: 'LENS: cheating and state leakage. Could the idea change game state (RAM, timing, RNG, input latency, lag frames), or reveal information RA would not (e.g. measured values RA does not show)? Could a user abuse it? Would RA reviewers accept it under the hardcore rules? Be strict.' },
  { key: 'variability', text: 'LENS: per-game variability and testability. How many of the ~6000 DS games would it work on, what classes of games break it (3D games, games using all sprites, games using display capture, games swapping screens, homebrew-like engines), and how could we prove it on our melonDS bench before hardware? Propose the concrete bench test.' },
]

phase('Ideate')
const ideaSets = await parallel(ANGLES.map(a => () =>
  agent(BRIEF + '\n' + a.text + '\nGive 2-4 distinct, concrete ideas from this angle. Verify hardware claims in gbatek; cite. Prefer fewer, deeper ideas over a long list.', { label: 'ideate:' + a.key, phase: 'Ideate', schema: IDEA_SCHEMA, effort: 'medium' })))
const ideas = ideaSets.filter(Boolean).flatMap((s, i) => s.ideas.map(x => ({ ...x, angle: ANGLES[i].key })))
log(ideas.length + ' ideas from ' + ideaSets.filter(Boolean).length + ' ideators')
const ideaText = ideas.map((x, i) => `### Idea ${i + 1}: ${x.name} (angle: ${x.angle})\nMechanism: ${x.mechanism}\nBorrows: ${x.resources_borrowed}\nShows: ${x.what_it_can_show}\nCost: ${x.cost}\nKnown risks: ${x.known_risks}\nEvidence: ${x.evidence}`).join('\n\n')

phase('Gauntlet')
const critiques = await parallel(LENSES.map(l => () =>
  agent(BRIEF + '\n' + l.text + '\n\nAttack EVERY idea below through your lens only. Your job is to find what breaks; default to skepticism, but mark fatal only when no engineering fix exists. Verify claims against gbatek and the code; cite.\n\n' + ideaText, { label: 'critic:' + l.key, phase: 'Gauntlet', schema: CRITIC_SCHEMA, effort: 'high' })))
const critText = critiques.map((c, i) => c ? `## Critic: ${LENSES[i].key}\n` + c.verdicts.map(v => `- ${v.idea}: ${v.fatal ? 'FATAL' : 'survives'} | ${v.problems} | fix: ${v.fixable_how} | ${v.evidence}`).join('\n') : `## Critic ${LENSES[i].key}: no result`).join('\n\n')

phase('Synthesize')
const report = await agent(BRIEF + '\n\nBelow are ideas and four critics\' verdicts. Produce the final report for the project owner, plain direct language, under 1,500 words:\n1. Ranked shortlist of viable approaches (survivors, possibly combining ideas). For each: how it works in 3-5 sentences, what it can show (vs the RA criteria), its residual risks, and THE ONE melonDS-bench experiment that would kill or confirm it (concrete: game, what to log, pass/fail).\n2. Ideas killed and the one-line reason.\n3. Whether any approach meets RA\'s wording on a DSi with zero risk to the game, and if not, what to ask RA.\n4. Recommended next step.\nRe-check any claim that decides the ranking against gbatek or code before relying on it.\n\n# IDEAS\n' + ideaText + '\n\n# CRITIQUES\n' + critText, { label: 'synthesize', phase: 'Synthesize', effort: 'high' })
return { report, ideaCount: ideas.length, critiques: critText }
