// Same-room cutscene scenarios built from data/cutscenes.json (tools/cutscenes/extract.js, docs/cutscenes.md).
// Both players start on the cutscene's synthetic checkpoint; A triggers it, B must watch the same event.

const fs = require("node:fs");
const path = require("node:path");
const { COMMON_CVARS, barrier, connect } = require("../lib");
const { scenarioName, batchName } = require("../../cutscenes/names");

const DATA = path.join(__dirname, "..", "data", "cutscenes.json");

const at = (stage) => ({ op: "waitStage", stage, timeoutSec: 120 });
const settle = { op: "waitStorySettled", pressA: true, timeoutSec: 300 };
const sameRoom = [{ op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 }, { op: "wait", sec: 2 }];

// Rough seconds per cutscene once both are in the room, for timeouts and the batch estimate.
const playSec = (e) => (e.mapType === "stb" ? 90 : e.source === "actor" ? 45 : 60);

const tagsOf = (e) => ["cutscene", `cutscene-${e.class}`, ...(e.mod.pullIn === "likely" ? ["cutscene-likely"] : [])];

// One cutscene with both players in its room: the trigger, both checks, both settled, back together.
function play(e, i, soft) {
    const t = `${e.id}#${i}`;
    const allow = e.test.allowNpc ? [{ op: "allowNpcEvent", ...e.test.allowNpc }] : [];
    const name = e.test.expectEvent;
    return {
        // The pause lets B's join check begin before A's STORY_EVENT can arrive.
        a: [...allow, ...barrier(`ready-${i}`, "B", 240), { op: "wait", sec: 1 }, e.test.trigger,
            { op: "expectEventName", name, timeoutSec: 90, soft, tag: t }, settle, ...barrier(`done-${i}`, "B", 420)],
        b: [...allow, ...barrier(`ready-${i}`, "A", 240),
            { op: "expectCutsceneJoin", name, timeoutSec: 90, soft, tag: t }, settle, ...barrier(`done-${i}`, "A", 420)],
    };
}

const goTo = (s) => [{ op: "warp", stage: s.stage, room: s.room, point: s.point, layer: s.layer ?? -1 }, at(s.stage), ...sameRoom];

function isolated(e) {
    const p = play(e, 0, false);
    const s = e.test.start;
    const desc = `${e.class} ${e.name} in ${s.stage} r${s.room}${s.layer !== undefined ? ` L${s.layer}` : ""}: B in the same room watches it too (expected today: ${e.mod.pullIn === "no" ? `missed, ${e.mod.refusal}` : e.mod.pullIn})`;
    return {
        name: scenarioName(e),
        description: desc,
        manualOnly: true,
        tags: tagsOf(e),
        timeoutSec: 240 + playSec(e) * 2,
        cvars: COMMON_CVARS,
        instances: ["A", "B"].map((n) => ({
            name: n,
            start: s,
            steps: [...connect, at(s.stage), ...sameRoom, ...(n === "A" ? p.a : p.b), ...sameRoom, { op: "quit" }],
        })),
    };
}

const superset = (a, b) => b.every((x) => a.includes(x));

// The flag steps from one checkpoint to the next, or null when a bit or level would have to go.
function flagSteps(from, to) {
    if ((from.hour ?? null) !== (to.hour ?? null) || !superset(to.eventBits, from.eventBits) ||
        !superset(to.levels.transform, from.levels.transform) || !superset(to.levels.darkClear, from.levels.darkClear)) {
        return null;
    }
    return [
        ...to.eventBits.filter((b) => !from.eventBits.includes(b)).map((no) => ({ op: "setEventBit", no })),
        ...to.levels.transform.filter((l) => !from.levels.transform.includes(l)).map((level) => ({ op: "setTransformLevel", level, set: true })),
        ...to.levels.darkClear.filter((l) => !from.levels.darkClear.includes(l)).map((level) => ({ op: "setDarkClear", level, set: true })),
    ];
}

// A stage's cutscenes in story order share one boot while their flags only grow (set before each
// warp); checks are soft (CUTSCENE PASS/FAIL lines in the log) so one miss does not end the batch.
function batches(tests, kind) {
    const out = [];
    let cur = null;
    // Stages in the order the story first reaches them, each stage's cutscenes in story order.
    const firstSeen = new Map();
    tests.forEach((e, i) => firstSeen.has(e.test.start.stage) || firstSeen.set(e.test.start.stage, i));
    const ordered = tests.map((e, i) => ({ e, i })).sort((x, y) =>
        firstSeen.get(x.e.test.start.stage) - firstSeen.get(y.e.test.start.stage) || x.i - y.i).map((x) => x.e);
    for (const e of ordered) {
        const s = e.test.start;
        const step = cur && cur.stage === s.stage && cur.items.length < 12 ? flagSteps(cur.last, s) : null;
        if (!step) {
            cur = { stage: s.stage, items: [], last: s };
            out.push(cur);
        }
        cur.items.push({ e, flags: step ?? [] });
        cur.last = s;
    }
    const seen = new Map();
    return out.map((g) => {
        const n = (seen.get(g.stage) ?? 0) + 1;
        seen.set(g.stage, n);
        const steps = { A: [], B: [] };
        g.items.forEach(({ e, flags }, i) => {
            const p = play(e, i, true);
            const move = i === 0 ? [] : [...flags, ...goTo(e.test.start)];
            steps.A.push(...move, ...p.a);
            steps.B.push(...move, ...p.b);
        });
        g.items = g.items.map((x) => x.e);
        const first = g.items[0].test.start;
        return {
            name: `${batchName(g.stage, kind)}-${n}`,
            description: `batch: ${g.items.length} ${kind} cutscene(s) of ${g.stage} on one boot (soft checks): ${g.items.map((e) => e.name).join(", ")}`,
            manualOnly: true,
            tags: ["cutscene-batch", `cutscene-batch-${kind}`, ...(g.items.some((e) => e.mod.pullIn === "likely") ? ["cutscene-batch-likely"] : [])],
            timeoutSec: 180 + g.items.reduce((sum, e) => sum + playSec(e) * 2 + 60, 0),
            cvars: COMMON_CVARS,
            instances: ["A", "B"].map((n) => ({
                name: n,
                start: first,
                steps: [...connect, at(first.stage), ...sameRoom, ...steps[n], ...sameRoom, { op: "quit" }],
            })),
        };
    });
}

function build() {
    if (!fs.existsSync(DATA)) return [];
    const catalog = JSON.parse(fs.readFileSync(DATA, "utf8"));
    // Catalog order: region, story milestone, stage, room.
    const tests = catalog.cutscenes.filter((e) => e.test && e.class !== "d");
    return [...tests.map(isolated), ...batches(tests.filter((e) => e.class !== "c"), "story"), ...batches(tests.filter((e) => e.class === "c"), "npc")];
}

module.exports = build();
