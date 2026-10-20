// Heart pieces a teammate took never show up for us and never come twice: the game's own placements (a
// field heart piece, a chest), a heart piece dropped at run time, an NPC reward check in a randomizer
// game, and a boss heart container (vanilla and randomizer).

const { MODS, STAGES, STORY, COMMON_CVARS, RANDOMIZER_MODE_CVARS, warp, waitStage, barrier, meetIn, connect } =
    require("../lib");

const HEART_PIECE = 0x21;
const HEART_CONTAINER = 0x22;
// Three hearts; a piece makes 16.
const BASE_LIFE = 15;
const FIELD_TBL = 6;

// Hyrule Field below Faron, its twilight gone: the heart piece in the tree (htPiece, item bit 0x81).
const FIELD = { stage: "F_SP121", room: 6, point: 0 };
const FIELD_START = { ...FIELD, eventBits: STORY.eldinGate.eventBits, levels: { transform: [0], darkClear: [0] } };
const HOUSE_START = { ...STAGES.linksHouse, eventBits: FIELD_START.eventBits, levels: FIELD_START.levels };
const TREE_BIT = 0x81;
// Kakariko Gorge, Eldin's twilight gone: chest 5 holds a heart piece.
const GORGE = { stage: "F_SP121", room: 3, point: 2 };
const GORGE_START = { ...GORGE, eventBits: STORY.lanayruGate.eventBits, levels: { transform: [0, 1], darkClear: [0, 1] } };
const GORGE_CHEST = 5;
// The Goron's heart piece after the springwater rush: Hyrule Field item bit 0x80, dropped at run time.
const SPRINGWATER = "goron_springwater_rush";
const SPRINGWATER_BIT = 0x80;
// Plumm's reward and the event bit its giver sets (F_286).
const PLUMM = "plumm_minigame_reward";
const PLUMM_BIT = 0x2380;
// Diababa's heart container: its check, the item bit fopAcM_createItemForBoss gives it, STAGE_LIFE.
const BOSS_CHECK = "boss:D_MN05A";
const BOSS_HEART_BIT = 0x9f;
const LIFE_BIT = 4;
const FOREST_TEMPLE = { stage: "D_MN05", room: 0, point: 0, eventBits: STORY.forestBoss.eventBits, levels: STORY.forestBoss.levels };
const DIABABA = { ...STORY.forestBoss, room: 50, point: 0, stageBoss: [] };

const life = (n, extra = {}) => ({ op: "expectInventory", maxLife: BASE_LIFE + n, ...extra });
const away = (s) => [warp(STAGES.linksHouse), waitStage(STAGES.linksHouse), warp(s), waitStage(s), { op: "wait", sec: 3 }];

// A randomizer game syncs once its unverified seed is confirmed.
const confirmRando = [
    { op: "expectLocalGame", kind: "randomizer", verified: false },
    { op: "expectSyncState", state: "unverified" },
    { op: "confirmUnverified" },
    { op: "expectSyncState", state: "ok" },
];

