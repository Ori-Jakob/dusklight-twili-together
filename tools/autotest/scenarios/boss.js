// Boss fights are never interrupted: B fights its own boss while A, in its own copy of the arena,
// kills A's boss, takes its heart container and warps out. Then B finishes its own clear.

const { COMMON_CVARS, STAGES, STORY, barrier, connect } = require("../lib");

// MoveRecord qual bit (StoryTypes.hpp).
const QUAL_BOSS = 64;
// fopAcM_createItemForBoss's item bit; dungeon bits STAGE_BOSS_ENEMY, STAGE_LIFE, STAGE_BOSS_ENEMY_2.
const HEART_BIT = 0x9f;
const HEART_ITEM = 0x22;
const BOSS_BIT = 3;
const LIFE_BIT = 4;
const MINIBOSS_BIT = 7;

const at = (s) => ({ op: "waitStage", stage: s, timeoutSec: 120 });
const settle = (timeoutSec = 300) => ({ op: "waitStorySettled", pressA: true, timeoutSec });
const noSave = { op: "dismissSaveRequest", optional: true, timeoutSec: 120 };
const fightStart = [
    { op: "keepLife" },
    { op: "markMaxLife" },
    // The arena plays its intro on entry.
    { op: "waitBossFight", quietTicks: 90, timeoutSec: 240 },
];

// A boss arena with the boss alive (lib.js's bossRoom checkpoints have it beaten).
const alive = (key, room = 50, point = 0) => ({ ...STORY[key], stage: STORY[key].stage, room, point, stageBoss: [] });

// A kills its boss the game's way, takes the heart container and leaves through the warp.
const killAndLeave = (b) => [
    { op: "killBoss", how: "real" },
    { op: "expectBossClear", heart: 1, warp: 1, timeoutSec: 300 },
    { op: "expectEventCount", name: "BOSSCLEAR", prefix: true, count: 1 },
    { op: "findFieldItem", bit: HEART_BIT, tag: "heart" },
    { op: "takeFieldItem", tag: "heart", timeoutSec: 90 },
    settle(),
    { op: "expectMaxLife", delta: 5 },
    { op: "triggerStory", via: "bossWarp" },
    noSave,
    settle(400),
    at(b.exitStage),
    { op: "expectStoryMove", role: "sent", qualHas: QUAL_BOSS, toStage: b.exitStage },
    { op: "signal", name: "a-out" },
];

// B's own clear: one death, one BOSSCLEAR, one warp, and no heart container: A took the team's
// (its STAGE_LIFE came through, its grant gave us the +5).
const ownClear = (b) => [
    { op: "killBoss", how: "real" },
    { op: "expectBossClear", heart: 0, warp: 1, timeoutSec: 300 },
    { op: "expectEventCount", name: "BOSSCLEAR", prefix: true, count: 1 },
    settle(),
    { op: "expectMaxLife", delta: 5, forSec: 5 },
    // Still in the arena: the held flags and A's prompt still wait.
    { op: "expectBossFight", value: true },
    { op: "expectNoPrompt", sec: 5 },
];

