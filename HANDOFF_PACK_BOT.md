# Handoff: the from-scratch "pack" bot (started 2026-11-03 12:30Z, updated 18:30Z)

Read this first, then `NIRVAN_CLAUDE.md` (the full history of the baseline bot, v110 to v125, with the
judgement table of every lever tried). The memory directory
`C:\Users\joshu\.claude\projects\c--Users-joshu-Desktop-VSCode-Projects-cambridge-battlecode\memory\`
holds the user's standing rules; `MEMORY.md` there is the index.

## Where things stand (18:30Z)

- **Ladder:** v119 of `bots/baseline` is the recommended version. Rating ~2,050. Measured on 1,200
  ranked games: v110 alone, v112 alone and v119 alone perform the same (2,089 / 2,129 / 2,081, +-30 to
  45); the drop came from test versions being live during practice tests (34-51, performance 2,027).
  Practice groups always play the live version, so every test costs ladder games.
- **Another client (likely the user's Devin) submits and activates versions since ~17Z** (v125, v126
  "top teams' whole emblem paths"). Check `cambc submissions list` before any ladder action and do not
  fight its selections.
- **The new bot (`bots/pack/src`) builds, runs without errors, and farms, but loses every local game
  to v119**: 28-272 gems against v119's 1,000 on big-world, the-farm, galaxy and packs. Against the
  starter bot it wins (1,000 gems by round ~1,800). Unopposed it farms at about half v119's rate; on
  contested maps (packs, galaxy, the-farm) it spends 15-36% of turns dead.
- **Corpus of top-team games:** `analysis/corpus/` (not committed; raw data) with `tables.md` and
  `top_profile.json`: 139 Mite, 52 Intercontinental Fish, top-10 opponents and our ranked games.
  Main facts: they capture alone (93-95% of captures by one champion), cycle a few near groups as
  they respawn (21 captures by round 300 against our 12; first capture round 23 against 41), take
  gold-only groups in 30% of captures, bank 22-gem median bags (ours 12), die a third as often per
  champion, and build Jungle then Monster first (Mite) or Capture then Jungle (IF), not
  Prospector/Treasure.

### Bugs found and fixed in the pack bot (each was costing most of a game)

1. Whole-map path searches every turn (25k fuel each on 100x100 maps) overran the fuel budget every
   other turn: replaced by static per-group and per-altar fields built in spare fuel (about 50k each
   on big-world; built at turn start when 48k remains, and on dead/channel turns).
2. Recall loop: a champion arriving on spawn walked off before the turn-end heal; now it waits.
3. Camps inside bushes are invisible from outside, even adjacent; champions stood beside them.
4. Fields read on a captured spawn tile (a wall in other groups' fields) said "unreachable".
5. Approach stopped one step short of the edge, so pocket groups behind holes were never dashed into.
6. Champions walked to strong groups and only at the edge judged them unsafe: safety is now decided
   at target choice with a cached full-health check.
7. A ranged champion chased the weakest camp out of range and oscillated; now it attacks any camp in
   range and moves toward the nearest.
8. Exceptions from refused engine calls ended the whole turn: every action is wrapped.
9. Bank trips of 33 steps for 14-gem bags: a trip must be worth its walk.
10. Chasing enemies out of reach (14 turns for nothing): engagement needs an enemy in reach.
11. The `FUEL_OVERRUN` line printed by `main.cpp` is unreliable; count `fuelExhausted` events in the
    match statistics instead (about one per 100 rounds per champion: fine).

### Tools for this bot (scratchpad, recreate if lost)

`play.sh MAP TAG [OPP]` (plays pack vs OPP, prints result, turn mix per activity, per-champion stats),
`tally.py`, `early.py stats.json...` (first capture, captures by 300, gold at 200/400 per team),
`turnmix.py rounds.txt` (attack/move/idle/dead share per champion from `scripts/analysis/rounds.mjs`).
The bot prints `ACT r= slot= why hp gems att mov tgt`, `FARM`, `TARGET`, `FIGHT`, `CAND`/`SKIP` (every
50 rounds), `OPENING`, `FIELD` lines with `debug = true` in bot.cpp; set it false before any submit.
Local games are deterministic: the same build on the same map gives the same game.

## User rules (from the whole session; follow them)

- Practice groups on the same maps, seats and opponents in the same hour are the judge. Local games
  against test-bot/stacker/baseline are noise except as mechanism checks (a bot that scores 0 gems
  locally is broken; a bot that wins 55/100 locally has proven nothing).
- The user allows practice games at will and submit/activate at will. Quota: 50 practice matches
  per hour (10 groups of 5). Each practice pair exposes the ladder for ~25 rating; keep v119 live
  while testing, i.e. submit, activate the test version, run groups, re-activate v119
  (`analysis/pg10/test_v119.sh` and `test_v12x.sh` show the mk/waitlive/waitquota pattern).
- Push to GitHub, keep it clean. Document findings in `NIRVAN_CLAUDE.md` (its judgement table).
- The user is asleep (EST). Machine local time is EST; platform timestamps are UTC (`date -u`).
- Specialists per map class are fine (route at round 1); the user said "go ham".

## Tooling

- `cambc build bots/<name> --release`; `cambc run bots/pack bots/baseline --map 00packs --release
  -o <dir> --rounds N --statistics <dir>/stats.json --json`. Never run two `cambc run` of the same
  bot concurrently before one build has finished (cmake races).
- Replay bundle `.repl26a` is a zip: `replay.repl26a`, `team-a.stdout` (NUL-padded; read with
  `tr -d '\000'`), `team-a.fuel`. `stats.json` has per-champion metrics (monsterDamageDone,
  championDamageTaken, deaths, earnedGold...) and `roster`.
- `python scripts/evaluate.py --bot pack --baseline baseline,stacker,test-bot --maps 01big-world,...
  --fuels 100000 --out <dir>` plays every map in both seats (configs are for `strategy.h` knobs of the
  baseline; for `pack` use the default config `current`).
- Map names: `maps/eval/*.map26a` (e.g. `00packs`, `01big-world`, `02the-pond`, `03reef`).
- Analysis scripts: `scripts/analysis/rounds.mjs, layout.mjs, mapgrid.mjs (print offset +4,+1 vs bot
  coords), camphp.mjs`; `analysis/pg10/*.py` (gamesum, goldsrc, capseq, deathquality, packshare).
- Ladder: `cambc submit bots/pack -n "<=80 chars"`, `cambc submissions activate vN` (switch takes up
  to ~10 min), `cambc match-groups create/show`, `cambc ladder --page N`, `cambc matches list --team X
  --kind ranked --limit 100`.
- Heredoc caveat: `\n` inside a bash heredoc becomes a real newline. Write scripts and C++ with the
  Write tool or a `.py` patch file, not inline heredocs.

## SDK facts learned the hard way (sdk/cpp/cambc.h, imports.h)

- `bitboard::copy_from` is private: use assignment `a = b`. `a |= b`, `a &= ~b` work in place; each
  op and each `bitboard()` constructor costs fuel (engine calls/SIMD in wasm).
- `distance_field::compute(walls, roots, visited, max)` and `compute_to(...)` are **wasm-side BFS**
  (fuel proportional to tiles). Measured on 00packs (31x31): a full-map compute ≈ 3k fuel,
  the whole turn ≈ 40k of the 100k budget. First play turn has 2x fuel but `initialise()` cost 150k
  (camp_groups(), per-group bitboards, home field) so round 1 overruns into round 2; acceptable.
- `camp_group::state()` throws when no camp of the group is in vision; use the raw
  `bc::detail::sdk_camp_group_state(handle, &state)` (1 alive, 2 reward available, 3 respawning,
  0 unknown) and `sdk_camp_group_cooldown` for the timer, only for groups with a spawn tile visible.
- Ally positions are **not** known out of vision (vision is per champion). `sdk_champion(handle,
  &info)` returns api_not_in_vision (97) or x,y < 0 for hidden champions, allies included. Team
  coordination must go through the 8-byte shared memory (`bc::shared_memory.read/write(0,
  std::span{&pod,1})`).
- Raw structs: `detail::champion_info {team,type,x,y,health,max_health,strength,focus,move_speed,
  attack_range,state}` (state bits: alive 1<<0, recalling 1<<1, submitting 1<<2, can_move 1<<3,
  can_attack 1<<4, can_cast 1<<11, teleporting 1<<12); `detail::camp_state {x,y,health,max_health,
  strength,focus,cooldowns[2]}` via `sdk_camp(handle,&state)`.
- `champion_stats {vision_radius, base_move_speed, base_attack_range, base_health, health_growth,
  base_strength, strength_growth, base_focus, focus_growth, camp_damage_multiplier}`.
- Items: `item_type::<family>_shard_of_<stat>` (families monster, power, durability, treasure,
  jungle, home, bruiser, tribute, capture, prospector, hunter, skirmish, greed, collectors);
  `item.cost()` (30), `item.result()` (the emblem), `item.passive()`, `item.passive_magnitude()`,
  `me.item_count()[item]`, `me.can_buy(item,1)`, `me.buy(item,1)`. Buying is only on spawn or dead.
- Abilities: `me.abilities()` (2 slots), `a.info()` optional variant (ability_damage{range,modifier},
  ability_cc, ability_aoe_damage{range,modifier,radius}, ability_dash{range}, ability_shield,
  ability_attack_reset), each with `exclusive_with_movement`; `a.cooldown()`, `a.use()`, `a.use(vec2)`,
  `a.use(camp)`, `a.use(champion)`, `a.valid_tiles()` (legal dash landings), `a.is_dash()` etc.
- Draft: `bc::draft::team_size()`, `current_draft(team)`, `pick(champion_draft)`. Draft enum:
  ryth 0, vesper_vale 1, vendetta 2, cassi_and_issac 3, stellar 4, prism 5, marcella 6, willow 7,
  ren 8, varg 9, tavi 10, heart_of_ruin 11. champion_type enum differs (cassi 3, issac 4, ...,
  varg 10, tavi 11, heart_of_ruin 12).
- Engine error 42 "Target must be a visible enemy or neutral camp" came up twice a game in the pack
  bot (an attack on a camp read as alive that was not targetable; guard with `visible.is_set`).
- `move_towards` is also SDK-side BFS (builds a blocked board each call). `me.move(delta)`,
  `me.move_absolute(tile)` return false on a newly revealed obstruction, throw on known-blocked.

## Game facts that matter for design (docs/GameRules.md)

Wolf 1350hp/20str/r3 (40g, 4 gems, respawn 40), elk 2400/60/r4 (90g, 15 gems, 60), golem
5000/120/r4 (160g, 36 gems, 80), gem cluster 120hp (7 gems, 50), gold cluster 120hp (20g, 50),
monolith 20000/200/r5 (300g, 200 gems, 150). Camps hit only the nearest champion, wake on damage,
sleep after 2 rounds with nobody in range, never heal. Capture: stand on a dead group's spawn tile
5 rounds (Capture emblem: 3), reward split among allies on the tiles. Tribute within Chebyshev 1 of
an altar, ceil(gems/10) turn ends, damage does not interrupt (stun, death, new lock do). Recall 5
turn ends (3 with Home), cancelled by damage/stun. Death: killer gets the bag + 100 gold, 60-round
respawn, camp kills destroy the bag. Ranged (range 3) cannot move and attack in one turn; melee
can. Dashes: ground within 2 tiles, cross holes not walls. Shards 30 gold, 5 = emblem. Fuel 100k a
turn (2x first play turn, 100x first draft turn). Vision 8, not shared with allies; one ward each.

## The pack bot's design (bots/pack/src)

- `draft.cpp`: fixed roster by team size: vendetta, heart_of_ruin, vesper_vale, marcella, varg.
- `bot.h`: `Group` (spawn tiles, edges = walkable tiles beside them, gems/gold, health_total,
  strength_sum/max, status alive/reward/cooling with predicted respawn when unseen, enemy_seen,
  unsafe_until, unreachable_until), `Unit` (raw champion read), `CampSeen` (camps of alive groups in
  view within 10 tiles, with awake flag from `vision::awake_camp_tiles`), `Shared` (6 claim bytes:
  group index+1, 0x80 = not farming; enemy_group/enemy_round).
- `bot.cpp` turn order: dead → shop; channeling → wait; at spawn → shop; stuck 8 turns → mark the
  group unreachable; `fight()` (engage when local health×damage ≥ 1.15× theirs, 1.8× with a bag ≥ 40;
  focus the weakest in range; abilities; else step away toward home); `survive()` (under 30% health:
  flee enemies, step out of camp range, else recall; two camp turns would leave < 20%: step out);
  `bank()` (nearest altar by field; trip when bag ≥ 40, or ≥ 10 within 2 steps or on spawn, win,
  deadline; enemy locks are attacked); recall to shop at 300 gold with an empty bag;
  `choose_target()` (value = reward/(claimers+1)/(walk + clear + capture), pack bonus 1.3 on rich
  groups, 0.5 crowding on small ones, enemy-sighting penalty, golem/monolith guards, 1.25 stickiness,
  jump routes via a holes-passable field for dash champions); `farm()` (hold reward tiles, wait by
  cooling groups, attack the lowest-health camp in range, attack resets, damage/aoe/cc abilities on
  camps, Heart of Ruin dash landing beside 2+ camps, shield when camps are on us); `place_ward()`.
- Navigation: per turn one field `reach` rooted at me over `blocked` (terrain | alive group spawn
  tiles | visible champions | teleport reservations), a `reach_jump` variant with holes passable for
  dash champions; movement by a `goal` field rooted at the destination tiles and
  `next_move_from(pos)`; `dash_toward()` picks the landing among `valid_tiles()` with the smallest
  jump-field value when the next step is a hole.

## Debug state (what to do next)

Latest result (12:55Z, after raising the engage margin to 1.6x, a 90-round enemy memory, an
enemy-half penalty before round 400, a separate `goal_jump` field and a visibility guard in
`attack_camp`): still **0 gems** on 00packs (21 deaths) and **0 gems on 01big-world with only 3
deaths** against the baseline. So the bot is not farming at all even when it is not dying: the
fault is in farm()/attack_camp()/observe_groups(), not in the fight model. Add TURN-level prints of
`camps.size()`, the target's status and `walk_to_group`, and whether `attack_camp` finds a focus.

1. `bot.cpp` currently has `debug = true` and `probe()` fuel prints (FUEL start/pre-reach/reach/
   observed/pre-goal/goal/end, TURN lines). Set `debug = false` before any submit.
2. Fuel is fine after round 1 (≈40k a turn on 00packs). The 60-round debug game shows the three
   champions walking toward target group 2/3 from spawn. The full game had 0 gems and 21 deaths with
   8.7k champion damage taken per body: they are reaching camps but barely attacking (240 monster
   damage in 841 rounds) and dying to the enemy champions. Suspects, in order: (a) `attack_camp()`
   never finds a `CampSeen` for the target (the `camps` list is only filled for groups whose raw
   state is 1 and within 10 tiles; check `c.group != int(&g - groups.data())` and that `read_camp`
   succeeds for camps in view); (b) `farm()` marks groups unsafe (`safe_to_clear`) and the champions
   wander; (c) `fight()` engages the baseline's 3-stack and loses; (d) `go()` reuses `visible` as the
   BFS scratch board and then refreshes it, but `reach_jump` is also reused as the jump goal field
   and `reach_jump_ok` flips false, so later `jump_to_group` reads a stale field in the same turn.
   Run `--rounds 200` with debug on, grep TURN/TARGET/FIGHT lines, and watch gems/hp per round.
3. When it farms: judge locally against `bots/baseline` on 6-8 maps both seats (`evaluate.py
   --bot pack --baseline baseline`), then the practice block (same maps/seats/opponents as a v119
   reference block in the same hour, see `analysis/pg10/test_v119.sh`), then activate if it wins.
4. Ideas not yet in the bot: TP crystals (10 gold, 3 tiles, 3-turn channel) for walled centres
   (quadrants); altar locks (10 gold) on the enemy's home altar; Capture emblem on one farmer; using
   `set_priority` so finishing blows act first; Cassi&Issac roster (every top team drafts it).

## What the baseline taught (so the new bot does not repeat it)

Every tuning lever on the baseline (packs at the clear, twin roster, chains of dashes, health
builds, death-site penalties, bank bag size, cohesion, gold value, first-turn fit, Varg roster)
measured flat or negative in practice. The single kept gain of 2026-11-03 was pocket entries over
holes (v119). The open gap is the fight class: our champions farm cheaper groups and lose the first
team fight. The top teams (Mite, Intercontinental Fish, Ad Astra) draft Cassi&Issac plus Vesper,
Vendetta, Heart of Ruin, Marcella, Varg, Prism or Willow, clear rich groups in packs of 2-3,
bank bags of ~47 gems 29 times a game, and win fights with numbers. Compare their ranked replays
(`analysis/pg10/fetch.sh`, `capseq.py`, `gamesum.py`) on our worst maps before guessing.
