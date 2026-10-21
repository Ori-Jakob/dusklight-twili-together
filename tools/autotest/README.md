# Twili-Together autotest

Runs real game instances with the Twili-Together mod (and, for network scenarios, a private relay
server) and checks what each instance reports, its log, and that nobody crashes. Each scenario gets
a PASS/FAIL per instance, the game logs, the server log, and the crash report if an instance
crashed.

From the mod's folder (this repository, or `mods/twili_together` in a Dusklight tree):

```
node tools/autotest/run.js --exe <dusklight.exe>                  # every scenario
node tools/autotest/run.js --exe <dusklight.exe> smoke            # one scenario
node tools/autotest/run.js --list                                 # what exists
node tools/autotest/run.js --exe <dusklight.exe> smoke --repeat 5 # soak
```

Options: `--exe <dusklight.exe>` (a Dusklight build from the fork with Twili-Together in its
`mods/` folder; inside a Dusklight tree the default is `build/windows-clang-relwithdebinfo/`),
`--work <dir>` (slot directories, default `work/`), `--out <dir>` (reports, default `out/`),
`--timeout <sec>`, `--repeat <n>`, `--no-kill`, `--tag <tag>` (adds every scenario with that tag,
manual ones too, in definition order; with `--list` it lists them).

Test from a copy of the build output (the exe, its DLLs and PDB, `res/` and `mods/`) so the build
directory can relink while tests run, and give concurrent runs their own `--work`:

```
node tools/autotest/run.js smoke --exe <copy>/dusklight.exe --work <dir>
```

Needs a GPU (there is no headless mode) and a user config with a valid disc image path: each run
takes it from `%APPDATA%/TwilitRealm/Dusklight/config.json`. Instances open 800x480 windows in a
row, muted. Don't minimize them: a minimized window skips frames.

Exit code is 0 when every scenario passed. Output goes to `out/<timestamp>/<scenario>/`:
`report.json`, `<instance>.log`, `<instance>.stderr.log`, `<instance>.result.json`,
`<instance>.script.json` and, for network scenarios, `server.log`. `out/<timestamp>/summary.json`
has every report of the run; with `--repeat` each round goes to `out/<timestamp>/run<N>/`.

## Running the whole suite

Without scenario names the runner runs everything except the manual scenarios (`manualOnly`:
window tours and showcases that only exist for screen captures; run them by name). That is about
130 scenarios and takes around two hours on one runner. To go faster, split the list over two
runners, each with its own `--work` and `--out`:

```
node run.js <first half of the names>  --exe <copy>/dusklight.exe --work w1 --out out1 --repeat 3
node run.js <second half of the names> --exe <copy>/dusklight.exe --work w2 --out out2 --repeat 3
```

More than two runners (or other heavy work on the machine) makes the timing-sensitive scenarios
flaky: they count game ticks, and a starved instance falls behind its peer.

The relay has its own tests, no game needed: `npm test` in `../server`.

Not covered: reloading or disabling the mod from the Mods window while two instances are
connected. Check that by hand after changes to startup, shutdown or the actors.

## Isolation

Every instance runs with `--user-dir work/slot<N>` (config, memory card, achievements, mod data
and the mod cache) and `--log-dir work/slot<N>/logs`. The game's cache directory (pipeline caches,
and the logs without `--log-dir`) is the SDL preference path, which `--user-dir` does not move, so
the runner also points `USERPROFILE`, `APPDATA` and `LOCALAPPDATA` at `work/slot<N>/home`. A run
therefore never writes the player's real game data.

Before each scenario the runner rebuilds the slot's `config.json`: from the user's config only the
disc image (`backend.isoPath`, `backend.isoVerification`), every other setting at the game's
default, plus the test settings: autosave and pause on focus loss off, background controller input
off, audio muted, windowed 800x480, UI scale 100; a scenario that needs another value sets it in
`cvars`. It deletes the slot's saves, achievements, logs and mod data. The first run seeds each
slot's shader caches from the user's data folder; later runs reuse them.

Mods: every `.dusk` next to the exe loads by default, and the in-tree build puts all in-tree mods
there (`custom_actor_demo` alone places a rock and mines in South Faron). The runner reads each
bundle's `mod.json` and disables every mod except Twili-Together and the `enableMods` of the
scenario and of the instance.

Fixture mods (`tools/autotest/mods/`, asset-only) are packaged into `mods/autotest/` next to the
exe, a folder the game does not scan. An instance that enables one gets a copy in its slot's own
`mods/` folder (the user mods directory under `--user-dir`), so only that instance loads it.

The Prelaunch menu is skipped by passing `--stage <start stage>` with `backend.skipPreLaunchUI`
off. With that setting on, the host opens Prelaunch over the running game whenever a mod registers
a game mode (the randomizer does); the boot takeover still happens underneath, but while Prelaunch
is open cutscenes do not update and the memory card thread never starts. The mod's boot takeover
replaces the `--stage` handling anyway.

Test instances ignore the machine's controller: background input is off in the config, and the
mod's pad hook zeroes port 0 whenever no scripted input runs.

## How an instance runs a script

`--cvar mod.dev_n0ted_twili__together.autotest_script=<script.json>` (src/autotest/) makes the mod
take over the boot (pre-hook on `dScnLogo_c::nextSceneChange`): it builds a fresh save and loads
`start`. It then runs one step per simulation tick from `mod_update` and logs each as
`[autotest] step i/n {...}`. The run ends with `[autotest] RESULT PASS|FAIL`, a result file, and
`HostService.request_quit` with exit code 0 (pass), 2 (a step failed) or 3 (global timeout). On a
host without `request_quit` the runner stops the game 30 s after its result file appears.

Without the cvar the autotest is dormant: no autotest hooks, no ticks (scenario `dormant`).

The mod's own settings are `--cvar` overridable per instance: `tt("name", value)` in `lib.js`
builds `mod.dev_n0ted_twili__together.<name>=<value>` (the loader escapes `.` to `_` and `_` to
`__` in mod ids).

`start` fields: `stage`, `room`, `point`, `layer` (-1 = from story flags), `horseName` (the save's
horse name, default "Epona"), `status` (`armed` = tunic, sword and shield, the default; `new`;
`debug` = full inventory plus some story event bits), `clearTwilight` (default false: it moves many
stages to other layers, Ordon Village to a cutscene layer), `hour`, `eventBits` (event bit numbers
set before the first load, so it already picks the story layer: `[0x4510]`, Day 2 complete, puts
South Faron in twilight), `levels` (`{transform: [n], darkClear: [n]}`) and `firstPickups`
(default false: rupees count as seen before, so a dropped rupee never opens the first-pickup
message that pauses the world).

Story checkpoints (`lib.js` `STORY`, one per area) use more fields, all applied before the first
load: `fixture` (a save captured with `saveFixture`, read from `fixtures/<name>.json`; the other
fields apply on top), `saveSwitches` (`[{stage, no}]`), `items` (item numbers, given through
`execItemGet`), `collect` (`{crystal: [n], mirror: [n]}`), `lightDrops` (`{area: count}`),
`vessels` (areas whose Vessel of Light is held), `transformStatus` (`wolf`, `human`), `stageBoss`
(stages whose boss is defeated) and `midna` (`ride`, `off`).

Pick start and warp stages that load without a forced cutscene on a fresh save; the ones in
`lib.js` `STAGES` are known to.

### Steps

Timings in `frames` are simulation ticks (30 per second), independent of the frame rate.

| op | fields | does |
| --- | --- | --- |
| `waitStage` | `stage`, `timeoutSec` | waits until the player exists and no stage change is pending for 60 ticks |
| `warp` | `stage`, `room`, `point`, `layer` | requests a stage change |
| `walk` | `frames`, `stickX`, `stickY`, `circle`, `buttons`, `async` | synthetic port-0 input (L and R also pull their analog triggers); `circle` walks in a slow loop; `async` lets the next steps run meanwhile |
| `wait` | `frames` or `sec` | |
| `markPos` / `expectPos` | none; `minMoved`, `near` (`[x, y, z]`), `maxDist` | remember Link's position / fail unless he moved at least `minMoved` since, or is within `maxDist` of `near` |
| `connect` | `name`, `room`, `team` (default: instance name, run room, no team) | connects to the run's server |
| `disconnect` | | |
| `waitConnected` | `timeoutSec` | until the server assigned us a client id |
| `waitPeers` | `count`, `sameStage`, `timeoutSec` | until that many other clients are online (in our stage and layer) |
| `expectPeers` | `count`, `sameStage` | fails unless exactly that many |
| `waitDummies` / `waitNoDummies` | `count`, `timeoutSec` | until that many remote-player actors exist / none do |
| `checkDummies` | `maxDist` | fails if a peer in our layer has no dummy, or its dummy is further than `maxDist` from the peer's reported position |
| `signal` / `waitSignal` | `name`, `from`, `timeoutSec` | barrier between instances, relayed by the server |
| `giveItem` / `expectItem` | `item`, `timeoutSec` | `execItemGet` (a gameplay give, so it is shared) / wait for the item bit |
| `burnShield` / `expectShield` | none; `item` (255 = none), `owned`, `forSec`, `timeoutSec` | what a wooden shield burning up does (unequipped, first-get bit cleared) / wait for the equipped shield (and its first-get bit) |
| `addKeys` / `expectKeys` | `count`; `forSec`, `timeoutSec` | add to the current stage's small-key count / wait until it equals `count` (and, with `forSec`, stays so that long) |
| `setSwitch` / `unsetSwitch` / `expectSwitch` | `no`, `room` (default: current), `set` | save switch through `dComIfGs_onSwitch` etc. |
| `setEventBit` / `expectEventBit` | `no`, `set` | permanent event bit |
| `log` | `msg` | |
| `quit` / `fail` | `reason` | end the run (`quit` disconnects cleanly first) |
| `exitNow` | | pass and exit immediately without disconnecting, like a crash or a killed process |