function bossScenario(b) {
    return {
        name: `boss-${b.id}`,
        description: `B fights ${b.name} while A kills its own copy, takes the heart container and warps out: B's fight goes on (boss alive, no event, warp or heart container, no prompt, boss bit held); then B's own clear plays once with no second heart container and the held flags merge`,
        tags: ["boss-fight"],
        timeoutSec: 1800,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: b.start,
                steps: [
                    ...connect, at(b.start.stage), { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...fightStart,
                    ...barrier("fighting", "B", 300),
                    ...killAndLeave(b),
                    ...barrier("b-clear", "B", 900),
                    ...barrier("b-out", "B", 600),
                    // B's own heart container never reaches us as a second one.
                    { op: "expectMaxLife", delta: 5, forSec: 10 },
                    { op: "expectDungeonBit", stage: b.start.stage, bit: BOSS_BIT },
                    { op: "expectDungeonBit", stage: b.start.stage, bit: LIFE_BIT },
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: b.start,
                steps: [
                    ...connect, at(b.start.stage), { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...fightStart,
                    { op: "expectBossFight", value: true },
                    ...barrier("fighting", "A", 300),
                    { op: "bossWatch", tag: b.id, until: "a-out", from: "A", holdSec: 20, timeoutSec: 900 },
                    { op: "expectBossHeld", min: 3 },
                    // Received mid-fight, uninterrupted: A's heart container (one, with its toast) and its bit.
                    { op: "expectItemToast", kind: "item", from: "A", item: HEART_ITEM, timeoutSec: 30 },
                    { op: "expectMaxLife", delta: 5, forSec: 5 },
                    { op: "expectDungeonBit", bit: LIFE_BIT },
                    { op: "expectDungeonBit", bit: BOSS_BIT, set: false },
                    ...ownClear(b),
                    ...barrier("b-clear", "A", 300),
                    { op: "triggerStory", via: "bossWarp" },
                    noSave,
                    settle(400),
                    at(b.exitStage),
                    { op: "expectBossFight", value: false },
                    { op: "expectBossHeld", max: 0, released: 3 },
                    // A went where our own warp took us: its move needs no prompt.
                    { op: "expectNoPrompt", sec: 15 },
                    { op: "expectMaxLife", delta: 5, forSec: 10 },
                    { op: "expectDungeonBit", stage: b.start.stage, bit: BOSS_BIT },
                    { op: "expectDungeonBit", stage: b.start.stage, bit: LIFE_BIT },
                    ...barrier("b-out", "A", 300),
                    { op: "quit" },
                ],
            },
        ],
    };
}

// The same with B's guard off: logs what A's kill does to B's fight (BOSSWATCH lines), fails nothing.
function observeScenario(b) {
    return {
        name: `boss-${b.id}-observe`,
        description: `observe: ${b.name} with B's boss-fight guard off; BOSSWATCH lines log what A's kill, heart container and warp-out change in B's fight, then B's own clear`,
        tags: ["boss-fight-observe"],
        manualOnly: true,
        timeoutSec: 1500,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: b.start,
                steps: [
                    ...connect, at(b.start.stage), { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...fightStart,
                    ...barrier("fighting", "B", 300),
                    ...killAndLeave(b),
                    ...barrier("b-done", "B", 900),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: b.start,
                steps: [
                    { op: "bossGuard", on: false },
                    ...connect, at(b.start.stage), { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...fightStart,
                    ...barrier("fighting", "A", 300),
                    { op: "bossWatch", tag: `${b.id}-observe`, expect: false, until: "a-out", from: "A", holdSec: 30, timeoutSec: 900 },
                    { op: "killBoss", how: "real", timeoutSec: 60 },
                    { op: "expectBossClear", heart: 1, warp: 1, soft: true, timeoutSec: 300 },
                    { op: "bossWatch", tag: `${b.id}-observe-after`, expect: false, sec: 5 },
                    ...barrier("b-done", "A", 300),
                    { op: "quit" },
                ],
            },
        ],
    };
}

// A mini-boss whose defeat scene the harness cannot drive: both replay its flag writes (the
// mini-boss bit and its switch) as its scene ends them, then leave the stage.
function miniBossScenario(m, observe) {
    const watch = observe ?
        { op: "bossWatch", tag: `${m.id}-observe`, expect: false, until: "a-out", from: "A", holdSec: 30, timeoutSec: 600 } :
        { op: "bossWatch", tag: m.id, until: "a-out", from: "A", holdSec: 20, timeoutSec: 600 };
    const leave = [{ op: "warp", ...m.exit }, at(m.exit.stage)];
    return {
        name: `boss-${m.id}${observe ? "-observe" : ""}`,
        description: observe ?
            `observe: ${m.name} with B's guard off; BOSSWATCH lines log what A's defeat flags change in B's fight` :
            `B fights ${m.name} while A's defeat flags (mini-boss bit, switch ${m.switchNo}) arrive and A leaves: B's fight goes on; B's own defeat flags then merge once B leaves`,
        tags: [observe ? "boss-fight-observe" : "boss-fight"],
        manualOnly: observe,
        timeoutSec: 1200,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: m.start,
                steps: [
                    ...connect, at(m.start.stage), { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...fightStart,
                    ...barrier("fighting", "B", 300),
                    { op: "killBoss", how: "flags" },
                    { op: "wait", sec: 3 },
                    ...leave,
                    settle(),
                    { op: "signal", name: "a-out" },
                    ...barrier("b-out", "B", 600),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: m.start,
                steps: [
                    ...(observe ? [{ op: "bossGuard", on: false }] : []),
                    ...connect, at(m.start.stage), { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...fightStart,
                    ...(observe ? [] : [{ op: "expectBossFight", value: true }]),
                    ...barrier("fighting", "A", 300),
                    watch,
                    ...(observe ? [] : [
                        { op: "expectBossHeld", min: 2 },
                        { op: "expectDungeonBit", bit: MINIBOSS_BIT, set: false },
                        { op: "killBoss", how: "flags" },
                        { op: "expectDungeonBit", bit: MINIBOSS_BIT },
                        { op: "expectBossFight", value: true },
                    ]),
                    ...leave,
                    settle(),
                    ...(observe ? [] : [
                        { op: "expectBossFight", value: false },
                        { op: "expectBossHeld", max: 0, released: 2 },
                        { op: "expectDungeonBit", stage: m.start.stage, bit: MINIBOSS_BIT },
                        { op: "expectStageSwitch", stage: m.start.stage, no: m.switchNo },
                        { op: "expectNoPrompt", sec: 5 },
                    ]),
                    ...barrier("b-out", "A", 300),
                    { op: "quit" },
                ],
            },
        ],
    };
}

const bosses = [
    { id: "diababa", name: "Diababa (Forest Temple)", start: alive("forestBoss"), exitStage: "F_SP108" },
    { id: "fyrus", name: "Fyrus (Goron Mines)", start: alive("goronMinesBoss"), exitStage: "F_SP109" },
];
const miniBosses = [
    {
        id: "dangoro",
        name: "Dangoro (Goron Mines)",
        start: { ...alive("goronMinesBoss"), stage: "D_MN04B", room: 51, point: 3 },
        exit: STAGES.goronMines,
        switchNo: 14,
    },
];

module.exports = [
    ...bosses.map(bossScenario),
    ...miniBosses.map((m) => miniBossScenario(m, false)),
    ...bosses.map(observeScenario),
    ...miniBosses.map((m) => miniBossScenario(m, true)),
];
