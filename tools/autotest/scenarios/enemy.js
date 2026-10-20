// Enemy health multiplier and enemy-death sync; steps in StepsEnemy.cpp.

const { STAGES, COMMON_CVARS, tt, waitStage, barrier, connect } = require("../lib");

// Bokoblin type 0 without switches: 40 health (d_a_e_oc.cpp daE_OC_c::create).
const BOKOBLIN = { name: "E_oc", param: 0xffffff00, dy: 30 };
// Tektite, not on the health-scaling allowlist: keeps its 80 whatever the room sets.
const TEKTITE = { name: "E_tt", param: 0xffffffff, dy: 30 };
// Mini Freezard, neither kill-synced (its death has no puff and no mark) nor duplicated.
const FREEZARD = { name: "E_fz", param: 0xffffffff, dy: 30 };
// Tile Worm with defeated switch 0x13 (its low param byte).
const TILE_WORM = { name: "E_hz", param: 0xffffff13, dy: 0 };
// Bokoblin type 0 with defeated switch 0x12.
const BOKO_SW = { name: "E_oc", param: 0xff12ff00, dy: 30 };
// Defeated switches 0x14, 0x15 and 0x16 (Young Gohma and Deku Baba: top byte, Tektite: low byte).
const GOHMA = { name: "E_kg", param: 0x14ffff00, dy: 30 };
const TEKTITE_SW = { name: "E_tt", param: 0xffffff15, dy: 30 };
const DEKU_BABA = { name: "E_db", param: 0x16ffff00, dy: 0 };
// The ceiling variants, 20 below it: Deku Baba type 2, Young Gohma type 1.
const DEKU_BABA_ROOF = { name: "E_db", param: 0x17ffff02, onRoof: true, dy: -20 };
const GOHMA_ROOF = { name: "E_kg", param: 0x18ffff01, onRoof: true, dy: -20 };
// No group, no roof spawn (d_a_e_s1.cpp); Stalfos keeps its switch in angle z.
const SHADOW_BEAST = { name: "E_s1", param: 0xffffffff, dy: 30 };
const STALFOS = { name: "E_sf", param: 0xffffffff, angleZ: 0xff, dy: 30 };
const HELMASAUR = { name: "E_mm", param: 0xffffffff, dy: 30 };
const CHU_WORM = { name: "E_sm", param: 0xffffffff, dy: 30 };
// E_Make (d_a_obj_enemy_create.cpp): type, count, activate and deactivate switches, no loop end.
const SPAWNER = (type, count, on, off) => ({
    name: "E_Make",
    param: ((0xff << 24) | (off << 16) | (on << 8) | (count << 4) | type) >>> 0,
});
// Seen by Enemy Count as placed by the stage.
const PLACED = { placed: true };
// Counts and digests near the start point: the stage's own enemies elsewhere in the room stay out.
const NEAR = 800;
// The spawned enemies attack the idle players; keep those alive for the whole run.
const CVARS = [...COMMON_CVARS, "game.infiniteHearts=1"];

// Kill sync matches spawn data: every instance spawns relative to the shared start point.
const HOME = { anchor: "playerHome" };
// Forest Temple entrance: elsewhere Bokoblins drown or fall, or a demo holds kills back.

// Spawns `enemies` ([tag, template, dx, dz, extra]) and waits for any that falls off a ledge.
const spawnAndSettle = (enemies) => [
    ...enemies.map(([tag, t, dx, dz, extra]) => ({ op: "spawnEnemy", ...t, ...HOME, dx, dz, tag, ...extra })),
    ...enemies.map(([tag]) => ({ op: "expectEnemyHealth", tag })),
    { op: "wait", frames: 150 },
    ...enemies.map(([tag]) => ({ op: "expectEnemyHealth", tag, timeoutSec: 0 })),
];

// barrier() for more than two instances.
const barrierAll = (name, others, timeoutSec = 120) => [
    { op: "signal", name },
    ...others.map((from) => ({ op: "waitSignal", name, from, timeoutSec })),
];

// enemy-kill-sync: one of each way to die, plus a plain delete and an enemy off the allowlist.
const KILL_SYNC_ENEMIES = [
    ["real", BOKOBLIN, 0, 250],
    ["za", BOKOBLIN, 250, 250, { setId: 0x20 }],
    ["fall", BOKOBLIN, -250, 250],
    ["del", BOKOBLIN, 250, -250],
    ["tek", TEKTITE, -250, -250],
    ["fz", FREEZARD, 0, -250],
];

const syncOn = (owner) => [
    ...(owner ? [{ op: "setRoomOption", name: "syncNPCs" }] : []),
    { op: "waitRoomOption", name: "syncNPCs", timeoutSec: 30 },
];

// Share Enemy Damage is on by default; it needs Sync Enemy Deaths.
const damageOn = (owner) => [
    ...syncOn(owner),
    ...(owner ? [{ op: "setRoomOption", name: "syncEnemyDamage" }] : []),
    { op: "waitRoomOption", name: "syncEnemyDamage", timeoutSec: 30 },
];

// Two instances in the Forest Temple entrance, A owns the room; `steps(isA, other)` after connecting.
const pair = (name, description, steps, cvars = CVARS) => ({
    name,
    description,
    timeoutSec: 300,
    cvars,
    instances: ["A", "B"].map((inst) => {
        const isA = inst === "A";
        const other = isA ? "B" : "A";
        return {
            name: inst,
            start: STAGES.forestTemple,
            launchDelayMs: isA ? 20000 : 1500,
            steps: [
                waitStage(STAGES.forestTemple),
                ...connect,
                ...steps(isA, other),
                ...barrier("done", other),
                { op: "quit" },
            ],
        };
    }),
});

