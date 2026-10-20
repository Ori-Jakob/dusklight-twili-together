// Smooth remote motion and hiding players in cutscenes.

const { STAGES, COMMON_CVARS, tt, warp, waitStage, barrier, meetIn, connect } = require("../lib");

// B measures its own motion, A measures B's dummy: one sender tick per local tick.
const smoothMotion = (name, description, cvars, serverEnv = {}) => ({
    name,
    description,
    serverEnv,
    timeoutSec: 300,
    cvars: [...COMMON_CVARS, ...cvars],
    instances: [
        {
            name: "A",
            start: STAGES.faronField,
            steps: [
                ...connect,
                ...meetIn(STAGES.faronField, "B"),
                ...barrier("walk", "B"),
                { op: "measureDummyMotion", frames: 240, settleFrames: 20, minSpeed: 3, maxBadRatio: 0.03 },
                ...barrier("measured", "B"),
                { op: "quit" },
            ],
        },
        {
            name: "B",
            start: STAGES.faronField,
            steps: [
                ...connect,
                ...meetIn(STAGES.faronField, "A"),
                ...barrier("walk", "A"),
                { op: "walk", frames: 300, circle: true, async: true },
                { op: "measureDummyMotion", self: true, frames: 240, settleFrames: 20, minSpeed: 3 },
                ...barrier("measured", "A"),
                { op: "quit" },
            ],
        },
    ],
});

// A personal setting: each instance's own value decides what it sees.
const hideInCutscenes = [...COMMON_CVARS, tt("hide_players_in_cutscene", 1)];

// The body's climb rate may change sharply on few ticks, for our Link and for the dummy alike.
const steadyBody = { frames: 240, settleFrames: 20, jerk: 3, maxJerkRatio: 0.05 };

// B stands across a slope (and falls from over it); A's dummy of B plants both feet like B's Link.
// A standing wolf, B's own too, keeps a paw up to about 8 over its ground at times.
const feetOnSlope = (name, description, stage, wolf) => ({
    name,
    description,
    timeoutSec: 300,
    cvars: COMMON_CVARS,
    instances: [
        {
            name: "A",
            start: stage,
            steps: [
                ...connect,
                ...meetIn(stage, "B"),
                { op: "waitSignal", name: "on-slope", from: "B", timeoutSec: 90 },
                { op: "expectFeetOnGround", maxSpread: wolf ? 10 : 2, minBend: 3, frames: 30 },
                ...barrier("feet-checked", "B"),
                { op: "expectDummyAirborne", timeoutSec: 5 },
                ...barrier("fell", "B"),
                { op: "quit" },
            ],
        },
        {
            name: "B",
            start: stage,
            steps: [
                ...connect,
                ...meetIn(stage, "A"),
                ...(wolf ? [{ op: "transform", form: "wolf", timeoutSec: 30 }] : []),
                { op: "findSlope", minDeg: 12, maxDeg: 30 },
                { op: "wait", frames: 30 },
                // Our own Link on the same spot is the yardstick for the check on A
                { op: "expectFeetOnGround", self: true, maxSpread: wolf ? 10 : 2, minBend: 3, frames: 30 },
                { op: "signal", name: "on-slope" },
                ...barrier("feet-checked", "A"),
                { op: "teleportSelf", dy: 250 },
                { op: "wait", frames: 60 },
                ...barrier("fell", "A"),
                { op: "quit" },
            ],
        },
    ],
});

