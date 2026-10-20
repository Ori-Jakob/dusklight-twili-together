// Block pushes and chain pulls (syncPushPull): a teammate in the room sees them move live and stop on
// the same spot; a chain's switch comes only from the puller's game. Spots from the disc's room data.

const { STORY, COMMON_CVARS, warp, waitStage, barrier, connect } = require("../lib");

// Goron Mines room 6: a plain push block (osiBLK0, no saved spot), 75 a step.
const MINES = { stage: "D_MN04", room: 6, point: 0 };
const MINES_BLOCK = [-1480, -3425];
// Death Mountain room 3: a block on path 13 saves its spot in memory switches 0x3a and 0x50; two
// 50-unit pushes (dir 3, toward -z in the world) reach the path's second point and set 0x3a.
const DEATH_MTN = { ...STORY.kakariko, stage: "F_SP110", room: 3, point: 4 };
const KAKARIKO = STORY.kakariko;
const DM_BLOCK = [-3400, -4550];
const DM_BLOCK_HALF = [-3400, -4600];
const DM_BLOCK_SAVED = [-3400, -4650];
const DM_SWITCH = 0x3a;
// Snowpeak Ruins room 3: an ice block of the courtyard puzzle (saved along path 0).
const SNOWPEAK = { stage: "D_MN11", room: 3, point: 0 };
const ICE_BLOCK = [4900, -3100];
// City in the Sky room 13: a nine-link chain switch (switch 0x54) that opens a shutter. Arbiter's
// Grounds' entrance chain hangs over sand that voids Link out.
const CITY = { stage: "D_MN09", room: 13, point: 0 };
const CHAIN = [-12200, -3300];
const CHAIN_SWITCH = 0x54;
const CHAIN_OUT = 4;

const room = (name, value) => [
    { op: "setRoomOption", name, value },
    { op: "waitRoomOption", name, value, timeoutSec: 20 },
];
// Without meetIn's walk: it could take Link off a ledge in a dungeon room.
const meet = (s, other) => [
    waitStage(s),
    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 90 },
    { op: "waitDummies", count: 1, timeoutSec: 60 },
    { op: "clearEnemies", radius: 0, keep: true },
    ...barrier(`met-${s.stage}-${s.room}`, other),
];
const block = (near, fields = {}) => ({ op: "expectBlock", near, ...fields });
const settled = (near, kind = "movebox") => block(near, { kind, stillFrames: 30, timeoutSec: 30 });
// Each instance's block ends where the other's did.
const samePlace = (near, other, kind = "movebox") => [
    settled(near, kind),
    ...barrier("settled", other),
    { op: "signalBlockPos", kind, near, name: "blk" },
    { op: "expectBlockPosSignal", kind, near, name: "blk", from: other, timeoutSec: 30 },
];
const duo = (name, description, start, a, b, extra = {}) => ({
    name,
    description,
    timeoutSec: 300,
    cvars: COMMON_CVARS,
    ...extra,
    instances: [
        { name: "A", start, steps: [...connect, ...a, { op: "dumpPush" }, ...barrier("done", "B"), { op: "quit" }] },
        { name: "B", start, steps: [...connect, ...b, { op: "dumpPush" }, ...barrier("done", "A"), { op: "quit" }] },
    ],
});