const health = (tag, value, extra = {}) => ({ op: "expectEnemyHealth", tag, health: value, timeoutSec: 10, ...extra });

// Thirteen spots around the start point (enemy-count-cap).
const CAP_TEKTITES = [
    ...[-300, -150, 0, 150, 300].flatMap((dx) => [250, -250].map((dz) => [dx, dz])),
    ...[-150, 0, 150].map((dx) => [dx, 400]),
].map(([dx, dz], i) => ({ tag: `t${i + 1}`, dx, dz }));

module.exports = [
    {
        name: "enemy-health",
        description: "owner sets 200%/300% enemy health: Bokoblins scale on both clients (also one spawned before connecting), a Tektite does not, a get-up scales, damage does not, a disconnect reverts without rounding a live enemy down to 1, a deleted enemy is dropped",
        timeoutSec: 360,
        cvars: CVARS,
        instances: [
            {
                // Connects first, so it owns the room.
                name: "A",
                start: STAGES.southFaron,
                launchDelayMs: 20000,
                steps: [
                    waitStage(STAGES.southFaron),
                    ...connect,
                    { op: "setEnemyHealthPercent", value: 200 },
                    { op: "expectEnemyHealthPercent", value: 200, timeoutSec: 10 },
                    // Barrier signals are only relayed to connected clients.
                    { op: "waitPeers", count: 1, timeoutSec: 120 },
                    ...barrier("health-200", "B"),
                    { op: "spawnEnemy", ...BOKOBLIN, dz: 250, tag: "boko" },
                    { op: "expectEnemyHealth", tag: "boko", health: 80, max: 80, tracked: true },
                    { op: "spawnEnemy", ...TEKTITE, dx: 250, dz: 250, tag: "tek" },
                    { op: "expectEnemyHealth", tag: "tek", health: 80, max: 80, tracked: false },
                    ...barrier("spawned", "B"),
                    { op: "setEnemyHealthPercent", value: 300 },
                    { op: "expectEnemyHealth", tag: "boko", health: 120, max: 120, timeoutSec: 10 },
                    // Damage outside cc_at_check (a wolf bite) is not touched...
                    { op: "setEnemyHealth", tag: "boko", health: 20 },
                    { op: "wait", frames: 5 },
                    { op: "expectEnemyHealth", tag: "boko", health: 20, max: 120, timeoutSec: 0 },
                    // ...but getting back up to the unscaled full health is full scaled health.
                    { op: "setEnemyHealth", tag: "boko", health: 40 },
                    { op: "expectEnemyHealth", tag: "boko", health: 120, max: 120, timeoutSec: 5 },
                    ...barrier("a-done", "B"),
                    // Offline the percent is 100 again.
                    { op: "disconnect" },
                    { op: "expectEnemyHealthPercent", value: 100, timeoutSec: 5 },
                    { op: "expectEnemyHealth", tag: "boko", health: 40, max: 40, timeoutSec: 5 },
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    waitStage(STAGES.southFaron),
                    // Spawned offline: vanilla, but tracked so that connecting rescales it.
                    { op: "spawnEnemy", ...BOKOBLIN, dz: 250, tag: "early" },
                    { op: "expectEnemyHealth", tag: "early", health: 40, max: 40, tracked: true },
                    ...connect,
                    { op: "waitPeers", count: 1, timeoutSec: 60 },
                    ...barrier("health-200", "A"),
                    { op: "expectEnemyHealth", tag: "early", health: 80, max: 80, timeoutSec: 10 },
                    { op: "spawnEnemy", ...BOKOBLIN, dx: -250, dz: 250, tag: "boko" },
                    { op: "expectEnemyHealth", tag: "boko", health: 80, max: 80 },
                    ...barrier("spawned", "A"),
                    { op: "expectEnemyHealthPercent", value: 300, timeoutSec: 15 },
                    { op: "expectEnemyHealth", tag: "boko", health: 120, max: 120, timeoutSec: 10 },
                    { op: "expectEnemyHealth", tag: "early", health: 120, max: 120, timeoutSec: 10 },
                    // A deleted enemy leaves the scaling list (the rescale would write to freed memory).
                    { op: "deleteEnemy", tag: "boko" },
                    { op: "expectEnemyGone", tag: "boko", timeoutSec: 5 },
                    { op: "setEnemyHealth", tag: "early", health: 3 },
                    ...barrier("a-done", "A"),
                    // Back at 100%, 3 rounds to 1 (a dying state for some enemies), so it stops at 2.
                    { op: "disconnect" },
                    { op: "expectEnemyHealthPercent", value: 100, timeoutSec: 5 },
                    { op: "expectEnemyHealth", tag: "early", health: 2, max: 40, timeoutSec: 5 },
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "enemy-kill-sync",
        description: "with Sync Enemy Deaths on, Bokoblins A kills (its own death, a puff and delete that sets the zone actor bit, a fall into the void) vanish for B and the zone actor bit is mirrored; a Tektite vanishes too; a plain delete and a Mini Freezard (not allowlisted) do not sync, and nothing echoes back",
        timeoutSec: 300,
        cvars: CVARS,
        instances: [
            {
                // Connects first, so it owns the room.
                name: "A",
                start: STAGES.forestTemple,
                launchDelayMs: 20000,
                steps: [
                    waitStage(STAGES.forestTemple),
                    ...connect,
                    ...syncOn(true),
                    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...barrier("ready", "B"),
                    ...spawnAndSettle(KILL_SYNC_ENEMIES),
                    ...barrier("spawned", "B"),
                    { op: "killEnemy", tag: "real", how: "real" },
                    { op: "killEnemy", tag: "za", how: "disappear", onActor: true },
                    { op: "killEnemy", tag: "fall", how: "fall" },
                    { op: "deleteEnemy", tag: "del" },
                    { op: "killEnemy", tag: "tek", how: "disappear" },
                    { op: "killEnemy", tag: "fz", how: "disappear" },
                    { op: "expectEnemyGone", tag: "real", timeoutSec: 10 },
                    { op: "expectEnemyGone", tag: "za", timeoutSec: 5 },
                    { op: "expectEnemyGone", tag: "fall", timeoutSec: 10 },
                    { op: "expectZoneActor", setId: 0x20 },
                    ...barrier("killed", "B"),
                    { op: "expectEnemySync", sent: 4, received: 0, applied: 0 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.forestTemple,
                steps: [
                    waitStage(STAGES.forestTemple),
                    ...connect,
                    ...syncOn(false),
                    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...barrier("ready", "A"),
                    ...spawnAndSettle(KILL_SYNC_ENEMIES),
                    ...barrier("spawned", "A"),
                    { op: "expectEnemyGone", tag: "za", timeoutSec: 10 },
                    { op: "expectEnemyGone", tag: "real", timeoutSec: 15 },
                    { op: "expectEnemyGone", tag: "fall", timeoutSec: 15 },
                    { op: "expectEnemyGone", tag: "tek", timeoutSec: 15 },
                    { op: "expectZoneActor", setId: 0x20 },
                    ...barrier("killed", "A"),
                    { op: "wait", frames: 90 },
                    // A plain delete is no defeat, and the Mini Freezard is not on the allowlist.
                    { op: "expectEnemyHealth", tag: "del", timeoutSec: 0 },
                    { op: "expectEnemyHealth", tag: "fz", timeoutSec: 0 },
                    { op: "expectEnemySync", sent: 0, received: 4, applied: 4, echoes: 0, expired: 0 },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "enemy-kill-scope",
        description: "a kill reaches a teammate but not a player on another team, and nobody once the owner turns Sync Enemy Deaths off",
        timeoutSec: 360,
        cvars: CVARS,
        instances: [
            ["A", "red", ["B", "C"]],
            ["B", "red", ["A", "C"]],
            ["C", "blue", ["A", "B"]],
        ].map(([name, team, others]) => {
            const isA = name === "A";
            return {
                name,
                start: STAGES.forestTemple,
                // A connects first, so it owns the room.
                launchDelayMs: isA ? 20000 : 3000,
                steps: [
                    waitStage(STAGES.forestTemple),
                    { op: "connect", team },
                    { op: "waitConnected", timeoutSec: 20 },
                    ...syncOn(isA),
                    { op: "waitPeers", count: 2, sameStage: true, timeoutSec: 150 },
                    ...barrierAll("ready", others),
                    ...spawnAndSettle([["one", BOKOBLIN, 0, 250], ["two", BOKOBLIN, 250, 250]]),
                    ...barrierAll("spawned", others),
                    ...(isA
                        ? [{ op: "killEnemy", tag: "one" }, { op: "expectEnemyGone", tag: "one", timeoutSec: 5 }]
                        : name === "B"
                            ? [{ op: "expectEnemyGone", tag: "one", timeoutSec: 15 }]
                            : []),
                    ...barrierAll("one-killed", others),
                    // C is on another team: its copy stays.
                    ...(name === "C" ? [{ op: "wait", frames: 60 }, { op: "expectEnemyHealth", tag: "one", timeoutSec: 0 }] : []),
                    ...(isA ? [{ op: "setRoomOption", name: "syncNPCs", value: false }] : []),
                    { op: "waitRoomOption", name: "syncNPCs", value: false, timeoutSec: 30 },
                    ...barrierAll("sync-off", others),
                    ...(isA ? [{ op: "killEnemy", tag: "two" }, { op: "expectEnemyGone", tag: "two", timeoutSec: 5 }] : []),
                    ...barrierAll("two-killed", others),
                    ...(isA ? [] : [{ op: "wait", frames: 90 }, { op: "expectEnemyHealth", tag: "two", timeoutSec: 0 }]),
                    { op: "expectEnemySync", ...(isA ? { sent: 1, received: 0 } : name === "B" ? { sent: 0, received: 1, applied: 1 } : { sent: 0, received: 0, applied: 0 }) },
                    ...barrierAll("done", others),
                    { op: "quit" },
                ],
            };
        }),
    },
    {
        name: "enemy-room-clear",
        description: "with world sync off, A's kill empties the room for B too, so B's own ALLdie sets its room-clear switch",
        timeoutSec: 300,
        cvars: CVARS,
        instances: ["A", "B"].map((name) => {
            const isA = name === "A";
            const other = isA ? "B" : "A";
            return {
                name,
                start: STAGES.linksHouse,
                launchDelayMs: isA ? 20000 : 1500,
                steps: [
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    // Without world sync only B's own ALLdie can set its switch.
                    ...(isA ? [{ op: "setRoomOption", name: "syncWorldState", value: false }] : []),
                    { op: "waitRoomOption", name: "syncWorldState", value: false, timeoutSec: 30 },
                    ...syncOn(isA),
                    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...barrier("ready", other),
                    ...spawnAndSettle([["boko", BOKOBLIN, 0, -150]]),
                    // Event 0xFF, zone switch 0xC5; spawned once the room has an enemy or it fires at once.
                    { op: "spawnEnemy", name: "ALLdie", param: 0xff00c5ff, tag: "alldie" },
                    { op: "expectEnemyHealth", tag: "alldie" },
                    { op: "wait", frames: 90 },
                    { op: "expectSwitch", no: 0xc5, room: 4, set: false, timeoutSec: 0 },
                    ...barrier("spawned", other),
                    ...(isA ? [{ op: "killEnemy", tag: "boko" }] : []),
                    { op: "expectEnemyGone", tag: "boko", timeoutSec: 15 },
                    // ALLdie waits 65 frames after the room is empty.
                    { op: "expectSwitch", no: 0xc5, room: 4, set: true, timeoutSec: 15 },
                    ...barrier("done", other),
                    { op: "quit" },
                ],
            };
        }),
    },
    {
        name: "enemy-kill-group",
        description: "a Shadow Beast group condemned on A disappears on B in one go",
        timeoutSec: 300,
        cvars: CVARS,
        instances: ["A", "B"].map((name) => {
            const isA = name === "A";
            const other = isA ? "B" : "A";
            // Group 2, no roof spawn, no path, no switch (d_a_e_s1.cpp daE_S1_Create).
            const S1 = { name: "E_s1", param: 0xffffff2f, dy: 30 };
            const tags = ["s1a", "s1b", "s1c"];
            return {
                name,
                start: STAGES.forestTemple,
                launchDelayMs: isA ? 20000 : 1500,
                steps: [
                    waitStage(STAGES.forestTemple),
                    ...connect,
                    ...syncOn(isA),
                    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...barrier("ready", other),
                    ...spawnAndSettle([["s1a", S1, -250, 250], ["s1b", S1, 0, 250], ["s1c", S1, 250, 250]]),
                    ...barrier("spawned", other),
                    ...(isA
                        ? [{ op: "forceGroupFail", tags }, { op: "expectEnemiesGone", tags, timeoutSec: 15 }]
                        : [{ op: "expectEnemiesGone", tags, maxSpreadTicks: 1, timeoutSec: 15 }]),
                    ...barrier("gone", other),
                    { op: "expectEnemySync", ...(isA ? { sent: 3, received: 0 } : { sent: 0, received: 3, applied: 3 }) },
                    ...barrier("done", other),
                    { op: "quit" },
                ],
            };
        }),
    },
    pair("enemy-damage-share",
        "at 200% health, a teammate's hits lower our copy of a Bokoblin (both ways), a health write without a hit is not shared, a Tektite is not shared, a remote hit stops at the floor of 2, and the kill still syncs",
        (isA, other) => [
            ...(isA ? [{ op: "setEnemyHealthPercent", value: 200 }] : []),
            { op: "expectEnemyHealthPercent", value: 200, timeoutSec: 30 },
            ...damageOn(isA),
            { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
            ...barrier("ready", other),
            ...spawnAndSettle([["boko", BOKOBLIN, 0, 250], ["tek", TEKTITE, 250, 250], ["boko2", BOKOBLIN, -250, 250]]),
            health("boko", 80, { max: 80 }),
            ...barrier("spawned", other),
            ...(isA ? [{ op: "damageEnemy", tag: "boko", amount: 30 }] : []),
            health("boko", 50),
            ...barrier("a-hit", other),
            ...(isA ? [] : [{ op: "damageEnemy", tag: "boko", amount: 30 }]),
            health("boko", 20),
            ...barrier("b-hit", other),
            // Outside the attribution window of A's own hit: a write without a hit stays local.
            { op: "wait", frames: 30 },
            ...(isA ? [{ op: "setEnemyHealth", tag: "boko", health: 5 }, { op: "damageEnemy", tag: "tek", amount: 20 }] : []),
            { op: "wait", frames: 60 },
            health("boko", isA ? 5 : 20, { timeoutSec: 0 }),
            health("tek", isA ? 60 : 80, { timeoutSec: 0 }),
            ...barrier("unshared", other),
            ...(isA ? [{ op: "damageEnemy", tag: "boko2", amount: 60 }] : []),
            health("boko2", 20),
            ...barrier("boko2-hit", other),
            ...(isA ? [] : [{ op: "damageEnemy", tag: "boko2", amount: 10 }]),
            health("boko2", 10),
            ...barrier("boko2-hit2", other),
            // More than B has left: B's copy stops at the floor instead of dying.
            ...(isA ? [{ op: "sendEnemyDamageForTest", tag: "boko2", dmg: 50, pct: 200 }] : [health("boko2", 2)]),
            ...barrier("floored", other),
            ...(isA ? [{ op: "killEnemy", tag: "boko2", how: "real" }] : []),
            { op: "expectEnemyGone", tag: "boko2", timeoutSec: 15 },
            { op: "expectEnemyDamage", ...(isA ? { sent: 2, received: 2, applied: 2, floored: 0, dropped: 0 } : { sent: 2, received: 3, applied: 3, floored: 1, dropped: 0 }) },
        ]),
    pair("enemy-damage-scale",
        "a hit converts between the sender's and our health percent, a rescale is never sent as damage, and the sender's health heals a hit we missed",
        (isA, other) => [
            ...(isA ? [{ op: "setEnemyHealthPercent", value: 200 }] : []),
            { op: "expectEnemyHealthPercent", value: 200, timeoutSec: 30 },
            ...damageOn(isA),
            { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
            ...barrier("ready", other),
            ...spawnAndSettle([["boko", BOKOBLIN, 0, 250]]),
            health("boko", 80, { max: 80 }),
            ...barrier("spawned", other),
            // 30 at 300% is 20 at our 200%.
            ...(isA ? [{ op: "sendEnemyDamageForTest", tag: "boko", dmg: 30, pct: 300 }] : [health("boko", 60)]),
            ...barrier("converted", other),
            ...(isA ? [{ op: "damageEnemy", tag: "boko", amount: 10 }] : []),
            health("boko", isA ? 70 : 50),
            ...barrier("hit", other),
            ...(isA ? [{ op: "setEnemyHealthPercent", value: 300 }] : []),
            health("boko", isA ? 105 : 75, { max: 120, timeoutSec: 20 }),
            ...barrier("rescaled", other),
            ...(isA ? [{ op: "damageEnemy", tag: "boko", amount: 30 }] : []),
            health("boko", isA ? 75 : 45),
            ...barrier("hit2", other),
            // The sender had 10 left at 100% (30 at our 300%): B missed a hit and drops to it.
            ...(isA ? [{ op: "sendEnemyDamageForTest", tag: "boko", dmg: 5, pct: 300, hpAfter: 10 }] : [health("boko", 30)]),
            ...barrier("healed", other),
            { op: "wait", frames: 30 },
            { op: "expectEnemyDamage", ...(isA ? { sent: 2, received: 0 } : { sent: 0, received: 4, applied: 4, floored: 0, healed: 1, dropped: 0 }) },
        ]),
    {
        name: "enemy-damage-scope",
        description: "a hit reaches a teammate's copy but not another team's, and nobody's once the owner turns Share Enemy Damage off",
        timeoutSec: 360,
        cvars: CVARS,
        instances: [
            ["A", "red", ["B", "C"]],
            ["B", "red", ["A", "C"]],
            ["C", "blue", ["A", "B"]],
        ].map(([name, team, others]) => {
            const isA = name === "A";
            return {
                name,
                start: STAGES.forestTemple,
                launchDelayMs: isA ? 20000 : 3000,
                steps: [
                    waitStage(STAGES.forestTemple),
                    { op: "connect", team },
                    { op: "waitConnected", timeoutSec: 20 },
                    ...damageOn(isA),
                    { op: "waitPeers", count: 2, sameStage: true, timeoutSec: 150 },
                    ...barrierAll("ready", others),
                    ...spawnAndSettle([["one", BOKOBLIN, 0, 250]]),
                    ...barrierAll("spawned", others),
                    ...(isA ? [{ op: "damageEnemy", tag: "one", amount: 10 }] : []),
                    ...(name === "C" ? [{ op: "wait", frames: 60 }, health("one", 40, { timeoutSec: 0 })] : [health("one", 30)]),
                    ...barrierAll("hit", others),
                    ...(isA ? [{ op: "setRoomOption", name: "syncEnemyDamage", value: false }] : []),
                    { op: "waitRoomOption", name: "syncEnemyDamage", value: false, timeoutSec: 30 },
                    ...barrierAll("share-off", others),
                    ...(isA ? [{ op: "damageEnemy", tag: "one", amount: 10 }] : []),
                    ...barrierAll("hit2", others),
                    { op: "wait", frames: 90 },
                    health("one", isA ? 20 : name === "B" ? 30 : 40, { timeoutSec: 0 }),
                    { op: "expectEnemyDamage", ...(isA ? { sent: 1, received: 0 } : name === "B" ? { sent: 0, received: 1, applied: 1 } : { sent: 0, received: 0, applied: 0 }) },
                    ...barrierAll("done", others),
                    { op: "quit" },
                ],
            };
        }),
    },
    pair("enemy-count",
        "at 200% a placed Bokoblin gets one extra on both clients (a Mini Freezard none), the extra's kill syncs by its extra index and leaves the original, and only the original's death sets its switch",
        (isA, other) => [
            ...(isA ? [{ op: "setEnemyCountPercent", value: 200 }] : []),
            { op: "expectEnemyCountPercent", value: 200, timeoutSec: 30 },
            ...syncOn(isA),
            { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
            ...barrier("ready", other),
            ...spawnAndSettle([["b1", BOKO_SW, 0, 250, PLACED], ["fz", FREEZARD, 250, 250, PLACED]]),
            { op: "expectEnemyCount", name: "E_oc", count: 2, maxDist: NEAR },
            { op: "expectEnemyCount", name: "E_fz", count: 1, maxDist: NEAR, timeoutSec: 0 },
            { op: "tagExtra", tag: "x1", of: "b1", dup: 1 },
            { op: "expectEnemyExtras", of: "b1", count: 1 },
            { op: "dumpEnemies" },
            ...barrier("spawned", other),
            ...(isA ? [{ op: "killEnemy", tag: "x1" }] : []),
            { op: "expectEnemyGone", tag: "x1", timeoutSec: 15 },
            { op: "wait", frames: 60 },
            { op: "expectEnemyHealth", tag: "b1", timeoutSec: 0 },
            { op: "expectSwitch", no: 0x12, room: 0, set: false, timeoutSec: 0 },
            ...barrier("extra-killed", other),
            ...(isA ? [] : [{ op: "killEnemy", tag: "b1", how: "real" }]),
            { op: "expectEnemyGone", tag: "b1", timeoutSec: 15 },
            { op: "expectSwitch", no: 0x12, room: 0, timeoutSec: 15 },
            ...barrier("killed", other),
            { op: "expectEnemySync", sent: 1, received: 1, applied: 1 },
        ]),
    pair("enemy-count-150",
        "at 150% about half of six placed Bokoblins get an extra, the same ones in the same spots on both clients",
        (isA, other) => [
            ...(isA ? [{ op: "setEnemyCountPercent", value: 150 }] : []),
            { op: "expectEnemyCountPercent", value: 150, timeoutSec: 30 },
            { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
            ...barrier("ready", other),
            ...spawnAndSettle([
                ["b1", BOKOBLIN, -300, 250, PLACED], ["b2", BOKOBLIN, 0, 250, PLACED], ["b3", BOKOBLIN, 300, 250, PLACED],
                ["b4", BOKOBLIN, -300, -250, PLACED], ["b5", BOKOBLIN, 0, -250, PLACED], ["b6", BOKOBLIN, 300, -250, PLACED],
            ]),
            { op: "dumpEnemies" },
            { op: "enemyDigest", maxDist: NEAR },
            { op: "expectPeerEnemyDigest", from: other, maxDist: NEAR },
        ]),
    {
        name: "enemy-count-room-clear",
        description: "with world sync off, an extra keeps the room from being cleared after the original dies, and its kill clears it on both clients",
        timeoutSec: 300,
        cvars: CVARS,
        instances: ["A", "B"].map((name) => {
            const isA = name === "A";
            const other = isA ? "B" : "A";
            return {
                name,
                start: STAGES.linksHouse,
                launchDelayMs: isA ? 20000 : 1500,
                steps: [
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    ...(isA ? [{ op: "setRoomOption", name: "syncWorldState", value: false }, { op: "setEnemyCountPercent", value: 200 }] : []),
                    { op: "waitRoomOption", name: "syncWorldState", value: false, timeoutSec: 30 },
                    { op: "expectEnemyCountPercent", value: 200, timeoutSec: 30 },
                    ...syncOn(isA),
                    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...barrier("ready", other),
                    ...spawnAndSettle([["boko", BOKOBLIN, 0, -150, PLACED]]),
                    { op: "tagExtra", tag: "x1", of: "boko", dup: 1 },
                    { op: "spawnEnemy", name: "ALLdie", param: 0xff00c5ff, tag: "alldie" },
                    { op: "expectEnemyHealth", tag: "alldie" },
                    { op: "wait", frames: 90 },
                    { op: "expectSwitch", no: 0xc5, room: 4, set: false, timeoutSec: 0 },
                    ...barrier("spawned", other),
                    ...(isA ? [{ op: "killEnemy", tag: "boko" }] : []),
                    { op: "expectEnemyGone", tag: "boko", timeoutSec: 15 },
                    // ALLdie waits 65 frames after the room is empty: the extra still counts.
                    { op: "wait", frames: 120 },
                    { op: "expectSwitch", no: 0xc5, room: 4, set: false, timeoutSec: 0 },
                    ...barrier("original-killed", other),
                    ...(isA ? [{ op: "killEnemy", tag: "x1" }] : []),
                    { op: "expectEnemyGone", tag: "x1", timeoutSec: 15 },
                    { op: "expectSwitch", no: 0xc5, room: 4, set: true, timeoutSec: 15 },
                    ...barrier("done", other),
                    { op: "quit" },
                ],
            };
        }),
    },
    {
        name: "enemy-count-late-join",
        description: "a placed Bokoblin spawned while offline gets its extra once the client joins a 200% room, the same one its teammate has",
        timeoutSec: 300,
        cvars: CVARS,
        instances: ["A", "B"].map((name) => {
            const isA = name === "A";
            const other = isA ? "B" : "A";
            return {
                name,
                start: STAGES.forestTemple,
                launchDelayMs: isA ? 20000 : 1500,
                steps: [
                    waitStage(STAGES.forestTemple),
                    ...(isA ? [
                        ...connect,
                        { op: "setEnemyCountPercent", value: 200 },
                        { op: "expectEnemyCountPercent", value: 200, timeoutSec: 30 },
                        ...spawnAndSettle([["b", BOKOBLIN, 0, 250, PLACED]]),
                        { op: "expectEnemyCount", name: "E_oc", count: 2, maxDist: NEAR },
                        { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 150 },
                    ] : [
                        ...spawnAndSettle([["b", BOKOBLIN, 0, 250, PLACED]]),
                        // Offline the room's percent is unknown: no extra yet.
                        { op: "expectEnemyCount", name: "E_oc", count: 1, maxDist: NEAR, timeoutSec: 0 },
                        ...connect,
                        { op: "expectEnemyCountPercent", value: 200, timeoutSec: 30 },
                        { op: "expectEnemyCount", name: "E_oc", count: 2, maxDist: NEAR },
                        { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 60 },
                    ]),
                    ...barrier("joined", other),
                    { op: "enemyDigest", maxDist: NEAR },
                    { op: "expectPeerEnemyDigest", from: other, maxDist: NEAR },
                    ...barrier("done", other),
                    { op: "quit" },
                ],
            };
        }),
    },
    {
        name: "enemy-hz-sync",
        description: "a Tile Worm A defeats stays on B as a dead tile (defeated in place, not deleted) and its switch syncs; a worm created dead reports nothing",
        timeoutSec: 300,
        cvars: CVARS,
        instances: ["A", "B"].map((name) => {
            const isA = name === "A";
            const other = isA ? "B" : "A";
            return {
                name,
                start: STAGES.forestTemple,
                launchDelayMs: isA ? 20000 : 1500,
                steps: [
                    waitStage(STAGES.forestTemple),
                    ...connect,
                    ...syncOn(isA),
                    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...barrier("ready", other),
                    ...spawnAndSettle([["hz", TILE_WORM, 0, 400]]),
                    ...barrier("spawned", other),
                    ...(isA ? [{ op: "killEnemy", tag: "hz", how: "hz" }] : []),
                    { op: "expectEnemyDeadInPlace", tag: "hz", timeoutSec: 15 },
                    // A's own death wait sets it; B gets it through world sync.
                    { op: "expectSwitch", no: 0x13, room: 0, timeoutSec: 15 },
                    { op: "expectEnemySync", ...(isA ? { sent: 1, received: 0 } : { sent: 0, received: 1, applied: 1 }) },
                    ...barrier("killed", other),
                    // The switch is set now: this one is created dead and never puffs.
                    { op: "spawnEnemy", ...TILE_WORM, ...HOME, dx: 250, dz: 400, tag: "hz2" },
                    { op: "expectEnemyDeadInPlace", tag: "hz2", timeoutSec: 15 },
                    { op: "wait", frames: 90 },
                    { op: "expectEnemySync", ...(isA ? { sent: 1, received: 0 } : { sent: 0, received: 1, applied: 1 }), timeoutSec: 0 },
                    ...barrier("done", other),
                    { op: "quit" },
                ],
            };
        }),
    },
    pair("enemy-count-kinds",
        "at 200% a placed Young Gohma, Tektite and Deku Baba get one extra each, alike on both clients and without their defeated switch; an extra's kill syncs and leaves its original, the switches and the Deku Baba's zone actor bit alone, the original's kill still sets that bit",
        (isA, other) => [
            ...(isA ? [{ op: "setEnemyCountPercent", value: 200 }] : []),
            { op: "expectEnemyCountPercent", value: 200, timeoutSec: 30 },
            ...syncOn(isA),
            { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
            ...barrier("ready", other),
            ...spawnAndSettle([["kg", GOHMA, -250, 250, PLACED], ["tt", TEKTITE_SW, 0, 300, PLACED], ["db", DEKU_BABA, 250, 250, { placed: true, setId: 0x21 }]]),
            ...["kg", "tt", "db"].map((of) => ({ op: "expectEnemyExtras", of, count: 1 })),
            ...["kg", "tt", "db"].map((of) => ({ op: "tagExtra", tag: `x${of}`, of, dup: 1 })),
            { op: "dumpEnemies" },
            { op: "enemyDigest", maxDist: NEAR },
            { op: "expectPeerEnemyDigest", from: other, maxDist: NEAR },
            ...barrier("spawned", other),
            ...(isA ? [{ op: "killEnemy", tag: "xkg" }, { op: "killEnemy", tag: "xtt" }, { op: "killEnemy", tag: "xdb", onActor: true }] : []),
            ...["xkg", "xtt", "xdb"].map((tag) => ({ op: "expectEnemyGone", tag, timeoutSec: 15 })),
            { op: "wait", frames: 60 },
            ...["kg", "tt", "db"].map((tag) => ({ op: "expectEnemyHealth", tag, timeoutSec: 0 })),
            ...[0x14, 0x15, 0x16].map((no) => ({ op: "expectSwitch", no, room: 0, set: false, timeoutSec: 0 })),
            { op: "expectZoneActor", setId: 0x21, set: false, timeoutSec: 0 },
            ...barrier("extras-killed", other),
            ...(isA ? [] : [{ op: "killEnemy", tag: "db", onActor: true }]),
            { op: "expectEnemyGone", tag: "db", timeoutSec: 15 },
            { op: "expectZoneActor", setId: 0x21 },
            ...barrier("killed", other),
            { op: "expectEnemySync", ...(isA ? { sent: 3, received: 1, applied: 1 } : { sent: 1, received: 3, applied: 3 }) },
        ]),
    pair("enemy-count-roof",
        "at 200% a Deku Baba and a Young Gohma hanging from the ceiling get their extra on the ceiling too, as far below it, alike on both clients",
        (isA, other) => [
            ...(isA ? [{ op: "setEnemyCountPercent", value: 200 }] : []),
            { op: "expectEnemyCountPercent", value: 200, timeoutSec: 30 },
            { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
            ...barrier("ready", other),
            ...spawnAndSettle([["db", DEKU_BABA_ROOF, -250, 250, PLACED], ["kg", GOHMA_ROOF, 250, 250, PLACED]]),
            ...["db", "kg"].map((of) => ({ op: "expectEnemyExtras", of, count: 1, roofGap: 5 })),
            { op: "dumpEnemies" },
            // The ceiling is far above the start point.
            { op: "enemyDigest", maxDist: NEAR, flat: true },
            { op: "expectPeerEnemyDigest", from: other, maxDist: NEAR, flat: true },
        ]),
    pair("enemy-damage-kinds",
        "teammates' hits are shared on a Shadow Beast, a Stalfos, a Helmasaur (what its helmet still takes while it wears one, its body once it is off) and a Chu Worm (its counted hits too, never the last), and a downed copy is left alone",
        (isA, other) => [
            ...damageOn(isA),
            { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
            ...barrier("ready", other),
            ...spawnAndSettle([["s1", SHADOW_BEAST, -250, 250], ["sf", STALFOS, 250, 250], ["mm", HELMASAUR, -250, -250], ["sm", CHU_WORM, 250, -250]]),
            health("s1", 50), health("sf", 200), health("mm", 20, { helmetHealth: 2000, shared: 20 }), health("sm", 80, { hitsLeft: 4 }),
            ...barrier("spawned", other),
            ...(isA ? [{ op: "damageEnemy", tag: "s1", amount: 10 }] : [{ op: "damageEnemy", tag: "sf", amount: 50 }]),
            health("s1", 40), health("sf", 150),
            ...barrier("plain", other),
            ...(isA ? [{ op: "damageEnemy", tag: "mm", helmet: true, amount: 5 }] : []),
            health("mm", 20, { helmetHealth: 1995, shared: 15 }),
            ...barrier("helmet", other),
            // As in the game, what the helmet took stays with the helmet.
            ...(isA ? [] : [{ op: "removeHelmet", tag: "mm" }]),
            health("mm", 20, { shared: isA ? 15 : 20 }),
            ...barrier("helmet-off", other),
            // A's helmet hit on B's bare body, and down to A's own health (a hit B missed).
            ...(isA ? [{ op: "damageEnemy", tag: "mm", helmet: true, amount: 5 }] : []),
            health("mm", isA ? 20 : 10, { shared: 10 }),
            ...barrier("bare", other),
            ...(isA ? [] : [{ op: "damageEnemy", tag: "mm", amount: 3 }]),
            health("mm", isA ? 20 : 7, { shared: 7, ...(isA ? { helmetHealth: 1987 } : {}) }),
            ...barrier("body", other),
            ...(isA ? [{ op: "damageEnemy", tag: "sm", amount: 2, hits: 1 }] : []),
            health("sm", 78, { hitsLeft: 3 }),
            ...barrier("sm-hit", other),
            ...(isA ? [] : [{ op: "damageEnemy", tag: "sm", amount: 2, hits: 2 }]),
            health("sm", 76, { hitsLeft: 1 }),
            ...barrier("sm-hit2", other),
            // More counted hits than B's copy has left: it keeps its last one.
            ...(isA ? [{ op: "sendEnemyDamageForTest", tag: "sm", dmg: 1, pct: 100, hits: 3 }] : [health("sm", 75, { hitsLeft: 1 })]),
            ...barrier("sm-last", other),
            // B's Shadow Beast downed (health 0): A's hit leaves it there.
            ...(isA ? [] : [{ op: "setEnemyHealth", tag: "s1", health: 0 }]),
            ...barrier("downed", other),
            ...(isA ? [{ op: "damageEnemy", tag: "s1", amount: 5 }] : []),
            health("s1", isA ? 35 : 0),
            ...barrier("downed-hit", other),
            { op: "wait", frames: 30 },
            { op: "expectEnemyDamage", ...(isA ? { sent: 5, received: 3, applied: 3, floored: 0, healed: 0, dropped: 0 } : { sent: 3, received: 6, applied: 5, floored: 0, healed: 1, dropped: 1 }) },
        ]),
    pair("enemy-spawner",
        "at 200% an enemy spawner of two Bokoblins makes four on both clients (a rat spawner keeps its two), each child's kill syncs by its place in the spawner's list, and the spawner's switch turns on once all four are gone",
        (isA, other) => [
            ...(isA ? [{ op: "setEnemyCountPercent", value: 200 }] : []),
            { op: "expectEnemyCountPercent", value: 200, timeoutSec: 30 },
            ...syncOn(isA),
            { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
            ...barrier("ready", other),
            { op: "spawnEnemy", ...SPAWNER(7, 2, 0x30, 0x31), ...HOME, dz: 300, tag: "mk", placed: true },
            { op: "spawnEnemy", ...SPAWNER(2, 2, 0x32, 0xff), ...HOME, dx: -300, dz: -300, tag: "rats", placed: true },
            { op: "expectBroodSize", of: "mk", count: 4 },
            { op: "expectBroodSize", of: "rats", count: 2 },
            { op: "wait", frames: 30 },
            { op: "expectEnemyCount", name: "E_oc", count: 0, maxDist: NEAR, timeoutSec: 0 },
            ...barrier("placed", other),
            { op: "setSwitch", no: 0x30, room: 0 },
            { op: "expectEnemyCount", name: "E_oc", count: 4, maxDist: NEAR },
            ...[0, 1, 2, 3].map((index) => ({ op: "tagBrood", tag: `c${index}`, of: "mk", index })),
            ...[0, 1, 2, 3].map((index) => ({ op: "expectEnemyHealth", tag: `c${index}` })),
            { op: "dumpEnemies" },
            ...barrier("spawned", other),
            ...(isA ? [{ op: "killEnemy", tag: "c0", how: "real" }, { op: "killEnemy", tag: "c2", how: "real" }] : []),
            ...["c0", "c2"].map((tag) => ({ op: "expectEnemyGone", tag, timeoutSec: 15 })),
            { op: "wait", frames: 30 },
            ...["c1", "c3"].map((tag) => ({ op: "expectEnemyHealth", tag, timeoutSec: 0 })),
            { op: "expectSwitch", no: 0x31, room: 0, set: false, timeoutSec: 0 },
            ...barrier("half", other),
            ...(isA ? [] : [{ op: "killEnemy", tag: "c1", how: "real" }, { op: "killEnemy", tag: "c3", how: "real" }]),
            ...["c1", "c3"].map((tag) => ({ op: "expectEnemyGone", tag, timeoutSec: 15 })),
            { op: "expectSwitch", no: 0x31, room: 0, timeoutSec: 15 },
            ...barrier("cleared", other),
            { op: "expectEnemySync", sent: 2, received: 2, applied: 2, echoes: 0 },
        ]),
    pair("enemy-count-cap",
        "at 300% thirteen placed Tektites want 26 extras: the room's cap of 24 keeps the same ones on both clients, though they spawn the Tektites in opposite orders",
        (isA, other) => [
            { op: "expectEnemyCountPercent", value: 300, timeoutSec: 30 },
            { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
            ...barrier("ready", other),
            { op: "spawnEnemies", ...TEKTITE, ...HOME, placed: true, enemies: isA ? CAP_TEKTITES : [...CAP_TEKTITES].reverse() },
            ...CAP_TEKTITES.map(({ tag }) => ({ op: "expectEnemyHealth", tag })),
            { op: "expectRoomExtras", count: 24 },
            { op: "wait", frames: 60 },
            { op: "expectRoomExtras", count: 24, timeoutSec: 0 },
            { op: "dumpEnemies" },
            { op: "enemyDigest", maxDist: NEAR },
            { op: "expectPeerEnemyDigest", from: other, maxDist: NEAR },
        ],
        // The owner's percent from the start, so the room's own enemies get theirs alike too.
        [...CVARS, tt("enemy_count_multiplier", 300)]),
];