module.exports = [
    {
        name: "presence-showcase",
        description: "two players stand near each other in Faron Field for a screenshot (manual)",
        manualOnly: true,
        timeoutSec: 240,
        cvars: COMMON_CVARS,
        instances: ["A", "B"].map((name, i, names) => ({
            name,
            start: STAGES.faronField,
            steps: [
                ...connect,
                ...meetIn(STAGES.faronField, names[1 - i]),
                { op: "log", msg: "showcase ready" },
                { op: "wait", sec: 20 },
                { op: "quit" },
            ],
        })),
    },
    {
        name: "pose-selftest",
        description: "interpolation buffer and cutscene classifier unit checks",
        timeoutSec: 120,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "solo",
                steps: [waitStage(STAGES.linksHouse), { op: "poseSelfTest" }, { op: "quit" }],
            },
        ],
    },
    // With frame interpolation on, a frame runs zero to two ticks.
    smoothMotion("smooth-motion", "a remote walking in circles moves every tick, with mixed frame pacing",
        ["game.enableFrameInterpolation=2"]),
    smoothMotion("smooth-motion-clean", "a remote walking in circles moves every tick, one tick per frame",
        ["game.enableFrameInterpolation=0"]),
    // The relay delays every packet by 0-90 ms (order kept): the playout delay must absorb it.
    smoothMotion("smooth-motion-jitter", "a remote walking in circles moves every tick with 0-90 ms network jitter",
        [], { TT_TEST_JITTER_MS: "90" }),
    {
        name: "puppet-ground-steady",
        description: "B walks in circles in Faron Field and then on a slope in South Faron: the height of A's dummy of B changes as smoothly as B's own body (no hop at every step or slope change)",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.faronField,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.faronField, "B"),
                    ...barrier("walk", "B"),
                    { op: "measureBodyHeight", ...steadyBody },
                    // The field is flat around here: the slope is in South Faron
                    warp(STAGES.southFaron),
                    ...meetIn(STAGES.southFaron, "B"),
                    ...barrier("slope", "B"),
                    { op: "measureBodyHeight", ...steadyBody },
                    ...barrier("measured", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.faronField,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.faronField, "A"),
                    ...barrier("walk", "A"),
                    { op: "walk", frames: 300, circle: true, async: true },
                    { op: "measureBodyHeight", self: true, ...steadyBody },
                    { op: "wait", frames: 60 },
                    warp(STAGES.southFaron),
                    ...meetIn(STAGES.southFaron, "A"),
                    { op: "findSlope", minDeg: 10, maxDeg: 30 },
                    { op: "wait", frames: 20 },
                    ...barrier("slope", "A"),
                    { op: "walk", frames: 300, circle: true, async: true },
                    { op: "measureBodyHeight", self: true, ...steadyBody },
                    ...barrier("measured", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    feetOnSlope("puppet-feet-slope",
        "B stands across a slope in South Faron: A's dummy of B bends its legs and drops to the lower foot like B's Link, both feet on A's ground; in B's fall the dummy's legs let go",
        STAGES.southFaron, false),
    feetOnSlope("puppet-feet-slope-wolf",
        "the same for B as a wolf in South Faron: all four paws of A's dummy on the slope",
        STAGES.southFaron, true),
    {
        name: "teleport-snap",
        description: "a peer that jumps 1600 units shows up there at once instead of sliding",
        timeoutSec: 240,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.faronField,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.faronField, "B"),
                    ...barrier("arm", "B"),
                    // B stands still until it jumps: a slide would show up as a partial step.
                    { op: "expectDummyTeleport", minDist: 1500, maxPreStep: 50, timeoutSec: 20 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.faronField,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.faronField, "A"),
                    ...barrier("arm", "A"),
                    { op: "wait", frames: 30 },
                    // Straight up: open sky is above the start point, and the fall is ordinary movement.
                    { op: "teleportSelf", dy: 1600 },
                    { op: "wait", frames: 60 },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "cutscene-hide",
        description: "hide_players_in_cutscene: a local cutscene hides everyone, a remote in a cutscene is hidden, a compulsory event hides nobody",
        timeoutSec: 300,
        cvars: hideInCutscenes,
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    ...barrier("room-ready", "B"),
                    ...barrier("b-in-cutscene", "B"),
                    // B's cutscene reaches us as its PLAYER_UPDATE flag and hides its dummy.
                    { op: "expectPeerFlag", flag: "inCutscene", set: true, timeoutSec: 10 },
                    { op: "expectDummyHidden", hidden: true, timeoutSec: 10 },
                    ...barrier("a-saw-hidden", "B"),
                    ...barrier("b-out", "B"),
                    { op: "expectDummyHidden", hidden: false, timeoutSec: 10 },
                    // A player-ordered event (like drinking from a bottle) is no cutscene.
                    { op: "beginEvent", kind: "compulsory", timeoutSec: 10 },
                    ...barrier("a-compulsory", "B"),
                    { op: "expectDummyHidden", hidden: false },
                    { op: "endEvent", timeoutSec: 10 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "A"),
                    ...barrier("room-ready", "A"),
                    // Our own cutscene hides A's dummy here.
                    { op: "forceCutscene", on: true },
                    { op: "expectDummyHidden", hidden: true, timeoutSec: 5 },
                    ...barrier("b-in-cutscene", "A"),
                    ...barrier("a-saw-hidden", "A"),
                    // Shown again once the release delay after the cutscene ran out.
                    { op: "forceCutscene", on: null },
                    { op: "wait", frames: 15 },
                    { op: "expectDummyHidden", hidden: false, timeoutSec: 5 },
                    ...barrier("b-out", "A"),
                    ...barrier("a-compulsory", "A"),
                    { op: "expectPeerFlag", flag: "inCutscene", set: false },
                    { op: "expectDummyHidden", hidden: false },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "cutscene-hide-personal",
        description: "Hide Players in Cutscenes is personal: A has it on, B off; A's cutscene and B's both hide B's dummy on A, while B sees A's dummy through both",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                cvars: [tt("hide_players_in_cutscene", 1)],
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "expectDummyHidden", hidden: false, timeoutSec: 10 },
                    ...barrier("ready", "B"),
                    // Our cutscene hides B here.
                    { op: "forceCutscene", on: true },
                    { op: "expectDummyHidden", hidden: true, timeoutSec: 5 },
                    ...barrier("a-in-cutscene", "B"),
                    ...barrier("b-saw-a", "B"),
                    { op: "forceCutscene", on: null },
                    { op: "wait", frames: 15 },
                    { op: "expectDummyHidden", hidden: false, timeoutSec: 5 },
                    ...barrier("a-out", "B"),
                    // B's cutscene hides B here too: our setting, not B's.
                    ...barrier("b-in-cutscene", "B"),
                    { op: "expectPeerFlag", flag: "inCutscene", set: true, timeoutSec: 10 },
                    { op: "expectDummyHidden", hidden: true, timeoutSec: 10 },
                    ...barrier("a-saw-b", "B"),
                    ...barrier("b-out", "B"),
                    { op: "expectDummyHidden", hidden: false, timeoutSec: 10 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                cvars: [tt("hide_players_in_cutscene", 0)],
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "A"),
                    { op: "expectDummyHidden", hidden: false, timeoutSec: 10 },
                    ...barrier("ready", "A"),
                    ...barrier("a-in-cutscene", "A"),
                    // A is in a cutscene and A hides players, but we do not: A stays shown.
                    { op: "expectPeerFlag", flag: "inCutscene", set: true, timeoutSec: 10 },
                    { op: "wait", frames: 30 },
                    { op: "expectDummyHidden", hidden: false },
                    ...barrier("b-saw-a", "A"),
                    ...barrier("a-out", "A"),
                    { op: "forceCutscene", on: true },
                    { op: "wait", frames: 30 },
                    { op: "expectDummyHidden", hidden: false },
                    ...barrier("b-in-cutscene", "A"),
                    ...barrier("a-saw-b", "A"),
                    { op: "expectDummyHidden", hidden: false },
                    { op: "forceCutscene", on: null },
                    ...barrier("b-out", "A"),
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "puppet-root-matrix",
        description: "B slashes its sword: A's dummy of B puts its root where B's calc put it (not the standing pin); then B's updates carry a ladder-style root, a magnetic-boots ceiling, a horse run tweak and a climbing wall's turn, and A's dummy matrix and root follow each exactly",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    { op: "expectDummyBody", rootMatch: true, minTicks: 30, timeoutSec: 20 },
                    ...barrier("slash", "B"),
                    // procCutNormal clears the root's Z to l_halfAtnWaitBaseAnime's -7.
                    { op: "expectDummyBody", rootZ: -7, rootMatch: true, notStanding: true, minTicks: 8, timeoutSec: 30 },
                    ...barrier("slashed", "B"),
                    ...barrier("root", "B"),
                    { op: "expectDummyBody", rootX: 12, rootZ: -20, rootMatch: true, minTicks: 15, timeoutSec: 20 },
                    ...barrier("saw-root", "B"),
                    ...barrier("magne", "B"),
                    { op: "expectDummyBody", magne: true, up: [0, -1, 0], turnDeg: 180, minTicks: 15, timeoutSec: 20 },
                    ...barrier("saw-magne", "B"),
                    ...barrier("tweak", "B"),
                    { op: "expectDummyBody", tweak: 2, tweakA: 3000, tweakB: 4000, minTicks: 15, timeoutSec: 20 },
                    ...barrier("saw-tweak", "B"),
                    ...barrier("climb", "B"),
                    { op: "expectDummyBody", turnDeg: 45, minTicks: 15, timeoutSec: 20 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    ...barrier("slash", "A"),
                    // Five B presses: plain slashes, no lock-on
                    ...[0, 1, 2, 3, 4].flatMap(() => [
                        { op: "walk", frames: 2, stickX: 0, stickY: 0, buttons: 0x200 },
                        { op: "walk", frames: 14, stickX: 0, stickY: 0 },
                    ]),
                    ...barrier("slashed", "A"),
                    { op: "wait", frames: 30 },
                    // "ym": flags, root X and Z (x8), tweak angles, magnet tilt and direction, climb yaw
                    { op: "patchPlayerUpdate", patch: { ym: [1 | 2 | 4, 96, -160, 0, 0, 0, 0, 0] }, packets: 200 },
                    ...barrier("root", "A"),
                    ...barrier("saw-root", "A"),
                    { op: "patchPlayerUpdate", patch: { ym: [1 | 8, 0, 0, 0, 0, -32768, 0, 0] }, packets: 200 },
                    ...barrier("magne", "A"),
                    ...barrier("saw-magne", "A"),
                    { op: "patchPlayerUpdate", patch: { ym: [1 | (2 << 4), 0, 0, 3000, 4000, 0, 0, 0] }, packets: 200 },
                    ...barrier("tweak", "A"),
                    ...barrier("saw-tweak", "A"),
                    { op: "patchPlayerUpdate", patch: { ym: [1, 0, 0, 0, 0, 0, 0, 0x2000] }, packets: 200 },
                    ...barrier("climb", "A"),
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "puppet-no-push",
        description: "A stands right against B's dummy (their colliders overlap), with PvP off and then on: A's own Link stays put, no push from the dummy, and with PvP on the dummy's hurtbox is still registered",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: ["A", "B"].map((name) => ({
            name,
            start: STAGES.faronField,
            steps: [
                ...connect,
                ...meetIn(STAGES.faronField, name === "A" ? "B" : "A"),
                { op: "clearEnemies", radius: 0, keep: true },
                ...barrier("still", name === "A" ? "B" : "A"),
                ...(name === "A"
                    ? [
                        { op: "approachDummy", dist: 40 },
                        { op: "measureSelfDrift", frames: 150, maxDrift: 3 },
                    ]
                    : [{ op: "wait", frames: 220 }]),
                ...barrier("pvp-off-done", name === "A" ? "B" : "A"),
                { op: "setRoomOption", name: "pvpMode", value: true },
                { op: "waitRoomOption", name: "pvpMode", value: true, timeoutSec: 20 },
                ...barrier("pvp-on", name === "A" ? "B" : "A"),
                ...(name === "A"
                    ? [
                        { op: "expectDummyHurtbox", registered: true, timeoutSec: 10 },
                        { op: "approachDummy", dist: 40 },
                        { op: "measureSelfDrift", frames: 150, maxDrift: 3 },
                        { op: "expectDummyHurtbox", registered: true, timeoutSec: 5 },
                    ]
                    : [{ op: "wait", frames: 220 }]),
                ...barrier("done", name === "A" ? "B" : "A"),
                { op: "quit" },
            ],
        })),
    },
];