module.exports = [
    duo("blocks-push", "A pushes a Goron Mines block: B's copy replays the push live and stops on A's spot", MINES,
        [
            ...meet(MINES, "B"),
            block(MINES_BLOCK, { moved: false }),
            ...barrier("ready", "B"),
            { op: "pushBlock", near: MINES_BLOCK },
            ...samePlace(MINES_BLOCK, "B"),
            block(MINES_BLOCK, { moved: true, maxApplied: 0 }),
        ],
        [
            ...meet(MINES, "A"),
            block(MINES_BLOCK, { moved: false }),
            ...barrier("ready", "A"),
            block(MINES_BLOCK, { moving: true, minApplied: 1, timeoutSec: 20 }),
            ...samePlace(MINES_BLOCK, "A"),
            block(MINES_BLOCK, { moved: true, matchesRemote: true, minSnapped: 0 }),
        ]),
    duo("blocks-off", "with Sync Blocks and Chains off, A's push moves only A's block: B's stays put",
        MINES,
        [
            ...meet(MINES, "B"),
            ...room("syncPushPull", false),
            ...barrier("off", "B"),
            { op: "pushBlock", near: MINES_BLOCK },
            settled(MINES_BLOCK),
            block(MINES_BLOCK, { moved: true }),
            ...barrier("pushed", "B"),
            ...barrier("b-checked", "B"),
        ],
        [
            ...meet(MINES, "A"),
            ...room("syncPushPull", false),
            ...barrier("off", "A"),
            ...barrier("pushed", "A"),
            block(MINES_BLOCK, { moved: false, stillFrames: 90, maxApplied: 0 }),
            ...barrier("b-checked", "A"),
        ]),
    duo("blocks-both", "A and B push the same block at once from opposite sides: both end on the lower client id's spot",
        MINES,
        [
            ...meet(MINES, "B"),
            ...barrier("go", "B"),
            { op: "pushBlock", near: MINES_BLOCK, firstDir: 0 },
            ...samePlace(MINES_BLOCK, "B"),
            block(MINES_BLOCK, { moved: true }),
        ],
        [
            ...meet(MINES, "A"),
            ...barrier("go", "A"),
            { op: "pushBlock", near: MINES_BLOCK, firstDir: 2 },
            ...samePlace(MINES_BLOCK, "A"),
            block(MINES_BLOCK, { moved: true }),
        ]),
    {
        name: "blocks-pathed-late",
        description: "A pushes a Death Mountain block twice: B there sees the first push live; B leaves, A's second push saves the spot in a switch, and B coming back finds the block there without a replay",
        timeoutSec: 420,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: DEATH_MTN,
                steps: [
                    ...connect,
                    ...meet(DEATH_MTN, "B"),
                    block(DM_BLOCK, { at: DM_BLOCK }),
                    { op: "pushBlock", near: DM_BLOCK, dir: 3 },
                    settled(DM_BLOCK),
                    block(DM_BLOCK, { at: DM_BLOCK_HALF }),
                    { op: "expectSwitch", no: DM_SWITCH, set: false },
                    ...barrier("first", "B"),
                    ...barrier("b-away", "B"),
                    { op: "pushBlock", near: DM_BLOCK, dir: 3 },
                    settled(DM_BLOCK),
                    block(DM_BLOCK, { at: DM_BLOCK_SAVED }),
                    { op: "expectSwitch", no: DM_SWITCH, set: true, timeoutSec: 10 },
                    ...barrier("second", "B"),
                    ...barrier("b-back", "B"),
                    { op: "dumpPush" },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: DEATH_MTN,
                steps: [
                    ...connect,
                    ...meet(DEATH_MTN, "A"),
                    block(DM_BLOCK, { minApplied: 1, timeoutSec: 30 }),
                    settled(DM_BLOCK),
                    block(DM_BLOCK, { at: DM_BLOCK_HALF, matchesRemote: true }),
                    ...barrier("first", "A"),
                    warp(KAKARIKO),
                    waitStage(KAKARIKO),
                    ...barrier("b-away", "A"),
                    ...barrier("second", "A"),
                    warp(DEATH_MTN),
                    waitStage(DEATH_MTN),
                    // The switch put it there when the room loaded: no live replay.
                    block(DM_BLOCK, { at: DM_BLOCK_SAVED, stillFrames: 30, maxApplied: 0 }),
                    { op: "expectSwitch", no: DM_SWITCH, set: true },
                    ...barrier("b-back", "A"),
                    { op: "dumpPush" },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    duo("blocks-ice", "A pushes a Snowpeak ice block: B's copy slides with it and stops on A's spot", SNOWPEAK,
        [
            ...meet(SNOWPEAK, "B"),
            ...barrier("ready", "B"),
            { op: "pushBlock", kind: "iceblock", near: ICE_BLOCK },
            ...samePlace(ICE_BLOCK, "B", "iceblock"),
            block(ICE_BLOCK, { kind: "iceblock", moved: true }),
        ],
        [
            ...meet(SNOWPEAK, "A"),
            ...barrier("ready", "A"),
            block(ICE_BLOCK, { kind: "iceblock", minApplied: 1, timeoutSec: 20 }),
            ...samePlace(ICE_BLOCK, "A", "iceblock"),
            block(ICE_BLOCK, { kind: "iceblock", moved: true, matchesRemote: true, tol: 10 }),
        ]),
    duo("blocks-chain", "A pulls a City in the Sky chain switch: B's chain follows A's puppet, B gets the switch from A's game, and the chain lets go with A",
        CITY,
        [
            ...meet(CITY, "B"),
            ...barrier("ready", "B"),
            { op: "approachChain", near: CHAIN },
            // The stick alone pulls at about 6, under the 10 the switch takes without iron boots.
            { op: "pullChain", near: CHAIN, untilSwitch: true, pullPower: 12, timeoutSec: 60 },
            { op: "expectSwitch", no: CHAIN_SWITCH, set: true },
            ...barrier("pulled", "B"),
            ...barrier("b-saw", "B"),
            { op: "releaseChain" },
            { op: "expectChain", near: CHAIN, held: false },
            ...barrier("released", "B"),
        ],
        [
            ...meet(CITY, "A"),
            ...barrier("ready", "A"),
            { op: "expectChain", near: CHAIN, driven: true, timeoutSec: 30 },
            ...barrier("pulled", "A"),
            { op: "expectChain", near: CHAIN, driven: true, minOut: CHAIN_OUT + 1 },
            { op: "expectChainAtPuppet", near: CHAIN, maxDist: 150 },
            { op: "expectSwitch", no: CHAIN_SWITCH, set: true, timeoutSec: 10 },
            ...barrier("b-saw", "A"),
            ...barrier("released", "A"),
            { op: "expectChain", near: CHAIN, driven: false, timeoutSec: 10 },
            { op: "expectSwitch", no: CHAIN_SWITCH, set: true },
        ]),
];