// B already in Diababa's arena, the boss alive; A took the team's heart container in the temple.
// B's own kill then leaves none (vanilla: STAGE_LIFE and bit 0x9F; randomizer: the check).
function bossScenario(name, description, rando, take, gain) {
    return {
        name,
        description,
        tags: ["boss-fight"],
        timeoutSec: 1200,
        cvars: rando ? [...COMMON_CVARS, ...RANDOMIZER_MODE_CVARS] : COMMON_CVARS,
        ...(rando ? { enableMods: [MODS.randomizer], rejectLog: [/conflict/i] } : {}),
        instances: [
            {
                name: "A",
                start: FOREST_TEMPLE,
                steps: [
                    ...connect,
                    waitStage(FOREST_TEMPLE, 120),
                    ...(rando ? confirmRando : []),
                    { op: "waitPeers", count: 1, timeoutSec: 120 },
                    ...barrier("fighting", "B", 400),
                    { op: "markMaxLife" },
                    take,
                    { op: "expectMaxLife", delta: gain },
                    ...barrier("took", "B"),
                    ...barrier("b-clear", "B", 900),
                    // B's own kill sends nothing back.
                    { op: "expectMaxLife", delta: gain, forSec: 10 },
                    { op: "expectPickups", deduped: 0 },
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: DIABABA,
                steps: [
                    ...connect,
                    waitStage(DIABABA, 120),
                    ...(rando ? confirmRando : []),
                    { op: "waitPeers", count: 1, timeoutSec: 120 },
                    { op: "keepLife" },
                    { op: "markMaxLife" },
                    { op: "waitBossFight", quietTicks: 90, timeoutSec: 240 },
                    ...barrier("fighting", "A", 400),
                    ...barrier("took", "A"),
                    { op: "expectMaxLife", delta: gain, timeoutSec: 30 },
                    ...(rando ? [] : [
                        { op: "expectDungeonBit", bit: LIFE_BIT, timeoutSec: 30 },
                        { op: "expectItemBit", bit: BOSS_HEART_BIT, timeoutSec: 30 },
                    ]),
                    { op: "killBoss", how: "real" },
                    { op: "expectBossClear", heart: 0, warp: 1, soft: rando, timeoutSec: 300 },
                    { op: "findFieldItem", bit: BOSS_HEART_BIT, present: false, forSec: 5 },
                    { op: "expectMaxLife", delta: gain, forSec: 5 },
                    { op: "expectPickups", deduped: 0, ...(rando ? { spawnsBlocked: 1 } : {}) },
                    ...barrier("b-clear", "A", 300),
                    { op: "quit" },
                ],
            },
        ],
    };
}

module.exports = [
    {
        name: "heart-live",
        description: "A takes Hyrule Field's tree heart piece (the stage's own, bit 0x81) while B stands in that room: B's copy vanishes, B gets one piece, and it is gone when B comes back",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: FIELD_START,
                steps: [
                    ...connect,
                    ...meetIn(FIELD, "B"),
                    { op: "setInventory", maxLife: BASE_LIFE },
                    { op: "findFieldItem", tag: "h", bit: TREE_BIT },
                    ...barrier("found", "B"),
                    { op: "takeFieldItem", tag: "h" },
                    { op: "expectItemBit", bit: TREE_BIT },
                    life(1),
                    ...barrier("taken", "B"),
                    life(1, { forSec: 5 }),
                    { op: "expectPickups", deduped: 0 },
                    ...barrier("done", "B", 180),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: FIELD_START,
                steps: [
                    ...connect,
                    ...meetIn(FIELD, "A"),
                    { op: "setInventory", maxLife: BASE_LIFE },
                    { op: "findFieldItem", tag: "h", bit: TREE_BIT },
                    ...barrier("found", "A"),
                    ...barrier("taken", "A"),
                    { op: "expectFieldItem", tag: "h", present: false, timeoutSec: 15 },
                    { op: "expectItemBit", bit: TREE_BIT },
                    life(1, { timeoutSec: 30 }),
                    life(1, { forSec: 5 }),
                    { op: "expectPickups", removed: 1, deduped: 0 },
                    ...away(FIELD),
                    { op: "findFieldItem", bit: TREE_BIT, present: false, forSec: 5 },
                    life(1, { forSec: 2 }),
                    ...barrier("done", "A", 180),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "heart-away",
        description: "A takes the tree heart piece while B is in Link's house: B gets one piece and the bit in its save slot, and the heart piece is not there when B arrives",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: FIELD_START,
                steps: [
                    ...connect,
                    waitStage(FIELD),
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    { op: "setInventory", maxLife: BASE_LIFE },
                    { op: "findFieldItem", tag: "h", bit: TREE_BIT },
                    ...barrier("ready", "B"),
                    { op: "takeFieldItem", tag: "h" },
                    { op: "expectItemBit", bit: TREE_BIT },
                    ...barrier("taken", "B"),
                    life(1, { forSec: 3 }),
                    ...barrier("done", "B", 180),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: HOUSE_START,
                steps: [
                    ...connect,
                    waitStage(STAGES.linksHouse),
                    { op: "setInventory", maxLife: BASE_LIFE },
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    ...barrier("ready", "A"),
                    ...barrier("taken", "A"),
                    { op: "expectItemBit", bit: TREE_BIT, tbl: FIELD_TBL, timeoutSec: 30 },
                    life(1, { timeoutSec: 30 }),
                    warp(FIELD),
                    waitStage(FIELD),
                    { op: "wait", sec: 3 },
                    { op: "findFieldItem", bit: TREE_BIT, present: false, forSec: 5 },
                    life(1, { forSec: 3 }),
                    { op: "expectPickups", deduped: 0 },
                    ...barrier("done", "A", 180),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "heart-drop",
        description: "the springwater Goron's heart piece, dropped at run time (Hyrule Field bit 0x80, check goron_springwater_rush): B's dropped copy goes when A takes A's, B's next drop is refused, one piece each",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: FIELD_START,
                steps: [
                    ...connect,
                    ...meetIn(FIELD, "B"),
                    { op: "setInventory", maxLife: BASE_LIFE },
                    { op: "spawnFieldItem", tag: "h", item: HEART_PIECE, bit: SPRINGWATER_BIT, check: SPRINGWATER },
                    { op: "expectFieldItem", tag: "h", timeoutSec: 10 },
                    ...barrier("dropped", "B"),
                    { op: "takeFieldItem", tag: "h" },
                    { op: "expectItemBit", bit: SPRINGWATER_BIT },
                    ...barrier("taken", "B"),
                    life(1, { forSec: 5 }),
                    { op: "expectPickups", deduped: 0 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: FIELD_START,
                steps: [
                    ...connect,
                    ...meetIn(FIELD, "A"),
                    { op: "setInventory", maxLife: BASE_LIFE },
                    { op: "spawnFieldItem", tag: "h", item: HEART_PIECE, bit: SPRINGWATER_BIT, check: SPRINGWATER },
                    { op: "expectFieldItem", tag: "h", timeoutSec: 10 },
                    ...barrier("dropped", "A"),
                    ...barrier("taken", "A"),
                    { op: "expectFieldItem", tag: "h", present: false, timeoutSec: 15 },
                    { op: "expectItemBit", bit: SPRINGWATER_BIT },
                    life(1, { timeoutSec: 30 }),
                    { op: "spawnFieldItem", tag: "h2", item: HEART_PIECE, bit: SPRINGWATER_BIT, check: SPRINGWATER, refused: true },
                    { op: "expectFieldItem", tag: "h2", present: false, forSec: 3 },
                    life(1, { forSec: 5 }),
                    { op: "expectPickups", removed: 1, deduped: 0 },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "heart-drop-overlap",
        description: "A and B each take their own drop of the springwater heart piece, A holding its item message while B's take finishes: the check comes with both grants, so each ends with exactly one piece",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: ["A", "B"].map((name) => {
            const other = name === "A" ? "B" : "A";
            return {
                name,
                start: FIELD_START,
                steps: [
                    ...connect,
                    ...meetIn(FIELD, other),
                    { op: "setInventory", maxLife: BASE_LIFE },
                    { op: "spawnFieldItem", tag: "h", item: HEART_PIECE, bit: SPRINGWATER_BIT, check: SPRINGWATER },
                    { op: "expectFieldItem", tag: "h", timeoutSec: 10 },
                    ...barrier("dropped", other),
                    // B takes its copy during A's 12 s.
                    ...(name === "A" ? [{ op: "takeFieldItem", tag: "h", holdSec: 12, timeoutSec: 60 }] :
                        [{ op: "wait", sec: 2 }, { op: "takeFieldItem", tag: "h" }]),
                    ...barrier("taken", other),
                    life(1, { forSec: 6 }),
                    { op: "expectPickups", deduped: 1 },
                    ...barrier("done", other),
                    { op: "quit" },
                ],
            };
        }),
    },
    {
        name: "heart-chest",
        description: "A opens Kakariko Gorge's heart piece chest (the stage's own chest 5) while B is in that room: B's chest opens by itself, B gets one piece, cannot open it again, and finds it open after coming back",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: GORGE_START,
                steps: [
                    ...connect,
                    ...meetIn(GORGE, "B"),
                    { op: "setInventory", maxLife: BASE_LIFE },
                    { op: "expectChestOpen", tbox: GORGE_CHEST, open: false },
                    ...barrier("ready", "B"),
                    { op: "openChest", tbox: GORGE_CHEST, approach: true, timeoutSec: 60 },
                    { op: "expectTbox", no: GORGE_CHEST },
                    life(1, { timeoutSec: 20 }),
                    ...barrier("opened", "B"),
                    life(1, { forSec: 5 }),
                    { op: "expectPickups", deduped: 0 },
                    ...barrier("done", "B", 180),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: GORGE_START,
                steps: [
                    ...connect,
                    ...meetIn(GORGE, "A"),
                    { op: "setInventory", maxLife: BASE_LIFE },
                    { op: "expectChestOpen", tbox: GORGE_CHEST, open: false },
                    ...barrier("ready", "A"),
                    ...barrier("opened", "A"),
                    { op: "expectChestOpen", tbox: GORGE_CHEST, timeoutSec: 20 },
                    life(1, { timeoutSec: 30 }),
                    { op: "openChest", tbox: GORGE_CHEST, approach: true, refused: true, frames: 90 },
                    life(1, { forSec: 5 }),
                    { op: "expectPickups", deduped: 0 },
                    ...away(GORGE),
                    { op: "expectChestOpen", tbox: GORGE_CHEST, forSec: 2 },
                    { op: "expectTbox", no: GORGE_CHEST },
                    ...barrier("done", "A", 180),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "heart-rando-reward",
        description: "randomizer game mode: A and B both win Plumm's reward check holding a heart piece at the same moment (its event bit and grant): each ends with exactly one piece",
        timeoutSec: 300,
        cvars: [...COMMON_CVARS, ...RANDOMIZER_MODE_CVARS],
        enableMods: [MODS.randomizer],
        rejectLog: [/conflict/i],
        instances: ["A", "B"].map((name) => ({
            name,
            steps: [
                ...connect,
                waitStage(STAGES.linksHouse),
                ...confirmRando,
                ...meetIn(STAGES.linksHouse, name === "A" ? "B" : "A"),
                { op: "setInventory", maxLife: BASE_LIFE },
                ...barrier("ready", name === "A" ? "B" : "A"),
                { op: "takeCheck", check: PLUMM, eventBit: PLUMM_BIT, item: HEART_PIECE },
                life(1, { timeoutSec: 20 }),
                ...barrier("taken", name === "A" ? "B" : "A"),
                life(1, { forSec: 8 }),
                { op: "expectEventBit", no: PLUMM_BIT },
                { op: "expectPickups", deduped: 1 },
                ...barrier("done", name === "A" ? "B" : "A"),
                { op: "quit" },
            ],
        })),
    },
    bossScenario(
        "heart-boss",
        "A takes Diababa's heart container in the temple (its check, bit 0x9F and the item's STAGE_LIFE) while B fights its own copy: B gets +5 once, and B's own kill leaves no heart container",
        false,
        { op: "takeCheck", check: BOSS_CHECK, itemBit: BOSS_HEART_BIT, item: HEART_CONTAINER },
        5),
    bossScenario(
        "heart-boss-rando",
        "randomizer game mode: A takes Diababa's check (a heart piece here) before its bit is set, while B fights its own copy: B's own kill places nothing for that check (spawnsBlocked 1) and B has A's piece once",
        true,
        { op: "takeCheck", check: BOSS_CHECK, item: HEART_PIECE },
        1),
];