A script that runs out of steps passes. The network steps need the session (it installs the
autotest's `NetDriver`); without it they fail with "needs the network session".

### Feature steps

Features register more steps from their own files through `src/autotest/AutoTestSteps.hpp`.

Motion (`StepsMotion.cpp`):

| op | fields | does |
| --- | --- | --- |
| `poseSelfTest` | | playout buffer rules and the cutscene classifier; needs no peer |
| `waitRoomState` | `key`, `value`, `timeoutSec` | until the room state's `key` equals `value` |
| `forceCutscene` | `on` (true, false, null = real events) | forces our cutscene state |
| `beginEvent` / `endEvent` | `kind` (`compulsory`), `timeoutSec` | orders a compulsory event / resets the running one |
| `expectDummyHidden` | `hidden`, `timeoutSec` | every peer in our layer has a dummy, hidden (or shown) |
| `expectPeerFlag` | `flag` (`inCutscene`, `wolf`), `set`, `timeoutSec` | the PLAYER_UPDATE presence flag of every peer in our layer |
| `measureDummyMotion` | `frames`, `settleFrames`, `minSpeed`, `maxBadRatio`, `self` | per-tick motion of the first peer's dummy (or our player) and its playout clock; fails on stalls and bursts |
| `dumpDummies` | | logs every remote client's pose and dummy state |
| `teleportSelf` | `dx`, `dy`, `dz` | moves our player at once |
| `expectDummyTeleport` | `minDist`, `maxPreStep`, `timeoutSec` | the first peer's dummy jumps `minDist` within one frame, without a slide before |
| `measureBodyHeight` | `frames` (150), `settleFrames` (20), `self`, `jerk` (5), `maxJerkRatio` (0.05), `minTravel` (60) | the height of the first peer's dummy's body matrix (or our player's) per tick: fails when its climb rate changes by more than `jerk` on too many ticks |
| `findSlope` | `minDeg` (12), `maxDeg` (30), `radius` (1200), `step` (60) | puts our Link on the nearest steady slope around it, standing across it (none near Faron Field's start; South Faron's spawn is on one) |
| `expectFeetOnGround` | `self`, `maxSpread` (4), `minBend` (0), `contact` (true), `frames` (30), `trace`, `timeoutSec` | the feet (paws) of the first peer's dummy (or ours) stand at the same height over our ground, the legs bent at least `minBend` degrees, and the dummy's leg adjustment on |
| `expectDummyAirborne` | `timeoutSec` (5) | the first peer's dummy has its feet off the ground (its sender's are) |
| `measureSelfDrift` | `frames` (150), `settleFrames` (10), `maxDrift` (3), `needDummy` (true) | our player, standing without input (next to the first peer's dummy), moves at most `maxDrift` |
| `expectDummyBody` | `rootX`, `rootZ` (cleared to these), `rootMatch` with `rootTol` (0.5), `notStanding`, `magne`, `up` (`[x, y, z]`) with `upTol` (0.05), `tweak`, `turnDeg` or `tweakA`/`tweakB` with `turnTol` (2), `minTicks` (1), `timeoutSec` | the first peer's dummy's body matrix extras ("ym") on `minTicks` ticks: the root where the sender's calc put it (`notStanding`: not the old standing pin), the body matrix's up axis and its turn beyond the shape angle |

The thresholds of `puppet-ground-steady` and `puppet-feet-slope(-wolf)` come from their runs: the
body's climb rate changed by at most 1.45 a tick, ours and the dummy's (`jerk` 3), a human's feet
stood 0.3 apart (`maxSpread` 2) and a standing wolf, B's own too, keeps a paw up to 7.6 over its
ground (`maxSpread` 10).

`puppet-root-matrix`: B's plain slash clears the root's Z to -7 (the dummy used to pin the standing
5); the patched "ym" phases (a ladder-style root, a magnetic-boots ceiling, a horse run tweak, a
climbing wall's turn) come out exactly: root (12, -20), up (0, -1, 0) at 180 degrees, 28.3 and 45.

`puppet-no-push`: 40 units from B's dummy (the colliders overlap), PvP off and on, A's Link moved
0.00 in 150 ticks: the dummy registers no co collider, only its PvP hurtbox. A player standing alone
on Hyrule Field's event tag 36 spot slides about 22 units a second on its own (the slope), dummy
or not.

`cutscene-hide-personal`: Hide Players in Cutscenes is personal. A has it on, B off: A's cutscene
and B's both hide B's dummy on A, and B keeps seeing A through both.

World sync (`StepsWorld.cpp`):

| op | fields | does |
| --- | --- | --- |
| `setRoomOption` / `waitRoomOption` | `name` (`syncWorldState`, `shareWoodenShield`, `sharePickups`, `teleportMode`, `syncNPCs`, `syncEnemyDamage`, `pvpMode`, `pvpFriendlyFire`, `pvpLethal`, `showLocationsMode`, `cutsceneSync`, `syncPushPull`), `value`, `timeoutSec` | sets our room-setting default (the owner's is the room's, so every instance sets it) / waits until the room reports that value |
| `hidePlayersInCutscene` | `value` (true) | our own Hide Players in Cutscenes setting (personal, not a room option) |
| `forceLayout` | `layout` (16 hex digits) | before `connect`: announce another save layout, as a different game build would |
| `waitLayoutMismatch` | `peer`, `timeoutSec` | until the incompatible-layout toast was shown for that peer |
| `expectNoItem` | `item`, `forSec` (5) | fails if the item's first-get bit gets set within `forSec` |
| `expectMerges` | `min`, `max`, `timeoutSec` | world-state merges applied so far: waits for `min`, fails above `max` |

Blocks and chains (`StepsPush.cpp`, scenarios in `scenarios/blocks.js`). `kind` is `movebox`
(default), `iceblock`, `swturn`, `lv6swturn`, `pushdoor` or `swchain`; `near` (`[x, z]`) picks the one
placed nearest, else the one nearest our Link. A turning object's spot is its step times 100, at
`[step * 100, 0]`:

| op | fields | does |
| --- | --- | --- |
| `pushBlock` | `kind`, `near`, `dir` (0 to 3 in the block's frame; default: any), `firstDir` (0), `pull`, `timeoutSec` (10) | what our Link's push does to the block, each tick until it starts moving; without `dir` it tries the next direction every 10 ticks |
| `expectBlock` | `kind`, `near`, `at` (`[x, z]`) with `tol` (5), `step` (a turning object's), `moved` (off its placement), `moving`, `minApplied` / `maxApplied` (teammate moves replayed), `minSnapped`, `matchesRemote` (on the spot the last teammate move or rest named), `stillFrames`, `timeoutSec` (30) | until all hold |
| `signalBlockPos` / `expectBlockPosSignal` | `kind`, `near`, `name` (`blockpos`); `from`, `tol` (5), `timeoutSec` | sends our block's spot in a signal / waits for the peer's and fails unless ours matches |
| `approachChain` | `near`, `dist` (40) | puts our Link beside the chain switch's handle, on its far side from the base, facing it |
| `pullChain` | `near`, `untilSwitch`, `untilOut`, `frames`, `pullPower` (0), `trace`, `timeoutSec` (30) | holds R (picking the chain up) and then pulls away from the base until the switch is on, that many links are out or the frames ran out; keeps holding R, still, afterwards. The stick pulls at about 6, under the 10 a switch takes without iron boots: `pullPower` sets the chain's tension to that once every link is out, until its force pull starts. `trace` logs the pull every 10 ticks |
| `releaseChain` | | lets go of R |
| `expectChain` | `near`, `driven` (follows a teammate's pull), `held` (by our Link), `minOut` / `maxOut`, `minDrivenTicks`, `endNear` with `tol` (150), `timeoutSec` (30) | until all hold |
| `expectChainAtPuppet` | `near`, `maxDist` (120), `timeoutSec` (10) | the chain's handle is within `maxDist` of the puller's puppet |
| `dumpPush` | | logs every tracked block and chain with its counters |

Block and chain sync: `blocks-push` (Goron Mines room 6 block, B replays A's push and stops on A's
spot), `blocks-off` (Sync Blocks and Chains off: B's block stays), `blocks-both` (both push the same
block at once from opposite sides; the lower client id's push wins and the other snaps to it),
`blocks-pathed-late` (a Death Mountain block saved in switch 0x3a: B sees the first push live, leaves,
and finds the saved spot when it comes back), `blocks-ice` (a Snowpeak courtyard ice block slides for
B too), `blocks-turn` (an Arbiter's Grounds turning wall turns for B too and its switch reaches B)
and `blocks-chain` (a City in the Sky chain switch: B's chain follows A's puppet, the switch reaches
B as a flag, and the chain lets go with A; Arbiter's Grounds' entrance chain lies on sand that
voids Link out).

Items the game takes away (`StepsTake.cpp`, scenarios in `scenarios/take.js`). The take steps use
the game's own calls outside any remote apply, so the take poller sees them:

| op | fields | does |
| --- | --- | --- |
| `takeItem` | `item`, `slot` (default: the item's) | what the game does to take it: `dComIfGs_setItem(slot, NONE)` (the lantern also backs up its oil, as the monkey does), `dComIfGs_setWarashibeItem(NONE)` for slot 21, `dComIfGs_offItemFirstBit` for the pumpkin (0xF4) and cheese (0xF5), `dMeter2Info_setSword(NONE, false)` for the worn Wooden Sword (0x3F, Talo's cut) |
| `setTradeItem` | `item` | the "Trade Item Set" flow event: `dComIfGs_setWarashibeItem(item)`, no item get |
| `restoreLantern` | | what a game over during the monkey's theft does: the lantern back in slot 1 with its backed-up oil |
| `equipShield` / `equipSword` | `item` | puts that shield / sword on as the collection screen does (`dMeter2Info_setShield` / `setSword`); `burnShield` then removes the shield |
| `expectSlot` | `slot` (default: `item`'s), `item` (255 = empty), `forSec`, `timeoutSec` | until the slot holds the item (and, with `forSec`, keeps it that long) |
| `expectFirstBit` | `item`, `set` (true), `forSec`, `timeoutSec` | the same for a first-get bit |
| `expectSword` | `item` (255 = none), `forSec`, `timeoutSec` | the same for the sword worn |
| `expectEquipItem` | `item` (0x103, the sword drawn), `timeoutSec` | what Link holds in hand (`mEquipItem`) |
| `expectTakeStats` | `min`, `max` (`{sent, received, applied, skipped, mergeTakes, deferred, regrants, returns}`), `timeoutSec` | waits until every `min` count is reached, fails at once above a `max` count. `applied`: a teammate's take removed our item; `mergeTakes`: a world state showed one taken; `deferred`: takes that had to wait; `regrants`: a taken item granted again; `returns`: items back without an item get, sent as `GIVE_ITEM` |
| `holdTakes` | `hold` (true) | teammates' takes wait as if it were never safe, until released |
| `publishWorldState` | `timeoutSec` | once world sync is on, sends our world state as an autosave does (the relay caches it and drops the queued packets it holds) |
| `expectLantern` | `inHand`, `lit`, `timeoutSec` | our lantern is held out (`mEquipItem`) / lit (on the belt too) |

`take-offline-join` launches B 45 s ahead of A: B takes Auru's memo and publishes its state before
A exists. `take-lantern-in-hand` puts the lit lantern away with A ("Put away"); it then hangs lit on
the belt, and the take puts it out. `take-wood-sword` starts unarmed in South Faron (a grant keeps
the sword already worn), sets F_0026 before Talo's unequip as the flow does, draws B's sword with B
and sheathes it with A. Written for the item takes, **not run yet** (the machine was in use):
`take-live`, `take-lantern`, `take-first-bits`, `take-offline-join`, `take-shields`,
`take-randomizer`, `take-lantern-in-hand`, `take-wood-sword`.

Boss fights (`StepsBoss.cpp`, scenarios in `scenarios/boss.js`). Each player fights their own boss;
while B fights, a teammate's kill, heart container and warp out of the arena must change nothing:

| op | fields | does |
| --- | --- | --- |
| `bossGuard` | `on` | off: our fights are still seen, but nothing is held, deferred or kept out (the observe scenarios) |
| `keepLife` | `on` (true) | refills our hearts every tick, so standing in the arena never ends in a game over |
| `waitBossFight` | `value` (true), `quietTicks` (60), `pressA`, `timeoutSec` (120) | our boss fight runs (`value`) and, for true, no event ran for `quietTicks` (the intro is over) |
| `expectBossFight` | `value`, `timeoutSec` (10) | whether our boss fight runs |
| `killBoss` | `how` (`real`, `flags`), `timeoutSec` (30) | the live boss of our room ends its fight. `real` as its own code does (Diababa: `ACTION_END` with demo mode 50, Fyrus: `ACTION_END`, the paths of their debug pads): death scene, heart container, warp and BOSSCLEAR. `flags`: only the flag writes that end its defeat scene (Dangoro: the mini-boss bit and switch 14; Diababa and Fyrus: their boss bit, switches and clear bit) |
| `bossWatch` | `tag`, `expect` (true), `sec` (20) or `until`/`from` (a peer's signal) with `holdSec` (15), `timeoutSec` | watches our fight: we stay in the room, the boss lives and is not dying, no event starts that it did not order, no warp or heart container appears, no story prompt or load, and our boss and mini-boss bits stay clear (Dangoro draws its armor only while the mini-boss bit is clear). `expect`: the first change fails the step; else each is logged once as `[autotest] BOSSWATCH <tag> interrupt <what>`. The end logs `BOSSWATCH <tag> end` with the switches, item, dungeon and event bits and max life that changed |
| `expectBossClear` | `heart` (1), `warp` (1), `soft`, `timeoutSec` (240) | after our kill: our boss or mini-boss bit is set and no event ran for 90 ticks (A pressed through the text), then that many boss heart containers and warps lie here; `soft` only logs a difference |
| `expectBossHeld` | `min`, `max`, `released`, `timeoutSec` (20) | teammates' packets held for our fight now, and applied after one so far |
| `expectDungeonBit` | `stage` (ours), `bit` (3), `set` (true), `timeoutSec` (20) | a dungeon bit of that stage's table (0 map, 1 compass, 2 boss key, 3 boss beaten, 4 heart container taken, 7 mini-boss beaten) |
| `expectStageSwitch` | `stage` (ours), `no`, `set` (true), `timeoutSec` (20) | a memory switch of that stage's table |
| `markMaxLife` / `expectMaxLife` | `delta` (5 per heart container), `forSec`, `timeoutSec` (30) | our max life since the mark |
| `expectCollect` | `crystal` or `mirror` (bit), `set` (true), `timeoutSec` (30) | a Fused Shadow or Mirror Shard bit of our collection |
| `expectEventCount` | `name`, `prefix`, `count` (1) | events of that name (or prefix) accepted here since the script began |
| `expectNoCutsceneJoin` | `name`, `sec` (20), `tag` | no pull-in copy of a teammate's event of that name for `sec` (logs `CUTSCENE PASS`) |

`boss-diababa` and `boss-fyrus` (tag `boss-fight`): both start in the arena with the boss alive
(lib.js's boss checkpoints without `stageBoss`) and wait for its intro. A kills its boss for real,
takes the heart container and leaves through the warp; B's `bossWatch` runs until 20 s after A
arrived (A's world state merged); the heart container's bit and grant came through meanwhile (+5 max
life), and so did A's Fused Shadow with its toast; the boss bit did not. Then B kills its own boss: one BOSSCLEAR, one warp, no heart container (A
took the team's), the prompt still waiting; B warps out, the held packets apply, no prompt comes (A
went where B's own warp went), and both end with +5 max life and the boss and heart bits.
`boss-dangoro` does the same for a mini-boss with `killBoss` `flags` and a walk out of the stage.
The `-observe` variants (manual, tag `boss-fight-observe`) turn B's guard off and only log what A's
kill changes in B's fight, then B's own clear (`expectBossClear` `soft`).

Chests, freestanding items, shared pickups and traps (`StepsPickups.cpp`):

| op | fields | does |
| --- | --- | --- |
| `spawnChest` | `tag`, `tbox` (TBOX number), `shape` (0 small, 1 big, 2 boss), `item`, `dist` (110) | a closed chest `dist` units in front of our player, facing him |
| `openChest` | `tag` or `tbox`; `refused`, `frames` (60), `approach`; `timeoutSec` | orders the chest's open as an A press does and presses A through the item message, until its bit is set and the event ended / with `refused`: for `frames` ticks no open may start an event. `approach`: our player first stands in front of the chest, facing it (a stage's own chest) |
| `expectChestOpen` | `tag` or `tbox`, `open` (true), `forSec`, `timeoutSec` | the chest's TBOX bit is set and its lid at the last frame, with no event of ours (or, `open: false`, still closed) |
| `expectTbox` | `no`, `tbl` (another stage table: its save slot), `set`, `forSec`, `timeoutSec` | a TBOX bit |
| `spawnFieldItem` | `tag`, `item` (0x21/0x22: a heart piece or container), `bit` (item bit), `dist` (150), `check`, `refused` | a resting field item `dist` units in front of our player. `check`: dropped as a boss or NPC drops one (`fopAcM_createItemForBoss`) under that check name; `refused`: a failed create is fine (a taken heart piece) |
| `takeFieldItem` | `tag`, `holdSec` (0), `timeoutSec` | puts the item on our player until he takes it, pressing A through its message only after `holdSec`; done when it is gone and no event runs |
| `holdFieldItem` | `tag`, `hold` (true) | marks the item as pulled by the clawshot, or lets it go |
| `findFieldItem` | `tag`, `bit` (0x80), `present` (true), `forSec` (3), `timeoutSec` | tags the stage's own item (or heart piece) with that item bit, like the fishing hole's heart piece / with `present: false`, fails if one is there before `forSec` passed |
| `markFieldItem` | `tag`, `wolfFlag` (event bit), `hooked` | turns a heart piece into the randomizer's golden wolf item with that flag / hooks it on a lure (the rod's `startCtrl`) |
| `takeCheck` | `check`, `item`, `switch`, `itemBit`, `eventBit`, `eventLabel` (a `saveBitLabels` index), `tag` | takes a check as its game code does without the actor: sets the flags given, deletes the tagged field item and grants `item` silently under the check name |
| `expectFieldItem` | `tag`, `present` (true), `forSec`, `timeoutSec` | the item exists |
| `expectItemBit` | `bit` (below 0x80 dungeon, else memory), `tbl` (another stage table: its save slot), `set`, `forSec`, `timeoutSec` | an item bit |
| `setInventory` | `rupees`, `life` (quarter hearts), `arrows`, `maxLife` (heart pieces, 5 a heart) | sets them in the save |
| `expectInventory` | `rupees`, `life`, `arrows`, `bombs` (bag 0), `seeds`, `poes`, `bottles` (filled slots), `maxLife`, `keys` with `tbl`; `forSec`, `timeoutSec` | waits until each given value matches (and holds `forSec`) |
| `expectPickups` | `sent`, `applied`, `skipped`, `rupees`, `life`, `arrows`, `seeds`, `bombs`, `trapsSent`, `trapsReceived`, `trapsApplied`, `grantsApplied`, `grantsQueued`, `removed`, `blocked`, `deduped`, `actorsRemoved`, `spawnsBlocked` (freestanding items removed, pickups stopped, GIVE_ITEMs dropped or a second own heart piece taken back, Poes and golden wolves removed, heart pieces and containers never placed); `min`/`max` objects of the same; `forSec`, `timeoutSec` | the SHARE_PICKUP, GIVE_ITEM and placed-check counters |
| `setLightDrops` | `area`, `num` | sets that area's tear count here only (a lost tear) |
| `itemNamesEnglish` | `value` (true, false, null = the disc's) | forces the item names' disc language |
| `expectItemNames` | `english`, `items` (`item`, `name`, `source`: `game`, `mod`, `fallback`, `unknown`), `timeoutSec` | the language the names see and each item's name and source |

Written for the chest sync, shared pickups, randomizer items, traps and toast batching, and **not run
yet** (the machine was in use): `chest-sync`, `chest-sync-away`, `pickup-share`, `pickup-no-replay`,
`rando-item-sync`, `rando-trap-spread` and `item-toast-burst` (`scenarios/pickups.js`).
`chest-sync-overlap`: both open one heart piece chest at once, each gets the piece once.

Written for freestanding items and item names, **not run yet** (the machine was in use):
`freestanding-sync`, `freestanding-overlap`, `freestanding-busy`, `freestanding-away`,
`item-name-audit` and `item-name-audit-rando` (`scenarios/freestanding.js`). Placed checks
beyond items: `poe-soul-sync` (a taken Poe fades for B, one soul each), `golden-wolf-sync` (a
learned skill removes B's golden wolf, a taken golden wolf item vanishes and arrives once) and
`fishing-heart-sync` (the fishing hole's heart piece leaves B's lure); `item-name-language` checks
the corrections are English-only (the US disc forced to another language). `story-tears-tab`
(`scenarios/story-late.js`) shows each teammate's tear count, and B's lost tear, in the Players tab.

Heart pieces a teammate took (`scenarios/hearts.js`): `heart-live` (A takes Hyrule Field's own tree
heart piece, bit 0x81, with B in that room: B's copy vanishes, one piece each, gone when B comes back),
`heart-away` (B in Link's house: the bit lands in B's save slot, the piece is not there when B
arrives), `heart-drop` (the springwater heart piece dropped at run time under `goron_springwater_rush`:
B's dropped copy goes, B's next drop is refused), `heart-drop-overlap` (both take their own drop of it,
A holding its message: one piece each), `heart-chest` (Kakariko Gorge's own heart
piece chest 5: opens for B, refused to B, open after coming back), `heart-rando-reward` (randomizer:
both win Plumm's reward check at once, one piece each) and, tag `boss-fight`, `heart-boss` (A takes
Diababa's heart container in the temple, B's own kill in its arena drops none, +5 once) and
`heart-boss-rando` (randomizer: A's check before its bit, B's kill places nothing, `spawnsBlocked` 1).

Items held up: chests, item gets, trades (`StepsGetItem.cpp`):

| op | fields | does |
| --- | --- | --- |
| `expectLocalGetItem` | `item` (shown), `real` (given), `shows` (1), `ticks` (0), `timeoutSec` (30) | what our capture sent: the latest item and the totals |
| `expectRemoteGetItem` | `item`, `near` (`[x, y, z]` from the dummy, in its facing frame) with `tol` (30), `minScale`, `emitters`, `maxFailures` (0); `drawn: false` (held up there, not drawn here); `none` (nothing wanted, built or loading); in every mode `light` (lit now), `minLightPow`, `sound` (the Poe soul's now), `minSoundTicks`, `minLightFades` with `minLightFadeTicks`, `minSparkleFades` with `minSparkleFadeTicks`/`maxSparkleFadeTicks`; `frames` (0), `timeoutSec` (30) | until the first peer's dummy shows it so, then for `frames` more ticks |

Written for items held up on a puppet, **not run yet** (the machine was in use): `getitem-chest`,
`getitem-heart-piece`, `getitem-hidden` and `getitem-rando` (`scenarios/getitem.js`).
`getitem-heart-piece` also checks daDitem_c's light on the puppet's item (power 6.4 of 8 when the
item went), its fade (8 ticks) and the heart sparkles' fade-out after it (10 ticks, 26 alpha a
tick); `getitem-poe-soul` the Poe soul's sound on every tick it is held up.

Fidelity (`StepsFidelity.cpp`):

| op | fields | does |
| --- | --- | --- |
| `setClothes` | `item` (0x2E Ordon, 0x2F Kokiri, 0x30 Magic Armor, 0x31 Zora), `timeoutSec` | changes our clothes like the collection screen; done once the new body is bound |
| `setBoots` | `on` | iron boots on (assigned to X) or off; done once that held for 10 ticks |
| `patchPlayerUpdate` | `patch`, `packets` (60) | merges `patch` into our next PLAYER_UPDATEs, each a keyframe |
| `assignItemX` | `item` | an owned item on X (press it with a walk's `buttons`: X = 0x400) |
| `expectDummyLook` | `clothes`, `casualHead`, `heavyBoots`, `zoraMask`, `lantern`, `heldItem`, `basePack`, `basePackAnm`, `standIn`, `ground`, `hidden`, `armorDrained`, `armorSettled`, `maxMissing`, `frames`, `timeoutSec` | the first peer's dummy shows every field given, then for `frames` more ticks |

Wolf, Midna and transformations (`StepsWolf.cpp`):

| op | fields | does |
| --- | --- | --- |
| `setForm` | `form` (`wolf`, `human`) | the form Link spawns in on the next stage load (follow with a warp) |
| `transform` | `form`, `trace`, `midnaSent`, `midnaLog`, `voice`, `timeoutSec` | transforms our player; with `trace` every stage and every expected emitter must show (`midnaSent`: we sent Midna on every tick of each stage her game drew her; `midnaLog`: a line per tick); `voice` plays Midna's demo's transformation voice |
| `expectLocalForm` | `form` | |
| `expectRemoteForm` | `form`, `body` (form or `hidden`), `count`, `timeoutSec` | peers send `form` and their dummies show `body` |
| `expectCleanAnims` | `frames` (120), `maxRefused` (0) | no dummy refuses more clips than that |
| `setMidna` | `ride`, `visible`, `timeoutSec` | Midna on our wolf's back (visible in the light world: transform level 3) |
| `callMidna` | `timeoutSec` (20) | orders the Midna talk Z orders (Midna must be with our Link); done once she is out as her shadow, past her appear clip. The talk stays open |
| `expectRemoteMidna` | `mode` (`drawn`, `shadow`, `apart`, `none`), `upper`, `hairHand`, `leftHand`, `rightHand`, `tired`, `silhouette`, `maxRefused`, `count`, `frames`, `timeoutSec` | peers' dummies pose their Midna so; `silhouette` (with `apart`): the peer sends her as her shadow and its dummy drew her through the gokou and invisible models on one of the last two ticks |
| `midnaSfx` | `sound`, `voice` (true) | our Midna starts that creature voice or sound (sent as PLAYER_SFX) |
| `markRemoteMidna` / `expectRemoteMidnaSince` | `minSfx`, `minApartTicks`, `minShownTicks`, `timeoutSec` | the first peer's dummy heard that many Midna sounds and showed her that long since the mark |
| `measureRemoteMidnaTransform` | `minSent` (`{A, swap, C, after}`), `maxMissed` (1), `maxGap` (0), `afterTicks` (30), `timeoutSec` | through the first peer's next transformation (and `afterTicks` after it), per stage: the ticks its pose had Midna against the ticks its dummy drew her, and the ticks without her between the first and last |
| `expectRemoteSfxTiming` | `sound`, `maxLag` (1), `transform`, `timeoutSec` | the first peer's dummy played that PLAYER_SFX within `maxLag` ticks of the pose tick it was made in (and of the transformation's start) |
| `transformFxSelfTest` | | replay planner and codec; needs no peer |
| `expectRemoteTransformFx` | `form`, `maxLag`, `minSilhouette`, `fur`, `maxAnchorErr`, `skip`, `hidden`, `anchorSource`, `timeoutSec` | the first peer's next transformation as sent and as its dummy replayed it |
| `expectNoTransformFx` | `frames` | the dummy replays no transformation |
| `expectDummyTransformFx` | `emitters`, `tev`, `silhouette`, `anchorSource`, `frames`, `timeoutSec` | exactly these emitters this tick, and the other fields |
| `waitRemoteTransformPhase` | `phase` (`A`, `swap`, `C`), `timeoutSec` | until the first peer sends that stage |

`midna-transform`: turning human, B's game draws Midna on all 14 ticks before the body swap and on
its 5 (as her shadow on the back), none after it (MD_RETURN); turning wolf, only after the swap.
A's dummy draws her on each tick its pose has her, with no tick without her in between.

`midna-shadow`: A, human in South Faron (light world), calls Midna out; she stands beside Link as
her shadow. B checks A's dummy draws her that way for 60 ticks, then holds its camera near A's dummy
from three sides. A's forced cutscene then hides the dummy, Midna included. Captures: A's own Midna
(`MIDNA_HOLD a-local`), A's dummy on B (`b-puppet`, `b-puppet-side`, `b-puppet-far`) and the hidden
dummy (`b-hidden`).

Wolf attacks (`StepsWolfFx.cpp`):

| op | fields | does |
| --- | --- | --- |
| `wolfSpin` | `dir` (`right`, `left`), `trail`, `timeoutSec` | spin attack on our wolf |
| `wolfDome` | `force`, `radius`, `minLocks`, `timeoutSec` | Midna's lock-on dome (B held by an async walk before it) |
| `expectLocalWolfFx` | `spin`, `charge`, `dome`, `minRadius`, `lockBlur`, `timeoutSec` | what our capture sends |
| `expectPeerWolfFx` | `spin`, `dome`, `minRadius`, `lockBlur`, `lockDashSeq`, `hairAim`, `timeoutSec` | what the first peer last sent |
| `expectRemoteWolfFx` | `spin`, `lastSpin`, `minSpinTicks`, `maxSpinTicks`, `spinEmitters`, `dome`, `domeShown`, `minRadius`, `maxRadius`, `lockBlur`, `minLockDashes`, `maxLockDashes`, `hairAim`, `count`, `frames`, `timeoutSec` | what the peers' dummies show |
| `viewDummy` | `back`, `side`, `up` | our camera behind our Link, looking at the first peer's dummy |

Status effects (`StepsStatusFx.cpp`):

| op | fields | does |
| --- | --- | --- |
| `forceStatus` | `kind` (`freeze`, `burn`, `shieldBurn`, `douse`, `elec`, `hurt`, `chill`, `extinguish`), `value`, `frames`, `timeoutSec` | puts the status on our player the way the game does |
| `expectLocalStatus` | `frozen`, `iceBlock`, `elec`, `armorDrained`, `firePoints`, `shieldBurnMin`/`Max`, `shield`, `shieldInHand`, `damageTimerMin`, `sinkMin`/`Max`, `timeoutSec` | our capture and player |
| `expectDummyStatus` | `frozen`, `iceBlock`, `thaws`, `fireMin`/`Max`, `fireEmittersMin`/`Max`, `fireReceived`, `shieldBurnMin`/`Max`, `shieldBurnFx`, `shieldBurnOuts`, `shieldItem`, `elec`, `elecFx`, `damageTimerMin`/`Max`, `flashesMin`, `iceWait`, `sinkMin`/`Max`, `statusFlags`, `frames`, `timeoutSec` | the first peer's dummy |
| `statusFxPack` | `iceBlock`, `elec` | logs whether this stage's particle pack has these effects |

Items and projectiles (`StepsItemFx.cpp`). Kinds in `{kind: n}` fields are `arrow`, `boomerang`,
`bomb`, `spinner` and `crodBall`; event types `explode`, `hitMark`, `sound`, `water` and
`particle`:

| op | fields | does |
| --- | --- | --- |
| `spawnLocalItem` | `kind` (`arrow`, `bombArrow`, `seed`, `bomb`, `waterBomb`, `bombling`), `count`, `forward`, `up`, `pitch`, `aimAtDummy`, `atDummy`, `timeoutSec` | our real item actors, without the aim |
| `explodeAtDummy` | | a bomb arrow's explosion beside the first peer's dummy |
| `injectItemFx` | `slots`, `events`, `hook`, `ball`, `packets` | our next updates show these |
| `setOil` | `value` (21600) | lantern oil |
| `expectLocalItemFx` | `objects`, `events`, `hookOutTicks`, `ironBallTicks`, `levelSfxTicks`, `minHookDist`, `timeoutSec` | our capture's totals |
| `expectRemoteFishing` | `shown` (true), `action`, `minLine`, `maxReach` (600), `minCastTicks`, `timeoutSec` | the first peer's dummy draws its fishing rod so (tip in its hand, line that long) |
| `markRemoteItemFx` | | remembers the first peer dummy's counters |
| `expectRemoteItemFx` | `minDrawn` / `maxDrawn` (`{kind: n}` drawn now), `minSeen` (`{kind: n}` since the mark), `explosions`, `hitMarks`, `sounds`, `splashes`, `particles`, `maxExplosions` (since the mark), `tornado`, `boomCharge`, `minLights`, `maxLights`, `minEmitters`, `maxHeapUsed`, `hookChain`, `minTipDist`, `minHookShots` (chains since the mark), `minHookPeak` (the latest chain's longest tip distance), `minIronBallMode`, `maxIronBallMode`, `minIronBallDist`, `lanternFlame`, `minLanternGlow`, `maxLanternGlow`, `levelSfx` (level sounds started since the mark); `frames` (0), `timeoutSec` (20) | until the first peer's dummy shows every field given, then for `frames` more ticks |

Life, PvP and enemies (`StepsPvp.cpp`, `StepsEnemy.cpp`):

| op | fields | does |
| --- | --- | --- |
| `setLife` / `markLife` | `value` (12, quarter hearts), `hearts` (containers, 1 to 20) | sets / remembers our life |
| `expectLife` / `expectLifeDelta` | `value`, `min`, `max` / `delta`, `frames`, `timeoutSec` | our life (saved plus the meter's pending change) |
| `approachDummy` | `dist` (110) | our Link in front of the first peer's dummy, facing it |
| `sendPvpHit` | `target`, `kind` (`sword`), `damage` (2), `knockback` (`light`, `knockdown`), `dirY`, `blocked` | a real DAMAGE_PLAYER without our collision pass or cooldown |
| `expectPvpResult` | `target`, `result` (string or list), `reason`, `damage`, `timeoutSec` | the DAMAGE_RESULT of our last hit on `target` |
| `expectPvpStats` | `target`, `sent`, `applied`, `blocked`, `dropped`, `refused`, `damage`, `hitStops`, `timeoutSec` (0) | our counters for hits on `target` (only the fields given) |
| `expectPvpTaken` | `count` (applied + blocked), `blocked`, `dropped`, `damage`, `reason`, `timeoutSec` (0) | our counters for hits on us |
| `expectDummyHurtbox` | `registered` (true), `guard`, `timeoutSec` (0) | the first peer's dummy registered its PvP hurtbox this tick |
| `expectReaction` | `knockback` (`light`, `knockdown`, `none`), `timeoutSec` | our Link plays that reaction (`none`: neither, and no i-frames) |
| `expectHitStop` | `target`, `count` (1), `maxFrames` (3), `none`, `frames` (30), `timeoutSec` (5) | the scene's pause timer runs after `count` hit-stops of ours on `target`, at most `maxFrames` long (start the swing with an async `walk`); `none`: no pause for `frames` |
| `expectHitMarker` | `target`, `result` (`applied`, `blocked`), `text`, `damage`, `drawn`, `timeoutSec` (10) | the latest marker a DAMAGE_RESULT from `target` made (`drawn`: a frame drew markers) |
| `expectLockOn` | `locked` (true), `stance`, `frames` (30), `timeoutSec` (5) | Z-targeting (hold L with an async `walk`, buttons 0x40) is locked on the first peer's dummy (`stance`: and our Link is in its enemy lock, battle stance with the shield allowed); `locked: false`: neither for `frames` |
| `expectPvpToast` | `text`, `count` (1), `max`, `none`, `frames`, `timeoutSec` (10) | knockout toasts with that text ("A beat B", "A knocked out B") |
| `chargeDummy` | `dist` (600), `charge` (true), `trace` (false), `timeoutSec` (10) | our ridden Epona, `dist` from the first peer's dummy on open ground (its front first, then either side), faces it with the camera behind her, then gallops straight at it (spurred) until we send it a hit; `trace` logs each tick |
| `spawnEnemy` | `name`, `param`, `dx`, `dy`, `dz`, `tag`, `anchor` (`player`, `playerHome`), `onRoof`, `angleX`, `angleZ`, `setId`, `placed` | an actor in the current room's layer (`onRoof`: `dy` below the ceiling over that spot; `placed`: seen by Enemy Count as a stage placement) |
| `spawnEnemies` | `enemies` (a list of `spawnEnemy` fields), any `spawnEnemy` field as every entry's default | all of them in one update, in list order |
| `expectEnemyHealth` | `tag`, `health`, `max`, `tracked`, `shared`, `hitsLeft`, `helmetHealth`, `timeoutSec` | until the tagged actor runs with that health, max and scaling (`shared`: health as shared damage sees it; `hitsLeft`: a Chu Worm's counted hits left; `helmetHealth`: a Helmasaur's helmet, -1 without one) |
| `setEnemyHealthPercent` | `value`, `timeoutSec` | once we own the room: its enemy health percent |
| `expectEnemyHealthPercent` | `value`, `timeoutSec` | the percent we apply (the room's while joined, else 100) |
| `setEnemyCountPercent` / `expectEnemyCountPercent` | `value`, `timeoutSec` | the same for the enemy count percent |
| `expectEnemyCount` | `name`, `count`, `maxDist` (from our Link's home), `timeoutSec` | exactly that many running enemies of that actor name in the current room |
| `tagExtra` | `tag`, `of`, `dup` (1), `timeoutSec` | tags extra `dup` of the enemy tagged `of`, once it runs |
| `expectEnemyExtras` | `of`, `count`, `roofGap`, `timeoutSec` | exactly `count` extras of `of`, each with setID 0xFFFF, its defeated switch stripped and in the original's layer (`roofGap`: and as far below its ceiling as the original, within that many units) |
| `expectRoomExtras` | `count`, `timeoutSec` | exactly that many live extras in the current room |
| `expectBroodSize` / `tagBrood` | `of`, `count` / `tag`, `of`, `index`, `timeoutSec` | the enemy spawner tagged `of` makes `count` enemies / tags the one at `index` of its list, once it runs |
| `enemyDigest` / `expectPeerEnemyDigest` | `maxDist`, `flat` / `from`, `maxDist`, `flat`, `timeoutSec` | signals a hash of the room's enemies (name, params, home, extra index; `flat`: `maxDist` across the ground, whatever the height) / waits for `from`'s to match ours |
| `setEnemyHealth` / `deleteEnemy` | `tag`, `health` / `tag` | writes health directly (a bite, a get-up) / deletes the actor |
| `expectEnemyGone` / `expectEnemiesGone` | `tag` / `tags`, `maxSpreadTicks`, `timeoutSec` | no longer running (and not scaled) / all gone within that many ticks of each other |
| `killEnemy` | `tag`, `how` (`disappear`, `real`, `fall`, `hz`, `hzReal`), `size`, `type`, `onActor` | a puff and delete (with the zone actor bit after it), a Bokoblin's own death or void fall, or a Tile Worm's death wait after its puff / its own death roll |
| `expectEnemyDeadInPlace` | `tag`, `timeoutSec` | the tagged Tile Worm still runs, dead (its tile only) and out of the ENEMY group |
| `expectZoneActor` | `setId`, `room`, `set`, `timeoutSec` | the zone actor bit |
| `expectEnemySync` | `sent`, `received`, `applied`, `echoes`, `expired`, `timeoutSec` | the enemy-death sync counters given |
| `damageEnemy` | `tag`, `amount`, `helmet`, `hits` | lowers health as an attributed hit of ours (unlike `setEnemyHealth`), so it is shared (`helmet`: a Helmasaur's helmet's; `hits`: also a Chu Worm's counted hits) |
| `removeHelmet` | `tag` | the tagged Helmasaur's helmet off, as the clawshot pull leaves it |
| `sendEnemyDamageForTest` | `tag`, `dmg`, `pct`, `hpAfter` (-1), `hits` (0) | sends an ENEMY_DAMAGE for the tagged enemy with any sender percent (not counted as sent) |
| `expectEnemyDamage` | `sent`, `received`, `applied`, `floored`, `healed`, `dropped`, `timeoutSec` | the shared-damage counters given (hits, not packets) |
| `forceGroupFail` | `tags` | tagged Shadow Beasts into their downed wait, so the group is condemned |
| `dumpEnemies` | | logs the current room's enemies, their scaling and extra index |

Epona (`StepsHorse.cpp`):

| op | fields | does |
| --- | --- | --- |
| `spawnHorse` | `dist` (250), `side`, `timeoutSec` | our Epona shown in front of our Link (created when the stage has none) |
| `rideHorse` | `ride` (true), `mode` (`force`, `mount`, `beside`), `side` (`left`, `right`), `trace`, `timeoutSec` | on (forced, or the real mount: beside her, A on the GET_ON prompt) or off our horse; `beside` only stands there |
| `expectLocalHorse` | `present` (true), `ridden`, `timeoutSec` | our horse as shown and ridden |
| `setHorseHidden` | `hidden` (true) | our horse's call wait on or off |
| `expectRemoteHorse` | `count`, `name`, `ridden`, `parked`, `maxDist`, `maxSeatDist`, `zelda`, `maxZeldaDist` (150), `zeldaLook`, `zeldaNeck`/`zeldaBody` (`[x, y]`) with `zeldaAngleTol` (64), `zeldaBtp`, `zeldaBtk`, `zeldaMatchSender` (her eye clips are the latest sent), `zeldaEyeMove`, `zeldaTurnTol` (1.5), `card`, `tagLine2`, `onScreen`, `tint`, `vanilla`, `hueGain`, `frames`, `timeoutSec` | peers' puppets as the fields say, held for `frames` ticks; a shown Zelda's head and body joints must turn by what her look gives |
| `spawnHoZelda` | `timeoutSec` | Zelda behind our Link on our horse (daHoZelda_c) |
| `expectLocalZeldaLook` | `btp`, `btk`, `timeoutSec` | our capture sends our Zelda's look ("oy") with these eye clips |
| `expectRiderBowTilt` | `minDeg` (20), `maxDeg` (35), `minTicks` (5), `timeoutSec` | a peer's riding dummy turns joint 5 for its bow |
| `expectNoRemoteHorse` | `timeoutSec` | no puppet shown, no horse card drawn |
| `expectRidePrompt` | `target` (`own`, `remote`), `frames` (60), `dist` (110) | GET_ON on our horse / never on the puppet |
| `expectNotRiding` | `frames` (30) | our Link rides nothing |
| `clearEnemies` | `radius` (8000, 0 for the whole stage), `keep` | deletes the enemies near our Link and logs each; `keep: true` also deletes any that come within the radius later in the script |
| `horseCamera` | `target` (`remote`, `own`, `both`, `dummy`), `back`, `near`, `up`, `side`, `height`, `hold` / `release` | the camera behind our Link looking at the horse (or the peer's dummy); `near` puts it that far from the target on our side instead; `hold` keeps it (CameraService operator) |
| `measureRiderClimb` | `phase` (`mount`, `dismount`), `frames` (120), `maxStep` (45), `seatTol` (60), `maxDip` (40), `maxSeatDist` (60), `trace` | the hips of the first peer's dummy against its horse puppet for `frames` ticks: never inside her body, no jump over `maxStep` in a tick; for a mount they reach the saddle and do not sink more than `maxDip` after |

`horse-mount-smooth` (`scenarios/horse.js`) mounts with an A press: `procHorseRideInit` called from a
step runs after `execute`, so the mount's first root reset never happens and B's own Link sinks into
the horse and snaps up at the end. From behind, her body pushes Link out of the prompt's reach.
`horse-zelda-look` patches "oy" with turns and moved eyes; before that B's own Zelda already turns
her neck (2222, -5555), which A's copy follows. `horse-fmap` opens the field map once: a second
open after the B press that closed it stays at alpha 0.

Teleport (`StepsTeleport.cpp`):

| op | fields | does |
| --- | --- | --- |
| `teleportTo` | `target`, `expect` (`arrived`, `local`, `stage`, `refused`, `timeout`, `failed`, or a list), `reason`, `placement` (`exact` beside the peer, `fallback` on the spot it sent), `offset` (`[dx, dy, dz]` added to the peer's pose we aim at), `timeoutSec` | asks to teleport to that peer and waits for the result |
| `expectNear` | `target`, `maxDist` (250), `timeoutSec` | our Link within `maxDist` of the peer's reported position |
| `expectBeside` | `target`, `minDist` (40), `maxDist` (150), `maxAngleDeg` (20), `timeoutSec` | our Link that far from the peer's reported position (on the ground plane), facing its way |
| `expectOwnEvent` | `running` (false), `holdTicks` (60), `timeoutSec` | an event runs in our own game (or not) for `holdTicks` |
| `standInEventTag` | `mapToolId`, `margin` (300), `outside` | puts our Link on level ground inside that event tag's area of our room, `margin` from its edge (or, `outside`, on the nearest ground up to 1000 past that margin outside it) |

Written for teleporting beside the target, also into its cutscene, and the widget's clicks with the
menu closed, **not run yet** (the machine was in use): `teleport-cutscene` and `teleport-fallback`
(new), and new expectations in `teleport` (placement, `expectBeside`) and `players-widget` (a real
click with the menu closed).

Team games (`StepsTeamGame.cpp`). A team syncs only between members in game on its team game;
the randomizer seed a player runs is found among the generated seeds on disk:

| op | fields | does |
| --- | --- | --- |
| `setGameIdentity` | `identity` (`kind`, `key`, `name`, `mode`, `permalink`, `seed`, `version`, `inGame`; `{}` restores detection) | announce this game instead of the detected one |
| `fakeRandoResolver` | `items` (`{checkName: itemNo}`), `clear` | an ItemService check resolver answering those checks, as a loaded randomizer seed does |
| `expectLocalGame` | `kind`, `key`, `keyPrefix`, `name`, `verified`, `permalink`, `timeoutSec` | until the detected game matches every field given |
| `expectSyncState` | `state` (`ok`, `pending`, `unverified`, `mismatch`), `timeoutSec` | until the server reports that state for us |
| `expectMemberSync` | `peer`, `state`, `timeoutSec` | the same for a teammate |
| `expectTeamGame` | `team` (default ours), `key`, `keyPrefix`, `kind`, `name`, `permalink`, `noPermalink`, `noKey`, `sameGame`, `owner` (peer name or `self`), `timeoutSec` | until the team's state matches every field given |
| `expectCopyText` | `text`, `timeoutSec` | what Copy Seed Permalink would put on the clipboard (the clipboard is left alone) |
| `claimTeamGame` / `confirmUnverified` | | the Room tab's "Make My Game the Team's Game" / "Sync Unverified Match" |
| `promote` | `peer`, `role` (`room`, `team`) | the Players tab's "Make ... Room Owner" / "Make ... Team Leader", confirmed |
| `expectRoomOwner` | `owner` (peer name or `self`), `timeoutSec` | until the room state names that owner |
| `setTeamColor` / `expectTeamColor` | `rgb`; `team` (default ours), `rgb`, `timeoutSec` | the Room tab's Team Colour (leader only) / until the server reports that team colour |
| `expectTeleportBlocked` | `peer`, `code` (`other-team`, `other-game`, ..., null for none), `timeoutSec` | until teleporting to that peer is refused for that reason |

Story (`StepsStory.cpp`):

| op | fields | does |
| --- | --- | --- |
| `storySelfTest` | | classification, curated moves, segments, spawn form, entrances, STORY_MOVE round trip |
| `dumpStory` | | logs the story state and the room's map events, exits, PLYR points and event tags |
| `expectLayer` | `layer` or `natural` | our layer, or the one our flags pick here |
| `triggerStory` | `via`, `mapToolId`, `point`, `layer`, ... | starts a story event: `requester`, `walk` (a map event's tag), `arrival` (`room`, `point`, `layer`: a PLYR start event), `bossWarp` (a defeated boss's warp hole plays BOSS_WARPIN), `tearsFull` (`area`, `num`: the tears complete, a reload runs kytag04's warp), `actor` (`profile`: stand on the nearest one), `forcedMove` (`stage`, `room`, `point`, `layer`, `bit`: Midna requests a potential event and the stage loads during it; follow it with a story step), `npc` (`profile`, `index`: that NPC orders its event table entry) |
| `expectStoryMove` | `role` (`sent`, `received`, `none`, `notReceived`), `curated`, `qualHas`, `cached`, `toStage`, `toRoom`, `toPoint`, `sec`, `timeoutSec` | our sent / received moves |
| `expectPrompt` / `expectNoPrompt` | `kind` (`move`, `inconsistent`), `titleHas` / `sec` | that pop-up shows (with that text in its title) / none is offered for `sec` |
| `answerPrompt` | `answer` (`follow`, `decline`, `catchup`) | presses the pop-up's button |
| `catchUp` | `expectKind` (`entrance`, `teleport`, `none`), `toStage`, `start` (true) | the Players tab's Catch up to story, confirmed |
| `expectStoryLoad` | `state` (`idle`, `waiting`, `loading`, `arrived`, `failed`), `reason`, `layerArg`, `timeoutSec` | the follow / catch-up load |
| `expectSegment` | `id` (`none` for no segment), `state` (`consistent`, `behind`, `inconsistent`) | the active story segment and where we stand in it |
| `expectTransient` | `value` | the team's move ended on a cutscene or battle layer its originator is still on |
| `expectLearned` / `forgetLearned` | `key` or `curated`, `count` | learned story moves of this game |
| `entranceSweep` | `timeoutSec` | loads every segment entrance, curated follow point and learned destination; each must be a known point and spawn the predicted form |
| `testRemap` | `from` (`stage`, `room`, `point`), `to` (`stage`, `room`, `point`, `layer`); none clears | a stand-in for the randomizer's entrance shuffle on every stage request |
| `allowNpcEvent` / `perturbStoryDigest` | `profile`, `event` / `value` | allowlists an NPC event for the pull-in / makes our story digest differ |
| `collectTear` | `nth` | frees the nth live tear from its shadow insect and picks it up |
| `countTears` / `expectTears` | none / `delta` | the stage's live tears now / that count plus `delta` |
| `expectLightDrops` | `area`, `num` | our tear count |
| `keepTakenTears` | `value` | tears teammates took stay on screen (to race a pickup) |
| `saveFixture` | `name` | writes the save and our place to `<out>/fixtures/<name>.json` |
| `expectConsistent` | `value`, `segment`, `timeoutSec` | whether our position fits our story |
| `expectJoin` | `state` (or a list), `reason`, `holdSec`, `timeoutSec` | the pull-in state; with `holdSec` it must hold |
| `waitStorySettled` | `pressA`, `timeoutSec` | our relocation chain played out and nothing ran for 60 ticks; `pressA` advances the scenes' text |
| `forcePullInBlocker` | `code` (null: the real checks) | forces the pull-in safety check |
| `setTransformLevel` / `setDarkClear` / `expectTransformLevel` / `expectDarkClear` | `level`, `set` | level bits of our save |
| `dismissSaveRequest` | `optional`, `timeoutSec` | answers the capture's save prompt with no |
| `storyPrompts` | `value` | the per-player pop-up setting |

Cutscenes (`StepsCutscene.cpp`). With `soft` a failed check logs `[autotest] CUTSCENE FAIL <tag>
<why>` and the script goes on; a pass logs `CUTSCENE PASS`:

| op | fields | does |
| --- | --- | --- |
| `expectEventName` | `name`, `soft`, `tag`, `timeoutSec` (30) | our game runs the event of that name (the story tracker's map event name, else the event data's) |
| `expectCutsceneJoin` | `name`, `soft`, `tag`, `timeoutSec` (60) | our pull-in copy of a teammate's event of that name runs, ran or was shared (a join that had finished before the step does not count); fails at once when that join was missed (with its reason) |

The cutscene scenarios (`scenarios/cutscenes.js`) are generated from `data/cutscenes.json`, the
catalog `tools/cutscenes/extract.js` builds from the disc image and the decomp (read
`docs/cutscenes.md`). Each class a to c cutscene with a recipe gets `cut-<id>`: both players start on
its synthetic checkpoint (stage, room, a start point near the trigger, the layer when no flags pick
it, the milestone's event bits and levels), A triggers it (`triggerStory` `walk` into its tag,
`arrival` at its point, or `npc` with the NPC's table entry, allowlisted for the test), A checks the
event name, B must join it (`expectCutsceneJoin`), both settle and meet in one stage again; for a
boss scene (class b) B must stay out (`expectNoCutsceneJoin`). They are all manual and tagged
`cutscene`, `cutscene-a` / `-b` / `-c` and, where StoryEvent's rules say B should be pulled in,
`cutscene-likely`; the description says what today's code is expected to do.

For a batch, `cut-batch-story-<stage>-<n>` (class a), `cut-batch-boss-<stage>-<n>` (class b, B stays
out) and `cut-batch-npc-<stage>-<n>` (class c) run a stage's cutscenes in story order on one boot, setting the flags that grow between them before
each warp, with soft checks; rerun a `CUTSCENE FAIL` by its `cut-<id>` name. Rough lengths: all
batches about 9 hours (`--tag cutscene-batch-story` about 6, `--tag cutscene-batch-npc` about 3),
every isolated scenario about 13 hours (`--tag cutscene-likely` about 7). Regenerate the catalog after
a decomp or mod rule change: `node tools/cutscenes/extract.js`.

Colouring (`StepsRecolor.cpp`):

| op | fields | does |
| --- | --- | --- |
| `recolorSelfTest` | | the recolour maths |
| `expectDummyRecolor` | `set`, `rgb` or `peerColor` or `vanilla`, `name`, `hueTol`, `minSat`, `c1Max`, `c1Min`, `maxBytes`, `frames`, `timeoutSec` | the dummy wears `set` in that colour, only in the tunic-coloured blocks |
| `markRecolor` / `expectRecolorDelta` | `name`; `minApplies`, `maxApplies`, `minDraws`, `maxGpuGrowthMB`, `maxPrivateGrowthMB` | recolours, draws and process memory since the mark |
| `colorCycle` | `count` (200), `everyTicks` (8), `s`, `v` | changes our colour like the picker |
| `setRupees` | `value`, `timeoutSec` | in Magic Armor, done once its BRK follows |
| `recolorCamera` | `target` (`dummy`, `self`), `dist`, `height`, `up`, `side` | our camera close on the dummy, for captures |

Looks (`StepsLooks.cpp`), the host side of look sharing:

| op | fields | does |
| --- | --- | --- |
| `looksDump` | `hashes` | tt.looks.dump: logs ModsService's view, every active mod with its overrides (and with `hashes`, the SHA-256 of each winning one, read on the game thread) and the shared cosmetics values |
| `checkLooksInventory` | `fixtureA`, `fixtureB`, `manifestMarker`, `sharedPath`, `aOnlyPath`, `aOnlySize`, `aOnlySha`, `arcEntryPath`, `arcEntryArchive`, `textureBundlePath`, `textureHash`, `textureWidth`, `textureHeight`, `textureFormat`, `baseFile`, `aliasPath`, `discOverlayFile` (a disc file nothing loads in Link's house, default `/res/Object/E_fz.arc`), `mountHeapSize` (0x400000), `aliasEntryBytes`, `runtimePath`, `runtimeBytes`, `runtimeSha`, `cosmetics` (`strings`, `ints`: values to read back; `refused`: `{mod, name}` that must be `MOD_UNAVAILABLE`) | registers a runtime overlay, a base alias of `baseFile`, an overlay over `discOverlayFile` and runtime POINTER and SOURCE textures, then two frames later checks ModsService: both fixtures listed with the raw `mod.json`, exactly the later-loaded one winning `sharedPath`, sizes, hashes and reads, the archive entry and texture keys, our runtime entries, the alias reading the disc (through ModsService and through `DVDOpen`), `discOverlayFile` read from the disc under the overlay, `debug_texture_source` (pointer, content and no match), the var allowlist, the watch callback and the generation. Then it mounts the alias and `baseFile` the way puppets mount archives, checks that their first file reads the same, overlays that file under the alias's own directory and, a frame later, checks that the alias reads the overlay and `baseFile` does not |
| `overlayChurnAlloc` | `index` (an AlAnm file, default `BCK_WAITCANOEFISHL`), `cycles` (50), `maxLiveGrowth` (0) | overlays that AlAnm entry with its own bytes plus 32 padding bytes, then adds and removes another overlay `cycles` times (each a rebuild and a generation bump), reading the entry copy-style twice after each; fails unless every read comes from the overlay, the second read of a frame makes no new copy and the live entry overlay copies grow by at most `maxLiveGrowth` |

Written for the look sharing host pieces, **not run yet** (the machine was in use):
`looks-inventory` (with the cosmetics mod and both looks fixtures) and `overlay-churn-alloc`
(`scenarios/looks.js`). Both need a host with ModsService and OverlayService 1.1.

Look mods between players (`StepsLookShare.cpp`). Archives by name (`Kmdl`, `Mmdl`, `Zmdl`,
`Bmdl`, `Alink`, `HyShd`, `CWShd`, `SWShd`, `Wmdl`, `Midna`, `Horse`), look types `link`, `wolf`,
`midna`, `epona`, choices `theirs`, `default`, `mine`:

| op | fields | does |
| --- | --- | --- |
| `looksSelfTest` | | SHA-256 and XXH64 vectors, the allowlist, and a peer's look entries (a file of another type or path dropped, names lowercased, a bad sha drops all, a file over 8 MB leaves its type unshared) |
| `lookOption` | `share` (`off`, `team`, `room`), `show` (Show Other Players' Looks), `fallback` (`mine`, `default`), `modList` (Show My Mod List), `allowPacks`; `player` with `choice` (Their Look), with `type` and `choice` (also `same`: back to Their Look), or with `block` | |
| `looksArchiveOverride` | `arc`, `bmd`; or `remove` | overlays the archive's default path with the disc's archive (decompressed), one texel of `bmd`'s first plain texture changed: a whole-archive look of ours |
| `expectLocalLooks` | `types` (`{state, files: [[arc, name], ...]}`, the exact files), `mods` (`{types, shareable}` per mod id), `notMods`, `dirty` (exactly the archives our mods touch), `sent` (type to state in the last manifest we sent; `[]` for none), `frames`, `timeoutSec` | our scan with the current Share My Looks |
| `expectPeerLooks` | `name`, `none` (lists no looks), `types` (`{state, files: count, mods: [ids]}`), `installId` (a valid one), `modList` (these ids; `[]` for none), `tag` / `noTag` (in their row's look tag), `frames`, `timeoutSec` | what a player's manifest and client state say |
| `expectDummyArcPath` | `name`, `horse`, `base` (exactly these archives from `/res/Object/tt/base`, the rest from their default paths), `frames`, `timeoutSec` | where the puppet mounted its private archives |
| `looksSourceTexture` | `register: {arc, bmd}`; or `name`, `horse`, `source` (`pointer`, `source`, `none`), `frames`, `timeoutSec` | registers a content (SOURCE) replacement for the first plain texture of that model on the disc (failing unless a copy of the disc's texels renders from it); then checks what the renderer picks for every texture of that name on the puppet |
| `expectLooksFile` | `player`, `byId` (true: under their install id, else their name), `none`, `look`, `types` (exactly), `blocked` (how many files), `frames`, `timeoutSec` | data_dir/looks.json after a flush, and our install id in it |

Written for look detection and the game's look on puppets (M2), **not run yet**: `looks-detect`
and `looks-default` (`scenarios/looks.js`), with fixtures C and D (look entries under a root no
archive has, so nothing loads them; D's `mod.json` forbids sharing). Shared files are not fetched
yet: a puppet shows its player's look only when that look is the game's.

Cosmetics (`StepsCosmetics.cpp`), teammates' cosmetics on their puppets. Vars are the cosmetics
mod's names (`herosTunicTorsoColor`, ...); colours as `"rrggbb"`, Midna's hair as an index.

| op | fields | does |
| --- | --- | --- |
| `cosmeticsSelfTest` | | the recolour (grey identity, hue kept, CMPR modes, RGB5A3 alpha, I8 to RGB5A3), Midna's palette, and the Yaz0, RARC and BMD readers on built inputs, cut ones included |
| `cosmeticsOption` | `share` (Share My Looks: `off`, `team` or `room`), `show` (Show Other Players' Cosmetics), `player` with `choice` (that player's Their Look: `theirs`, `default` or `mine`) | |
| `expectCosmetics` | `name` (a peer: what its manifest shares) or `self` (what we last sent), `none`, `vars` (exactly these set), `version`, `frames`, `timeoutSec` | |
| `expectDummyCosmetics` | `name`, `horse` (its Epona's puppet), `keys` (`disc`: the disc's own textures were read; `unavailable`: no ModsService; `failed`; `pending`; with the last three nothing is recoloured), `allKeys` (every target's texture was found on the disc), `vars` / `textures` (by var or TEX1 name: `{state: recolor/block/none, rgb, hueTol (15), source: pointer/source/none, vanilla}` for every texture of it on the puppet), `noSource` (no texture renders from a content replacement: none of our own cosmetics or packs), `hair` (`[base, tips]` of Midna's palette), `frames`, `timeoutSec` | the puppet's cosmetics textures as registered, and what `debug_texture_source` says the renderer picks for each |
| `dumpCosmetics` | | logs every peer's shared values and its dummy's textures |

Written for cosmetics sharing (M1), **not run yet**: `cosmetics-share`, `cosmetics-color-rule`,
`cosmetics-defaults`, `cosmetics-leak`, `cosmetics-absent` and `cosmetics-off`
(`scenarios/cosmetics.js`). All but `cosmetics-absent` enable the cosmetics mod on both instances.
Sharing is Off by default, so senders start with `share_looks=2` (Everyone in Room); a receiver
registers blockers only while something of its own could land on a puppet, so most give it a
colour of its own.

Hook cost (`StepsPerf.cpp`):

| op | fields | does |
| --- | --- | --- |
| `measureHookCost` | `frames` (300), `maxMs` (0.1) | calls of our every-tick hook targets x one host dispatch, per tick |

UI: colour and window (`StepsColor.cpp`). The picker is the host's colour control, driven by key
presses posted to the game's own window (so it needs no focus) and checked through the setting:

| op | fields | does |
| --- | --- | --- |
| `colorMathSelfTest` | `chunk` | the colour maths over all 2^24 colours |
| `setColor` / `expectColor` | `rgb` (`"#RRGGBB"`) or `r`, `g`, `b`; `timeoutSec` | writes / waits for the colour setting |
| `expectColorHsv` | `h`, `hTol`, `hMin`, `hMax`, `s`, `v`, `tol`, `timeoutSec` | the setting's colour as HSV |
| `expectPeerColor` / `expectSelfRow` | `name`, `rgb`; `saveLoaded`, `stage` | a peer's / our own client entry |
| `expectDummyColor` | `name`, `rgb`, `timeoutSec` | the peer's dummy tints (and, with recolouring, its textures) in that colour |
| `resetColorPushes` / `expectColorPushes` | `min`, `max` | colour updates we sent since the reset |
| `showWindow` / `hideWindow` | `tab` (0 Connection, 1 Room, 2 Players, 3 Looks) | the Twili-Together window |
| `openColorPicker` / `closeColorPicker` | | the window's colour control pressed (the square has focus) / a cancel |
| `pickerKey` | `key` (`left`, `right`, `up`, `down`, `confirm`, `cancel`, `next`, `prev`), `count`, `holdMs` | taps, one per tick; `holdMs` holds one, repeating every tick after 0.32 s |
| `resizeWindow` | `width`, `height` | the game window's client size |
| `mark` | `msg` | logged as a warning, which flushes the log: a trigger for window captures |

Map cursors (`StepsMapCursor.cpp`). Targets are peer names or `#<client id>`:

| op | fields | does |
| --- | --- | --- |
| `mapCursorSelfTest` | | projection, facing, outline, pins, colours |
| `showMinimap` | `minAlpha`, `timeoutSec` | until the minimap draws on screen |
| `injectMapCursorClients` / `clearMapCursorClients` | `clients` (`id`, `color`, `dx`, `dz`, `dy`, `angle`), `unit` (`cm`, `texel`) | extra markers around our map position |
| `expectMapCursor` | `surface` (`minimap`, `dmap`, `fmap`), `target(s)`, `color(s)`, `pinned`, `floorDelta`, `facing`, `count`, `holdTicks`, `maxAgeTicks`, `timeoutSec` | a fresh frame with those markers in exactly those colours |
| `expectNoMapCursor` | `surface`, `target`, `reason`, `holdTicks` (30), `timeoutSec` | no marker, for that reason: a frame reason (`locationsOff`, `cutscene`, `notConnected`, `alphaZero`, `notInField`, `otherRegion`), a client reason (`noSave`, `noUpdate`, `otherStage`, `otherLayer`, `otherFloor`, `badPose`) or `absent` |
| `checkMapCursorTransform` | `target`, `tolPx`, `captured`, `scale` | anchors and facings against the offscreen pass's matrices; arrow and outline sizes against the picture's units per texel (`scale`, the HUD scale); `captured`: the rect came from the picture's own draw |
| `expectMapPaletteClean` | `groups`, `holdTicks` | the dungeon map palette is untouched |
| `dumpMapCursors` | `surface` | logs the frame and `MAPCURSOR_PROBE` screen fractions |
| `openPauseMap` | `surface` (`dmap`, `fmap`), `timeoutSec` | the dungeon or field map, as the map button opens it (a B press closes it) |
| `checkFmapLinkIcon` | `tolPx` (1) | on the field map our position, placed as remote players are, lands under the Link icon |
| `expectMapHorse` | `surface` (`minimap`, `fmap`), `target`, `color`, `ridden`, `ownerAway`, `maxFromMarkerPx`, `holdTicks`, `timeoutSec` | a remote Epona's horseshoe on the minimap or the field map (`maxFromMarkerPx`: that near its owner's marker and half its height across) |

Item toasts (`StepsItemToast.cpp`), checked through the toast history:

| op | fields | does |
| --- | --- | --- |
| `itemToastSelfTest` | `items` | icons and names from the game files |
| `itemNameAudit` | `items`, `rando` (our game's kind) | every toastable item this game hands out (the randomizer's list, or all but the unused ids) has a proper name: not unknown, "Reserved", a number or a lower-case noun |
| `itemToastOption` | `name` (`item_toasts`, `item_toasts_own`), `value` | |
| `clearItemToasts` | | forgets the history and what waits |
| `testItemToast` | `item`, `name`, `r`, `g`, `b`, `count`, `saveTbl` | a live toast as if a teammate picked the item up |
| `expectItemToast` / `expectNoItemToast` | `kind` (`item`, `own`, `summary`, `merged`, `taken`: a teammate's take removed ours), `from`, `item`, `textContains`, `minCount`, `fromPacket`, `hold`, `pushed`, `rendered`, `timeoutSec` / `forSec` | a matching toast (`rendered`: pushed with that item's `item://` icon) |
| `expectItemToastCount` / `expectItemToastItems` | the same fields and `count` or `min`/`max`; `from`, `count` | how many toasts / items counted |
| `dumpItemToasts` | | logs the history |

Name tags (`StepsNameTags.cpp`):

| op | fields | does |
| --- | --- | --- |
| `expectNameTag` | `name`, `shown`, `gate`, `line2`, `rgb`, `health`, `life`, `maxLife`, `maxWidth`, `holdTicks`, `timeoutSec` | the peer's tag in the last presented frame (or none, for `gate`); `rgb` is its frame colour; `life` / `maxLife` its health bar in quarter hearts (`health: false`: no bar); `maxWidth` the tag's width |
| `viewPeer` | `name`, `yaw`, `beyond` | moves our player past the peer's dummy, seen from our camera, so the peer shows `yaw` degrees off centre |
| `showPlayerHealth` | `value` (true) | the Show Player Health option |

Written for the PvP battle stance, player health on name tags and HUD-scaled map markers, **not
run yet** (the machine was in use): `pvp-lockon` (now with `stance`), `name-tag-health`
(`scenarios/nametags.js`) and `map-cursor-hudscale` (`scenarios/mapcursor.js`). Every scenario
with `checkMapCursorTransform` now also checks the marker and outline sizes.

Players widget and the Players tab rows (`StepsPlayersWidget.cpp`). Mouse steps push SDL events
at window coordinates (an 800x480 window at UI scale 1), so a real pointer over the test window
disturbs them:

| op | fields | does |
| --- | --- | --- |
| `expectPlayersWidget` | `shown`, `gate` (`off`, `notConnected`, `noSave`, `empty`, `cutscene`, `menu`), `header`, `count`, `rows` (`name`; `label`, the name as shown; `area`, `tooltip` substrings; `teleport`: `ready`, `blocked`, `none`), `absent` (names), `x`, `y`, `minX`, `minY` (per mille), `opacity`, `scale` (percent), `clickableInGame` (the host takes its clicks with the menu closed), `holdTicks`, `timeoutSec` | the widget as the last tick built it |
| `expectPlayersTab` | `count`, `rows` (`name`, `teleport`, `tooltip`, `actions`, `text` in the row's RML), `absent`, `holdTicks`, `timeoutSec` | the Players tab's rows; the tab must be shown (`showWindow` tab 2) |
| `clickPlayersWidget` | `target`, `at` (`[x, y]`), `expect`, `reason`, `timeoutSec` | presses the target's teleport button (the host's click callback, or with `at` a real click there) and waits for the teleport's result |
| `movePlayersWidget` | `x`, `y` (0 to 1) | moves the widget there as a drag would |
| `mouseDrag` / `mouseMove` | `from`, `to`, `frames` / `x`, `y` | a left-button drag / a pointer move |
| `measurePlayersWidget` | `grab` (a point on the header with the widget in the top left corner), `by` (`[dx, dy]`), `area` (the overlay's free space; default the window size less 24, the overlay's insets at UI scale 1), `min` (`[w, h]`), `as`, `of`, `ratio` (`[min, max]`) | drags the widget out of the corner by `by` (menu open) and works out its size in dp from where the host left it, dragging again until two drags agree; fails below `min`, remembers it `as` a name, or checks both sides against the size measured `of` a name, then puts the widget back |
| `playersWidgetOption` | `name` (`players_widget`, `players_widget_opacity`, `players_widget_scale`), `value` | |

## Adding a scenario

Core scenarios are in `scenarios.js`; feature scenarios in `scenarios/<feature>.js`, each
exporting an array. A scenario has:

- `name`, `description`, `timeoutSec`;
- `cvars`: extra `--cvar` overrides for every instance;
- `serverEnv`: environment for the relay server (network scenarios only);
- `transport`: `"tcp"` connects over `tcp://` to the relay's TCP port instead of WebSocket;
- `restartServer`: `{afterSignal, downMs}` kills the relay once it logs that AUTOTEST_SIGNAL and
  starts it again on the same ports `downMs` later (default 3000); the scenario fails if it never
  happened;
- `server`: force (`true`) or suppress (`false`) the relay server; by default it runs when a step
  uses a network op;
- `enableMods`: ids of other mods next to the exe, or of fixture mods in `mods/autotest/`, to keep
  enabled (`MODS` in `lib.js`); an instance's own `enableMods` add to it for that instance;
- `files`: `{path: text}` written into every instance's user dir before launch (an instance's own
  `files` too), e.g. randomizer seeds under `mod_data/`;
- `sharedUserDir`: every instance runs on `slot0` (one user's config, saves and mod cache), each
  with its own log dir `slot0/logs/<instance>`;
- `expectLog` / `rejectLog`: regexes (or `{pattern, instance}`) every instance log must / must not
  match. Every scenario also rejects a HookService "was inlined into callers" warning for our mod
  (the hook would miss the inlined calls) and a failure of our mod;
- `instances`: one object per instance with `name`, optional `start`, optional `cvars`, optional
  `launchDelayMs` (delay before the *next* instance is launched), and `steps`. `script: false` with
  `runSec` runs the game without a script for that long and judges only its log.

The helpers in `lib.js`: `meetIn(stage, other)` waits for both players in a stage, checks each
other's dummy and barriers; `barrier(name, other)` synchronises two instances; `tt(name, value)`
builds a mod-setting `--cvar`. `STORY` holds the story checkpoints (start fields for one area each)
and `STORY_BITS` their event bits; `scenarios/story-late.js` builds on them, and its manual
`story-observe-*` scenarios play one beat and log what the story tracker saw.
