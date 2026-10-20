# Twili-Together protocol

Protocol version 5. Clients never talk to each other directly: every packet goes to the relay
(`tools/server/server.js`), which decides who gets it. The relay keeps connection metadata and a
few caches but never simulates the game.

## Transport

Every message is one JSON object with a `type` string.

| URL | Transport | Framing |
| --- | --- | --- |
| `wss://host[:port][/path]` | WebSocket over TLS | one JSON text message per packet |
| `ws://localhost:port` | plain WebSocket | same; the game allows plain `ws://` only to localhost |
| `tcp://host:port` | plain TCP | 4-byte little-endian length N (1 to 1048576), then N bytes of UTF-8 JSON |

WebSocket clients may offer the subprotocol `twili-together.5`. The relay pings WebSocket
connections every 10 s. TCP clients send `{"type":"KEEPALIVE"}` every 5 s, the relay answers it,
and either side drops a TCP peer that stays silent for 30 s. KEEPALIVE is never relayed.

The client polls the connection from the game thread once per tick. After an unrequested drop it
reconnects on its own (1, 2, 4, 8, 15, then every 30 s) with the same session key, so the relay
treats it as the same player coming back. A first connect that fails is not retried.

## Joining

The client's first packet is the handshake:

```json
{
  "type": "HANDSHAKE",
  "app": "twili-together",
  "protocolVersion": 5,
  "modVersion": "0.5.0",
  "layout": "1f0c9a2b44e7d310",
  "transport": "ws",
  "name": "Ori",
  "teamId": "red",
  "roomId": "friday",
  "sessionKey": "k3j2...",
  "color": {"r": 255, "g": 64, "b": 64},
  "game": {"inGame": true, "kind": "vanilla", "key": "vanilla", "display": {"name": "", "mode": ""}},
  "features": ["cos.send", "cos.recv"],
  "gameId": "GZ2E01"
}
```

- `layout` is a hash of the save structures (16 hex digits). World data only flows between
  clients with the same layout, since the world state is sent as raw save bytes.
- `sessionKey` is random per game process. It lets a reconnect replace the old connection and
  keeps the relay from replaying a player's own packets back to them.
- `game` says what the player runs; see [Team games](#team-games).
- `features` (optional) says what the client can do with [looks](#looks): `cos.send` (it reads
  its cosmetics settings, which needs the host's ModsService), `cos.recv` (it shows other
  players' cosmetics, which needs TextureService), and later `looks.send` and `looks.recv`.
  Unknown names are dropped. `gameId` is the disc's game id (4 to 8 of `A-Z0-9`), empty when the
  client cannot tell.

A handshake with another `app` or a `protocolVersion` below 5 gets `SERVER_MESSAGE` and
`DISABLE_CLIENT` (both with a `message`) and the connection is closed. Packets sent before the
handshake are ignored, and so is a second handshake.

On success the relay answers with `ALL_CLIENT_STATE`:

```json
{"type": "ALL_CLIENT_STATE", "clients": [ ...client states... ], "roomState": { ... },
 "relayFeatures": ["cos.send", "cos.recv"]}
```

then one `TEAM_STATE` per team in the room, then the newest `LOOK_MANIFEST` of every player the
joiner may see. Everyone else in the room gets an `UPDATE_CLIENT_STATE` with the new player.
`relayFeatures` lists the [looks](#looks) features this relay forwards; an older relay sends none,
and clients then share nothing.

A client state holds `clientId`, `name`, `teamId`, `color` (the player's own), `displayColor`
(what everyone shows them in: the team colour on a named team, otherwise their own), `modVersion`,
`layout`, `protocolVersion`, `online`, `isSaveLoaded`, `stageName`, `layerNo`, `roomNo`,
`saveTblNo`, `horseName`, `horsePlace` and `self` (true in your own entry).

## Rooms and owners

Players with the same `roomId` share a room (empty is the public room). The first player in a room
owns it. When the owner leaves, the player who has been connected longest takes over. The owner
can hand the room over with `SET_ROOM_OWNER {targetClientId}`.

`UPDATE_ROOM_STATE {state}` is accepted only from the owner and goes to the whole room. Unknown
keys and wrong types are ignored, numbers are clamped. The state:

| Key | Default | Meaning |
| --- | --- | --- |
| `syncWorldState` | true | teams share flags, items and save progress |
| `shareWoodenShield` | true | wooden shields sync like other items |
| `sharePickups` | true | with `syncWorldState`: teammates' consumable pickups are added for everyone (`SHARE_PICKUP`) |
| `syncNPCs` | false | enemy deaths sync within a stage |
| `syncEnemyDamage` | true | with `syncNPCs`: teammates' hits on an enemy weaken our copy too |
| `cutsceneSync` | true | story cutscenes pull in teammates in the same room |
| `syncPushPull` | true | with `syncWorldState`: teammates' block pushes and chain pulls play for everyone in the room (`PUSH_MOVE`, `PUSH_REST`, `CHAIN_PULL`) |
| `pvpMode`, `pvpFriendlyFire`, `pvpLethal` | false | PvP rules |
| `showLocationsMode` | true | map markers |
| `teleportMode` | false | teleport to a player |
| `teleportAcrossTeams` | false | teleport to other teams' players |
| `enemyHealthMultiplier` | 100 | percent, 100 to 500 |
| `enemyCountMultiplier` | 100 | percent, 100 to 300: extra copies of stage-placed regular enemies (at most 24 per room, picked in spawn key order) and more children from enemy spawners, made alike by every client when an area loads |

The relay adds `ownerClientId`. An empty room keeps its settings and caches for 6 hours. Older
clients still send `hidePlayersInCutscene`, now a personal setting: the relay drops it like any
unknown key.

## Team games

Players on the same `teamId` are a team. Each team has a leader (the longest-connected member, or
whoever the leader promoted with `SET_TEAM_OWNER {targetClientId}`) and a team game: what the
team plays.

A client's game identity is `{inGame, kind, key, display: {name, mode}, share, allowUnverified}`,
sent in the handshake and again with `GAME_IDENTITY` whenever it changes. `key` is one of:

- `vanilla`
- `rando/<format>/<digest>`: a randomizer seed the client found among its generated seeds and
  matched against the running randomizer. `share` then carries `{permalink, seed, version}` from
  the seed's anti-spoiler log.
- `rando-probe/<fingerprint>`: a randomizer game the client could only identify by probing a few
  item checks. It syncs only after the player confirms it (`allowUnverified`).
- `mode/<game mode id>` for other game modes.

The first member in game sets the team game. After that only the leader changes it: if teammates
still play the old game, the leader gets `TEAM_GAME_CONFLICT` and must answer with
`CLAIM_TEAM_GAME` to switch.

`TEAM_STATE {teamId, ownerClientId, game, color, members: [{clientId, sync, ...}]}` goes to the
room on every change. A member's `sync` is `ok` (in game on the team game), `unverified`,
`pending` (not in game yet) or `mismatch`. Only members of that team get the game keys and the
permalink. Team traffic (everything under World sync, story and kills) only flows between members
whose state is `ok`.

A named team also has a colour, a default derived from its id until the leader sends
`SET_TEAM_COLOR {color}`. Members show in it (`displayColor`).

## Packets by routing

The relay stamps every relayed packet with the sender's `clientId`.

### Room-wide

| Packet | Sent by | Notes |
| --- | --- | --- |
| `UPDATE_CLIENT_STATE` | client | stage, layer, room, save table and whether a save is loaded; or only `color`; or only `horseName` / `horsePlace`. The relay stores it and forwards its own view |
| `UPDATE_ROOM_STATE` | owner | see above |
| `AUTOTEST_SIGNAL` | client | test barrier between instances |
| `TEAR_COUNT` | client | `teamId`, `area` (0 Faron, 1 Eldin, 2 Lanayru; -1 none), `count`: the sender's own tears of light while an area's vessel is held and not full, sent on a change and to a teammate who comes online; teammates show it in the Players tab |

### Presence (same room, stage, layer and protocol)

| Packet | Notes |
| --- | --- |
| `PLAYER_UPDATE` | the player's pose, once per game tick while someone is there to see it |
| `PLAYER_SFX` | `{soundId, kind, mapInfo, sq}`: a Link (kinds 0-6) or Midna (7 voice, 8 sound) sound to play on the puppet when it shows pose tick `sq` |

A `PLAYER_UPDATE` carries `seq` (the sender's tick), `ep` (an epoch that changes on a scene
change), `fl` (presence flags: wolf, in cutscene, shield up, ...) and short keys with integer
arrays. A keyframe (`k: 1`, at least every 30 ticks and whenever the audience changes) has every
field; the packets in between only carry what changed. Receivers buffer about 2 s of samples and
play them back a little behind the newest one, so network jitter does not show. The main groups:

| Keys | What |
| --- | --- |
| `p`, `a`, `sa`, `ba`, `tw` | position, angles, body angle, twist |
| `la`, `ua`, `lf`, `uf`, `lr`, `ur` | lower and upper body animations: clips, frames, blend ratios |
| `eq`, `hi`, `ij`, `ib`, `pr` | clothes, sword, shield and item in hand, hand shapes, item clip, loaded ammo |
| `md`, `mf`, `ma`, `me`, `mw` | Midna on the wolf's back, her eyes, and her place when she is off it |
| `tf` | a running transformation |
| `sx`, `sf` | status effects: frozen, burning, electrocuted, shield burning |
| `wx` | wolf attacks: spin, Midna's dome, lock-on jumps |
| `x0`..`x7`, `xe`, `hk`, `bc`, `ls`, `hx`, `fr` | items in the world (arrows, bombs, boomerang, ...), one-shot events, clawshot, ball and chain, level sounds, bottle, fishing rod |
| `ox`, `op`, `os`, `oa`, `of`, `ow`, `ok`, `or`, `oz` | the player's Epona, and Zelda behind its rider |
| `oy` | where that Zelda looks: `[1 \| eyeFlags << 8, neckX, neckY, bodyX, bodyY, (btp + 1) \| (btk + 1) << 16, btpFrame, btkFrame x16, 4 eye offsets x1024]`, her HoZelda.arc eye clips and `daHoZelda_matAnm_c`'s eye move flag (1) and morph frames (bits 1-3); optional (absent: her eyes and turns stay as her clips have them) |
| `hp` | `[life, maxLife]` in quarter hearts, for the name tag's health bar; optional (a keyframe without it means unknown) |
| `gi` | the item held up (item get, trade, item that cannot be used there): `[shownItem \| givenItem << 8 \| flags << 16, yaw, scale x1024, timer, x, y, z x8]`, yaw and place relative to the player's facing and position; flag 8 alone means it exists but is not shown yet (receivers load its model); optional (absent in a keyframe means none) |
| `yo` | the body's height and feet: `[flags, stepOffset, extraOffset, rootBaseY]`, offsets x8. Flags: 1 feet on the ground, 2 idle (the body may drop to the lower foot), 4 the root joint's height is `rootBaseY` (jumps, ladders, climbing). `stepOffset` is the body's easing over steps (and the swim bob), `extraOffset` the rest of its lift over the position (stairs, a board). Receivers adjust the legs to their own ground; optional (absent in a keyframe means all zero, and the legs stay as animated) |
| `ym` | the body matrix beyond position and facing: `[flags \| tweak << 4, rootX, rootZ x8, tweakA, tweakB, magnetTilt, magnetDir, climbYaw]`. Flags: 1 sent (always), 2 and 4 the root joint's X and Z are `rootX` and `rootZ` (otherwise the clip's: ladders, climbing), 8 magnetic boots (`YrotS(dir) XrotM(tilt) YrotM(-dir)` before the facing). Tweaks: 1 boar run, 2 horse run (turns about a pivot by `tweakA`, `tweakB`), 3 wolf chain, 4 magnetic boots flight (`tweakA` about Y). `climbYaw` turns the body on a climbing wall; optional (absent: receivers pin the root to the standing pose) |

Presence is best effort: when the send queue is full, deltas are dropped and the next keyframe
repairs the stream.

### Looks

How a player looks to the others: for now, the colours of the cosmetics mod
(`dev.twilitrealm.cosmetics`), which teammates' games show on that player's puppets with their
own recolour.

```json
{"type": "LOOK_MANIFEST", "quiet": true, "rev": 4, "scope": "room", "looks": [],
 "cosmetics": {"ver": "1.0.0", "vars": {"herosTunicTorsoColor": "1e88e5", "midnaHairBaseColor": 3}}}
```

- `cosmetics` is null when the player shares none: the mod is off or missing, every value is at
  its default, or the player turned Share My Cosmetics off. `ver` is the cosmetics mod's version.
  `vars` holds only values that differ from the default: the 22 colours as `rrggbb` and
  `midnaHairBaseColor` / `midnaHairTipsColor` as a palette index 1 to 9. The names are the cosmetics
  mod's settings, the same list Dusklight's ModsService hands out.
- `scope` is `room` (everyone in the room) or `team` (teammates only).
- `looks` stays empty until look mods are shared.

The relay rebuilds the packet from these fields (unknown vars and values are dropped, colours
lowercased), stamps `clientId` and `gameId` (from the sender's handshake), and drops one over 16
KiB. It sends it to the others in scope that listed `cos.recv` (`cosmetics` stays null for a
sender without `cos.send`). It keeps each player's newest manifest while they are connected and
gives it to everyone who joins later. A player's manifests go out at most once every 2 s: one sent
sooner waits, a newer one replaces it, and the newest always goes out. Clients send one when they
join and whenever what they share changes, also at most once every 2 s.

### World sync (same room, team, team game and save layout)

| Packet | Fields | Notes |
| --- | --- | --- |
| `SET_FLAG` / `UNSET_FLAG` | `category`, `flagNo`, `roomNo`, `stageName`, `saveTblNo` | switches, chests, item flags. A new item flag removes the receiver's copy of that freestanding item (field item, heart piece or container, Ordon Sword) if it lies in a loaded room, with no item and no event; one the receiver is taking stays, one being pulled goes once let go, and a pickup of it gives nothing. A new switch fades the receiver's Poe with that soul switch unless it is dying for the receiver |
| `SET_EVENT_BIT` / `UNSET_EVENT_BIT` | `no`, `stageName`, `roomNo` | story event bits; a few local-only bits are never sent. `stageName` and `roomNo`: where the sender stood. A new event bit removes a golden wolf whose skill it is and the randomizer's golden wolf item with that flag |
| `GIVE_ITEM` | `itemNo`, optional `check`, `saveTblNo` | the receiver grants it through the game's item service, so a randomizer applies its own logic. Skipped when the receiver has it, except items that come more than once (heart pieces and containers, bottles, bomb bags, Poe souls, the randomizer's dungeon small keys and Sky Characters). A bottle found filled arrives empty. Randomizer items in unused ids (portals, dungeon keys, maps, compasses) travel here too; its trap does not. `saveTblNo`: the sender's stage table. `check` names the one thing in the world it came from: `freestanding:<stage>:<bit>` or `boss:<stage>` (bits below 0xA0), `fishing_heart_piece` (the same as the fishing hole's freestanding bit 0x80), `goron_springwater_rush` (the same as Hyrule Field's freestanding bit 0x80), `golden_wolf:<event bit>`, `poe:<stage>:<switch>` (not 255), `chest:<stage>:<box>`, or an NPC's heart piece (`goats_reward`, `archery_reward:F_SP109`, `prayer_reward`, `plumm_minigame_reward`, `snowboard_race_reward`, taken once its event bit is set). The receiver removes its copy (a Poe fades, a hooked heart piece leaves the lure), places no heart piece or container of that check from then on, and skips a check it took itself (its pickup, kill or chest open under way, or the flag set but not by a teammate) or already got from a teammate. Any other named check (no shop, no unparsed placed name) comes along for items that come more than once and reaches each player once in a session. A heart piece or container a player's own game gives for a check a teammate's grant already brought is taken back (its max life) |
| `UPDATE_DUNGEON_ITEMS` | `saveTblNo`, `keyDelta`, `dungeonItemBits` | small keys, map, compass, boss key, and the boss bits (boss and mini-boss defeated, heart container taken) |
| `COLLECT_REWARD` | `crystal`, `mirror` | Fused Shadow and Mirror Shard bits the sender just got (`mCrystal`, `mMirror`): the boss warp's Midna scene sets them with no item get. The receiver sets them and shows "<name> got Fused Shadow" or "Mirror Shard"; a later clear of its own sets them again, nothing doubles. Not sent in randomizer games, where the reward is a check and travels as `GIVE_ITEM` |
| `LIGHT_DROP` | `area`, `tbl`, `tbox` | a tear of light picked up. Unless that tear's TBOX bit is already set, the receiver sets it, adds one to the area's count (at most 16) and removes the tear from its screen; the bit keeps replays from counting twice |
| `SHARE_PICKUP` | `items` (item numbers, at most 64) | consumables the sender just collected (rupees, hearts, fairies, arrows, seeds, bombs; no purchases, no bottled fairy, no tear) and, in a randomizer game, its trap item `0x13`. The receiver adds the same gain within its own limits (wallet, hearts, quiver or slingshot owned, bomb bags of that kind or empty ones; no heal while knocked out) with `sharePickups` on, and passes a trap to its own randomizer. Never queued or cached; receivers ignore replays |
| `TAKE_ITEM` | `itemNo`, and `slot`, `firstBit: true` or `equip: "sword"` | the game took this item from the sender: the lantern (slot 1, the monkey), Auru's memo or Ashei's sketch (slot 19), Renado's letter, the invoice, the wooden statue or Ilia's charm (slot 21), the Sky Book (slot 22), the pumpkin or cheese (`firstBit`, no slot), or the worn Wooden Sword `0x3F` that Talo takes (`equip`: unequipped, still collected; the collection screen hides it once `F_0026` is set, and the game never gives it back). Never sent in a randomizer game and never for a shield; consumables are not takes. The receiver removes its copy the way the game does once that is safe (no menu, cutscene or loading, not held in hand, the sword not drawn; a lantern lit on the belt goes out), with a toast, and grants the item again on a later `GIVE_ITEM` of it from a teammate, into its empty slot or sword. An item that comes back to the sender without an item get (a game over during the monkey's theft, an event's trade swap) goes out as `GIVE_ITEM` |
| `UPDATE_WORLD_STATE` | `fmt` (`dsv1`), `save`, `dan`, `danStageNo`, `saveTblNo`, `seenEpoch`, `seenSeq`, optional `targetClientId` | the whole save as base64; merged, never simply copied. Outside randomizer games an empty slot is not refilled with an item the receiver's first-get bit says it had (it was taken), except the lantern once the monkey's theft is over (event bits `F_0224`/`F_0225` without `F_0226`), and a takeable item the sender's save shows taken (slot empty, first-get bit set; the lantern only during the theft) is taken from the receiver too. Once `F_0026` is set (or the receiver took the sword) the Wooden Sword is never put back on, and a receiver still wearing it while the sender's save does not (and had it) loses it. `seenEpoch`/`seenSeq`: the last queued packet this save has applied |
| `REQUEST_WORLD_STATE` | `catchUp` | asks the team for its state |

Each of these carries `teamId`, `layout`, `protocolVersion` and `addToQueue`. The relay adds
`senderSessionKey`, `senderName` and `senderColor`.

Boss fights: while the receiver fights its own boss or mini-boss (from the first tick it stands in the
room of a live one until it stands, settled, outside that arena; a game over that reloads the arena
keeps it), it holds what a teammate's kill there would interrupt: switch `SET_FLAG`s and `UNSET_FLAG`s
and the event bits whose `stageName` (and, for arenas that are one room of a larger stage, `roomNo`) are
the arena's, and the boss and mini-boss bits of `UPDATE_DUNGEON_ITEMS` for the arena's stage table. Keys,
map, compass, boss key, item flags, chests, `GIVE_ITEM` and the heart container's `STAGE_LIFE` go on: with
that bit set our own kill spawns no second heart container. A merge meanwhile leaves out the held bits
and the arena's boss bits. All of it applies, in order, when the fight ends; nothing is dropped.

Catch-up: the relay keeps, per team, team game and layout, the last untargeted
`UPDATE_WORLD_STATE` and every `addToQueue` packet since (numbered with `queueEpoch` and
`queueSeq`, so a reconnecting client skips what it already applied). A joining client sends
`REQUEST_WORLD_STATE`. Teammates who are online and caught up answer with a targeted
`UPDATE_WORLD_STATE`; the cache answers only when none can. With `catchUp: true` the relay also
replays the queue and the team's last 16 story moves, oldest first. A fresh untargeted world state
replaces the cache and drops from the queue what it holds: its sender's own packets and the
others' up to `seenSeq`. The others' later packets stay queued (all of them when `seenEpoch` is not
this queue's epoch); a state without `seenSeq` empties the queue. A `TAKE_ITEM` with `firstBit`
stays queued past every state: a save cannot tell a taken pumpkin from one never had.

#### Heart pieces

The 45 heart pieces of the unmodded game (from `tools/cutscenes/heart-pieces.js`, which lists the
disc's placements, the decomp for the givers, and the randomizer's location list). Stage tables in
hex. Every one of them, and every randomizer check at these places, reaches each teammate once:

| Heart pieces | Where | Taken when set | Appears |
| --- | --- | --- | --- |
| 17 overworld chests | F_SP108 r14 26 (lantern switch 0x5C), r5 30; F_SP117 r2 1; F_SP109 r0 21; F_SP110 r0 22; F_SP115 r0 0, r1 29 (switch 0x14); F_SP121 r0 2 and 3, r3 5, r11 8; D_SB00 r0 0; D_SB02 r0 62 (0x5F); D_SB03 r0 3 (0x2D); D_SB04 r10 62; D_SB06 r1 2 and 3 (0x02, 0x03) | TBOX bit | placed; a lantern or grotto chest after its switch |
| 16 dungeon chests | D_MN05 r3 31, r5 19; D_MN04 r4 20, r6 9; D_MN01 r3 5, r8 8; D_MN10 r2 19, r13 26; D_MN11 r0 21, r7 25; D_MN06 r3 5 (zone switch 0xE2), r4 12; D_MN07 r11 6, r12 9; D_MN08 r1 30, r4 0 | TBOX bit | placed or after a switch |
| Faron Field tree, Kakariko Gorge spire | F_SP121 r6, r3 (`htPiece`) | memory item bit 0x81, 0x82 (table 6) | placed |
| Kakariko bomb rock spire | F_SP109 r0 layers 2-4 | 0x8C (table 3) | placed |
| Cats' hide and seek | F_SP128 r0 layers 1-2 | 0x8B (table 3) | placed |
| Fishing hole | F_SP127 r0 | 0x80 (table 0xB) | placed; also the rod's catch (`fishing_heart_piece`) |
| Bulblin Camp roasted boar | F_SP118 r1 `Obj_rw` | 0x9F (table 0xA), check `boss:F_SP118` | dropped when the boar breaks, or at load once its switch 0x01 is set |
| Goron springwater rush | F_SP121 r0 `grA` | 0x80 (table 6), check `goron_springwater_rush` | dropped by the Goron once `M_070` and `F_0377` are set |
| Fado, Talo, Charlo, Plumm, Yeta | F_SP00, F_SP109, F_SP116, F_SP115, F_SP114 | event bit `F_0539`, `M_042`, `F_0294`, `F_286`, `F_0483` | the NPC's reward event; the giver checks its bit |

Before this was fixed: chests and the placed heart pieces were covered (bit synced and merged, gone
on load, the lid opens or the piece vanishes live, a second take refused, two at once each got one;
the grant goes out when the item message closes, with the bit). The springwater heart piece's grant
carried no check, so two takes of it counted twice. The NPC rewards gave a second piece to a
teammate who won the same reward before the bit arrived (offline or at the same time), and a
restart could replay the grant. Now the NPC rewards and the springwater rush are checks, a second
piece of one is taken back, and a heart piece or container whose check reached the receiver is
never placed even where its flag did not (`daObjLife_Create` fails, Checks hooks).

Boss heart containers (`UTAWA_HEART`, bit 0x9F of the dungeon's table and its `STAGE_LIFE`; Zant's is
placed in D_MN08A r10, bit 0x80): the teammate's grant, bit and `STAGE_LIFE` reach a receiver in its
own fight, elsewhere (into that table's save slot) or offline (merged), so its own kill drops none
(the bosses check `STAGE_LIFE`, the container its bit). A grant taken in the receiver's own table keeps
the `STAGE_LIFE` its item function sets. In a randomizer game the container is the check's item and
ignores `STAGE_LIFE`; its bit or, without it, the check stops it.

### Kills and story (teammates only)

| Packet | Goes to | Notes |
| --- | --- | --- |
| `ENEMY_DEFEATED` | teammates in the same stage and layer | `{v: 2, stageName, layerNo, kills: [...]}`; a kill is the enemy's spawn key `{roomNo, procName, params, setId, home, dup}` (`dup` 0, or 1 to 4 for an Enemy Count extra; an enemy spawner's child has its spawner's `home` and `setId` 0xC000 plus its place in the spawner's list, counted on over rounds) plus `fxSize`, `fxType`, `zoneActor`. Never cached, a replay would delete an enemy that respawned |
| `ENEMY_DAMAGE` | teammates in the same stage and layer | `{v: 1, quiet: true, stageName, layerNo, hits: [...]}`; a hit is a spawn key as in `ENEMY_DEFEATED` plus `dmg` (0 to 30000), `pct` (the sender's health percent for that enemy, 100 to 500), `hpAfter` (the sender's health left at 100%, or -1) and, only when not 0, `hits` (1 to 4: counted hits of a Chu Worm, which dies at four whatever its health; `dmg` may then be 0). The receiver lowers its copy's health by `dmg` converted to its own percent, and further to `hpAfter` if that is lower (a hit it missed), never below 2 (11 for Wooden Puppets and Skulltulas), and a Chu Worm's hits left by `hits`, never below 1: deaths stay with `ENEMY_DEFEATED`. A helmeted Helmasaur's health is what its helmet still takes. Never cached |
| `STORY_EVENT` | teammates in the same stage and layer | `ph` = `start`, `joined` or `end`; a story cutscene teammates in the same room may watch too. Boss and mini-boss scenes are never announced or joined, and nobody is pulled in during a boss fight of their own. With `req: "npc"` it carries `npc: {prof, room, params, set, home, k, sd}`: the NPC's spawn key, the entry of its event table it ordered and a digest of the story flags; a teammate joins through its own copy of that NPC, only for allowlisted events and never in a randomizer game |
| `STORY_MOVE` | the whole team | `ph: "arrive"`: a story event moved the sender to another stage. Teammates get a prompt to follow. The last 16 per team are replayed on catch-up, oldest first. Besides `from`, `to`, `event`, `qual` and `hops` it carries `hl` (each arrival: `at`, the arrival demo's `m`, `sw`, `name`), `bits` (event bits the move set), `key`, `th` (it ended on a cutscene or battle layer) and `boss`. A teammate fighting a boss gets the prompt once the fight is over |

### Blocks and chains (teammates only, while `syncPushPull` is on)

| Packet | Goes to | Notes |
| --- | --- | --- |
| `PUSH_MOVE` | teammates in the same stage and layer | `{v: 1, stageName, layerNo, key, from: {x, z}, to: {x, z}, dir, pull, walk}`: a block the sender's Link (or iron ball) just set moving. `key` is the object's placement `{roomNo, procName, params, setId, home: {x, y, z}, angle: [x, y, z]}`; `dir` 0 to 3 in the object's own frame; `to` only for grid blocks (`Obj_Movebox`), `walk` only for ice blocks (1 a push, 2 an iron ball hit). A turning switch or push door (`Obj_SwTurn`, `Obj_Lv6SwTurn`, `Obj_PushDoor`) sends `step` and `toStep` (its turn steps, -12 to 12) for `from` and `to`, and `dir` 0 (a step up) or 1 (down) |
| `PUSH_REST` | teammates in the same stage and layer | `{v: 1, stageName, layerNo, key, from, pos: {x, z}}`: where that move stopped (`step` and `toStep` for a turning object) |
| `CHAIN_PULL` | teammates in the same stage and layer | `{v: 1, stageName, layerNo, key, held, hook, out, cap, end: {x, y, z}, rel: {x, y, z}}`: a chain switch the sender holds (by hand, or `hook` by the clawshot), ten a second and once more with `held: false` on release. `out` is the links out, `cap` the stepped chains' limit, `end` the handle; `rel` the handle from the sender's feet in its facing frame (hand only) |

A receiver plays a move with its own copy of the object only if that copy stands where the move
started (else it walks one cell or jumps to `to` or `pos`). Two moves from the same spot within five
seconds are concurrent: everyone follows the one from the lower client id, and the other sender's
copy goes where it ended. Its chain copy follows the puppet's hand and never sets or clears the
chain's switch itself; the puller's switch reaches it as a flag. All three are live only: never
cached or queued, so a player who arrives later sees what the switches saved.

### Teleport (one target, while `teleportMode` is on)

1. The requester sends `REQUEST_TELEPORT {targetClientId, requestId}`.
2. The relay forwards it to the target, or answers itself with `TELEPORT_TO {ok: false, reason,
   fromServer: true}`. Reasons: `rate-limited` (more than one request a second), `disabled`,
   `offline`, `not-in-game`, `other-team` (unless `teleportAcrossTeams`), `other-game` (always
   between different team games).
3. The target answers `TELEPORT_TO {targetClientId, requestId, ok, stageName, roomNo, layerNo,
   pos, angleY, ageMs, at}`, also during its own cutscenes: `pos`/`angleY`/`roomNo` is where a
   stage load puts the requester (the last spot the target stood on solid ground in its room, else
   the nearest standable start point of the room; `ageMs` 0 for a start point), and `at {x, y, z,
   angleY}` is the target's own position and facing. Or `ok: false` and a reason (`busy`,
   `loading` during a stage change, `no-safe-spot`). The relay checks the fields and passes it
   back, leaving out an unusable `at`.
4. The requester lands beside `at` (or the target's newer `PLAYER_UPDATE` when they share a stage
   and layer), without a reload when that room is loaded, else after loading `pos` on the layer
   its own progress picks; ground there that is not standable leaves it on `pos`.

### PvP (one target, while `pvpMode` is on)

The attacker's game decides that a hit landed and sends `DAMAGE_PLAYER {targetClientId, hitId,
kind, damage, spl, dirY, blocked, viewSeq}` (`spl` 0 light, 1 knockdown; `damage` in quarter
hearts, at most 4). The relay forwards it only when both players are in game in the same stage
and layer and the team rules allow it (friendly fire), at most every 200 ms per attacker and
victim; otherwise it answers with `DAMAGE_RESULT {result: "refused", reason}`. The victim
checks again, plays the hit through its own damage code and answers `DAMAGE_RESULT {targetClientId,
hitId, result, reason, damage, knockout}` with `result` = `applied`, `blocked` or `dropped`. The
relay passes the result back to the attacker.

`kind`: 0 sword, 1 wolf, 2 arrow, 3 bomb, 4 slingshot, 5 boomerang, 6 clawshot, 7 shield bash,
8 Ball and Chain, 9 stomp, 10 spinner, 11 horse. The victim clamps `damage` and `spl` to what its
own table allows for the kind (README, PvP); an unknown kind from a newer client is clamped to
the last one it knows.

`knockout` is only set on an applied hit: `ko` when it took the victim's last heart (`pvpLethal`),
`floor` when it took them down to the one-heart floor. The relay then sends `PVP_KNOCKOUT
{attackerClientId, victimClientId, knockout}` to the whole room, which shows it as a toast.

### Server to client only

`ALL_CLIENT_STATE`, `TEAM_STATE`, `TEAM_GAME_CONFLICT`, `SERVER_MESSAGE`, `DISABLE_CLIENT` and
`PVP_KNOCKOUT`. The relay drops them when a client sends them. `DISABLE_CLIENT` disconnects the
client without an automatic reconnect.
