#!/usr/bin/env node
// Maps every cutscene of the game from the disc image and the decomp; output described in docs/cutscenes.md.
//   node tools/cutscenes/extract.js [--iso <image>] [--decomp <fork root>] [--out <json>] [--docs <md>]

const fs = require("node:fs");
const path = require("node:path");
const { Disc, rarc, arcFile } = require("./gc");
const { parseDzx, parseEventList } = require("./stage");
const decomp = require("./decomp");
const { layerNo, milestoneFlags, twilightLevel, MILESTONES } = require("./layers");
const { renderDocs } = require("./docs");

const MOD = path.resolve(__dirname, "..", "..");
const hex = (v, w = 4) => `0x${v.toString(16).padStart(w, "0")}`;
const u16 = (v) => v & 0xffff;
const NONE = 0xff;

function parseArgs(argv) {
    const o = {
        iso: null,
        decomp: path.resolve(MOD, "..", ".."),
        out: path.join(MOD, "tools", "autotest", "data", "cutscenes.json"),
        docs: path.join(MOD, "docs", "cutscenes.md"),
    };
    for (let i = 0; i < argv.length; i++) {
        const k = argv[i].replace(/^--/, "");
        if (!(k in o)) throw new Error(`unknown option ${argv[i]}`);
        o[k] = path.resolve(argv[++i]);
    }
    // Same source as tools/autotest/run.js: the disc image from the user's config, read only.
    if (!o.iso) {
        const cfg = path.join(process.env.APPDATA ?? "", "TwilitRealm", "Dusklight", "config.json");
        o.iso = JSON.parse(fs.readFileSync(cfg, "utf8"))["backend.isoPath"];
    }
    return o;
}

// The milestone the story first reaches a stage at (an estimate of play order, not game data).
const FIRST_VISIT = {
    F_SP103: "new-game", F_SP00: "new-game", F_SP104: "new-game", R_SP01: "new-game", F_SP108: "new-game",
    R_SP108: "new-game", D_SB10: "day1", R_SP107: "captured", R_SP301: "captured", D_MN05: "faron-light",
    D_MN05A: "faron-light", D_MN05B: "faron-light", F_SP109: "eldin-twilight", F_SP110: "eldin-twilight",
    F_SP111: "eldin-twilight", R_SP109: "eldin-twilight", R_SP209: "eldin-twilight", R_SP110: "eldin-twilight",
    F_SP102: "epona", D_MN04: "kb1-defeated", D_MN04A: "kb1-defeated", D_MN04B: "kb1-defeated",
    F_SP112: "lanayru-twilight", F_SP113: "lanayru-twilight", F_SP115: "lanayru-twilight", F_SP116: "lanayru-twilight",
    F_SP122: "lanayru-twilight", F_SP126: "lanayru-twilight", R_SP116: "lanayru-twilight", R_SP160: "lanayru-twilight",
    R_SP161: "lanayru-twilight", F_SP123: "lanayru-light", F_SP127: "lanayru-light", R_SP127: "lanayru-light",
    D_MN01: "zora-armor", D_MN01A: "zora-armor", D_MN01B: "zora-armor", F_SP117: "mdh-done", F_SP118: "desert",
    F_SP124: "desert", F_SP125: "desert", D_MN10: "tent", D_MN10A: "tent", D_MN10B: "tent", F_SP114: "arbiter",
    D_MN11: "arbiter", D_MN11A: "arbiter", D_MN11B: "arbiter", D_SB00: "arbiter", D_SB01: "arbiter",
    D_MN06: "snowpeak", D_MN06A: "snowpeak", D_MN06B: "snowpeak", F_SP128: "louise", R_SP128: "louise",
    D_MN07: "cannon-fixed", D_MN07A: "cannon-fixed", D_MN07B: "cannon-fixed", D_MN08: "mirror", D_MN08A: "mirror",
    D_MN08B: "mirror", D_MN08C: "mirror", D_MN08D: "mirror", D_MN09: "zant", D_MN09A: "zant", D_MN09B: "zant",
    D_MN09C: "zant", R_SP300: "zant",
};
function firstVisit(stage, room) {
    if (stage === "F_SP121") {
        if (room === 0 || (room >= 2 && room <= 5) || room === 7) return "eldin-twilight";
        if (room >= 9 && room <= 14) return "lanayru-twilight";
        return "faron-light";
    }
    return FIRST_VISIT[stage] ?? (/^D_SB/.test(stage) ? "master-sword" : "new-game");
}

// ---- mod rules read from the mod's own sources ----

function modRules() {
    const moves = fs.readFileSync(path.join(MOD, "src/story/StoryMoves.cpp"), "utf8");
    const npc = fs.readFileSync(path.join(MOD, "src/story/StoryNpc.cpp"), "utf8");
    const block = (text, name) => text.slice(text.indexOf(name), text.indexOf("};", text.indexOf(name)));
    const str = (s) => (s.trim() === "nullptr" ? null : s.trim().replace(/"/g, ""));
    const curated = [];
    const re = /\{("[\w-]+"),\s*"[^"]*",\s*"[^"]*",\s*([^,]+),\s*(-?\d+),\s*([^,]+),\s*(-?\d+),\s*([^,]+),\s*(-?\d+),\s*Form::(\w+),\s*\w+,\s*\w+(?:,\s*(-?\d+))?(?:,\s*([-\w]+))?(?:,\s*(0x[0-9A-Fa-f]+|\d+))?\s*\}/g;
    for (const m of block(moves, "kStoryMoves[]").matchAll(re)) {
        curated.push({
            id: str(m[1]), fromStage: str(m[2]), fromRoom: +m[3], toStage: str(m[4]), toRoom: +m[5], eventPrefix: str(m[6]),
            followPoint: +m[7], form: m[8], toPoint: m[9] !== undefined ? +m[9] : -1,
            requester: m[10] && m[10] !== "-1" ? m[10] : null, bit: m[11] ? parseInt(m[11]) : 0,
        });
    }
    const sidePoints = [...block(moves, "kSideEffectPoints[]").matchAll(/\{"(\w+)",\s*(\d+),\s*(\d+)\}/g)].map((m) => `${m[1]}/${m[2]}/${m[3]}`);
    const npcT = new Set([...block(npc, "kNpcTProfiles[]").matchAll(/fpcNm_\w+/g)].map((m) => m[0]));
    const allowlist = block(npc, "kNpcJoinable").match(/std::array<Allowed,\s*(\d+)>/);
    const notStory = new Set([...block(moves, "kNotStoryRequesters[]").matchAll(/fpcNm_\w+/g)].map((m) => m[0]));
    const bossStages = new Set([...block(moves, "kBossStages[]").matchAll(/\{"(\w+)"/g)].map((m) => m[1]));
    return { curated, sidePoints: new Set(sidePoints), npcT, npcAllowlist: allowlist ? +allowlist[1] : 0, notStory, bossStages };
}

function profileValues(root) {
    const text = fs.readFileSync(path.join(root, "include/f_pc/f_pc_name.h"), "utf8");
    const map = new Map();
    for (const m of text.matchAll(/\/\* 0x([0-9A-Fa-f]+) \*\/ X\((fpcNm_\w+)\)/g)) if (!map.has(m[2])) map.set(m[2], parseInt(m[1], 16));
    return map;
}

// ---- disc ----

function loadDisc(disc) {
    const stages = new Map();
    const stb = new Map();
    const addStb = (arc, where) => {
        for (const k of arc.keys()) {
            const m = k.match(/([\w$]+)\.stb$/);
            if (m) {
                if (!stb.has(m[1])) stb.set(m[1], []);
                stb.get(m[1]).push(where);
            }
        }
    };
    for (const f of disc.files.keys()) {
        const m = f.match(/^res\/Stage\/(\w+)\/(?:STG_00|R(\d+)_00)\.arc$/);
        if (!m) continue;
        const [, name, roomStr] = m;
        if (!stages.has(name)) stages.set(name, { name, stage: null, rooms: new Map() });
        const arc = rarc(disc.file(f));
        const dz = arcFile(arc, roomStr === undefined ? "stage.dzs" : "room.dzr");
        const entry = { dzx: parseDzx(dz ?? Buffer.alloc(4)), events: parseEventList(arcFile(arc, "event_list.dat")) };
        if (roomStr === undefined) stages.get(name).stage = entry;
        else stages.get(name).rooms.set(+roomStr, entry);
        addStb(arc, { arc: path.basename(f, ".arc"), stage: name, room: roomStr === undefined ? -1 : +roomStr });
    }
    const objectEvents = new Map();
    let keep = [];
    for (const f of disc.list("res/Object/")) {
        if (!f.endsWith(".arc")) continue;
        let arc;
        try {
            arc = rarc(disc.file(f));
        } catch {
            continue;
        }
        const name = path.basename(f, ".arc");
        const demo = name.match(/^Demo(\d+)_(\d+)$/);
        if (demo) addStb(arc, { arc: name, bank: +demo[1], bank2: +demo[2] });
        const ev = parseEventList(arcFile(arc, "event_list.dat"));
        if (name === "Event") keep = ev;
        else if (ev.length) objectEvents.set(name, ev);
    }
    return { stages, stb, objectEvents, keep };
}

// ---- event metadata (names and kinds only, no game text) ----

const SYSTEM_STAFF = ["ALL", "CAMERA", "DIRECTOR", "MESSAGE", "TIMEKEEP", "PACKAGE", "SOUND", "LIGHT", "EFFECT", "CREATE"];

function summarize(ev) {
    if (!ev) return null;
    const cuts = (type) => ev.staff.filter((s) => s.type === type).flatMap((s) => s.cuts);
    const names = (type) => [...new Set(cuts(type).map((c) => c.name))];
    const stb = ev.staff.flatMap((s) => s.cuts).map((c) => c.data?.FileName).find((n) => typeof n === "string") ?? null;
    const next = cuts("director").find((c) => c.name === "NEXT");
    const out = {
        staff: ev.staff.filter((s) => !SYSTEM_STAFF.includes(s.name)).map((s) => s.name),
        stb: stb ? stb.replace(/\.stb$/, "") : null,
        talk: names("camera").includes("TALK"),
        save: names("message").includes("SAVE"),
        telop: names("message").includes("TELOP"),
        cameraCuts: names("camera").slice(0, 6),
        director: names("director").slice(0, 6),
    };
    if (next) {
        const d = next.data;
        out.next = { exit: d.ID ?? null, stage: d.Stage ?? null, point: d.StartCode ?? null, room: d.RoomNo ?? null, layer: d.Layer ?? null };
    }
    return out;
}

// ---- placements and their decoded event triggers ----

const SRC = { params: (a) => a.params >>> 0, angleX: (a) => u16(a.angle[0]), angleY: (a) => u16(a.angle[1]), angleZ: (a) => u16(a.angle[2]), setID: (a) => a.setId };
const field = (a, spec) => (spec.src === "const" ? spec.value : (SRC[spec.src](a) >>> spec.shift) & spec.mask);

function decodeTagEvent(a, bits) {
    const p = a.params >>> 0;
    const scale = a.scale ? a.scale.map((s) => s * 0.1) : [1, 1, 1];
    const type = p & 0xff;
    const mult = 100 * (type === 5 ? 10 : 1);
    const invalid = u16(a.angle[0]) & 0x7fff;
    const valid = u16(a.angle[2]);
    const flag = (i) => {
        const label = bits.index[i];
        const e = bits.labels.get(label);
        return e ? { index: i, label, bit: e.value } : { index: i, label: label ?? `#${i}`, bit: null };
    };
    return {
        mapToolId: p >>> 24,
        swbit: (p >> 8) & 0xff,
        swbit2: (p >> 16) & 0xff,
        type,
        horseRodeo: type === 5,
        invalidFlag: invalid !== 0x7fff && invalid !== 0 ? flag(invalid) : null,
        validFlag: valid !== 0xffff && valid !== 0 ? flag(valid) : null,
        area: (u16(a.angle[0]) & 0x8000) ?
            { shape: "box", x: Math.round(scale[0] * mult), y: Math.round(scale[1] * mult), z: Math.round(scale[2] * mult) } :
            { shape: "cylinder", radius: Math.round(scale[0] * mult), height: Math.round(scale[1] * mult) },
    };
}

// Placements grouped by stage and room with their layers (-1: every layer).
function groupPlacements(list, cap = 25) {
    const groups = new Map();
    for (const p of list) {
        const key = `${p.stage}/${p.room}`;
        if (!groups.has(key)) groups.set(key, { stage: p.stage, room: p.room, layers: new Set(), pos: p.pos, actor: p.name });
        groups.get(key).layers.add(p.layer);
    }
    return [...groups.values()].slice(0, cap).map((g) => ({ ...g, layers: g.layers.has(-1) ? "any" : [...g.layers].sort((a, b) => a - b) }));
}

// ---- story conditions ----

const MILESTONE_INDEX = new Map(MILESTONES.map((m, i) => [m.id, i]));
// Twilight is only walked in once the story brings Midna there (R_SP107 from the capture on).
const TWILIGHT_FROM = { 0: "castle-escape", 1: "eldin-twilight", 2: "lanayru-twilight", 6: "mirror" };
// The milestone a curated story move (src/story/StoryMoves.cpp) starts from.
const CURATED_AFTER = {
    "faron-capture": "day3-ilia", "castle-escape": "captured", "faron-gate": "castle-escape", "faron-light": "castle-escape",
    "eldin-gate": "forest-temple", "eldin-light": "eldin-twilight", "lanayru-gate": "after-mines", "lanayru-light": "meteor",
    "mdh-start": "lakebed", "mdh-zelda": "mdh", "master-sword": "mdh-done", "palace-entry": "mirror", "zant-defeated": "mirror",
    "castle-barrier": "zant", "final-battle": "barrier",
};

function makeRecipes(L) {
    const base = milestoneFlags(L);
    // gates: {need, forbid, layers: Set|null, minOrder}
    return function recipe(stage, room, gates) {
        const min = gates.minOrder ?? MILESTONE_INDEX.get(firstVisit(stage, room));
        const build = (i, night) => {
            const f = { ...base[i].flags, bits: new Set(base[i].flags.bits), night };
            for (const b of gates.need ?? []) f.bits.add(b);
            return f;
        };
        for (let i = min; i < base.length; i++) {
            for (const night of [false, true]) {
                const f = build(i, night);
                if ((gates.forbid ?? []).some((b) => f.bits.has(b))) continue;
                const layer = layerNo(stage, room, f, L);
                if (gates.layers && !gates.layers.has(layer)) continue;
                const tw = twilightLevel(stage, room, f);
                if (tw !== null && stage !== "R_SP107" && i < MILESTONE_INDEX.get(TWILIGHT_FROM[tw])) continue;
                return { milestone: base[i].milestone.id, order: i, layer, natural: true, night, flags: f };
            }
        }
        if (!gates.layers || gates.layers.size === 0) return null;
        // A layer no saved flags pick: a cutscene layer that a stage change names.
        const layer = [...gates.layers].sort((a, b) => a - b)[0];
        for (let i = min; i < base.length; i++) {
            const f = build(i, false);
            if ((gates.forbid ?? []).some((b) => f.bits.has(b))) continue;
            return { milestone: base[i].milestone.id, order: i, layer, natural: false, night: false, flags: f };
        }
        return null;
    };
}

// PLYR points without an arrival event of their own, a plain start (mode 0) first; never a horse,
// fall or special-event start (modes 2, 3, 10, 11).
function startPoints(roomData, dist, ok) {
    const mode = (p) => (p.params >>> 12) & 0x1f;
    return roomData.dzx.players.filter((p) => (p.params >>> 24) === NONE && ![2, 3, 10, 11].includes(mode(p)) && ok(p))
        .sort((a, b) => (mode(a) !== 0) - (mode(b) !== 0) || dist(a) - dist(b));
}

function checkpoint(stage, room, point, r) {
    const out = { stage, room, point };
    if (!r.natural) out.layer = r.layer;
    out.eventBits = [...r.flags.bits].sort((a, b) => a - b);
    out.levels = { transform: [...r.flags.transform].sort(), darkClear: [...r.flags.dark].sort() };
    if (r.night) out.hour = 21;
    return out;
}

// ---- classification and the mod's handling (src/story/StoryEvent.cpp notJoinable and friends) ----

const SYSTEM_PROFILES = /^fpcNm_(DOOR|KNOB|SPIRAL_DOOR|L\dM?BOSS_DOOR|PushDoor|TBOX|Obj_BossWarp|SCENE_EXIT|Obj_Kshutter|Obj_SmgDoor|Obj_KkrGate|Obj_RiderGate|Obj_Cdoor|Obj_BoomShutter|Obj_Stopper|Obj_Ladder|Obj_MvStair|Obj_RotStair|Tag_RiverBack|Obj_AmiShutter|Obj_Lv4EdShutter|Obj_Lv6ChgGate|Obj_Swpush|SWC00|SWHIT0|ANDSW2|ALLDIE|Obj_SwBall|Obj_SwLight|Obj_SwHang|Obj_KWheel|Obj_Lv6ElevtA)/;
const SYSTEM_ARCS = /^(Door|L\dM?B?door|Z_bdor|V_\w*Door|A_SM[GK]Door|S_shut|Stop00|Lv\dshut|Lv\dGate|D_KGate|M_RGate|K_l3bdoor|BoxC|SceneExit|ef_Portal|Obj_[ko]brg|TagMsg|MSGNPC|RvBack|P_LBswBC|M_VolcBom|Table$|Prayer|poFire|GWolf|KN_|fairy|SCanCrs|SCanTen)/;
const NPC_PROFILE = /^fpcNm_(NPC_|PERU|OBJ_SEKIZOA|Tag_Mhint|MYNA)/;
const STORY_ACTOR_PROFILES = /^fpcNm_(Tag_TWGate|Obj_SCannon|TAG_EVT|Obj_WoodStatue|Tag_Statue|Obj_TobyHouse)_e$/;
const SYSTEM_ACTOR_PROFILES = /^fpcNm_(Obj_Elevator|Obj_Avalanche)_e$/;

function classify(e) {
    if (e.source === "keep") return { cls: "d", why: "common event list (Event.arc)" };
    if (e.source === "boss") return { cls: "b", why: "boss actor with its own camera (potential event)" };
    if (e.mapType === "camera") return { cls: "d", why: "map-tool camera (no event data)" };
    const reqs = e.triggers.map((t) => t.profile).filter(Boolean);
    if (e.source === "actor" && SYSTEM_ARCS.test(e.arc)) return { cls: "d", why: `system archive ${e.arc}` };
    if (e.summary?.save && e.summary.staff.length === 0) return { cls: "d", why: "save prompt" };
    if (reqs.length && reqs.every((p) => SYSTEM_PROFILES.test(p) || SYSTEM_ACTOR_PROFILES.test(p))) return { cls: "d", why: "door, chest, switch, lift or warp" };
    if (e.source === "actor" && reqs.some((p) => STORY_ACTOR_PROFILES.test(p))) return { cls: "a", why: "story object event (twilight gate, cannon, statue, demo stage jump)" };
    if (/^D_MN\d\d[AB]$/.test(e.stage ?? "") || /^(BOSS|MBOSS|MB_|BS_)/.test(e.name)) return { cls: "b", why: "boss or mini-boss room" };
    if (reqs.some((p) => NPC_PROFILE.test(p))) return { cls: "c", why: reqs.includes("fpcNm_Tag_Mhint_e") ? "Midna hint" : "NPC requester" };
    if (e.source === "actor") return { cls: "c", why: "actor event list" };
    return { cls: "a", why: e.mapType === "stb" ? "STB demo" : "map event or stage event" };
}

function modHandling(e, rules) {
    const m = { pullIn: "unverified", refusal: null, elsewhere: "none", notes: [] };
    const trig = e.triggers.filter((t) => t.kind !== "chain");
    const kinds = new Set(trig.map((t) => t.kind));
    if (e.source === "boss") m.refusal = "not-a-demo";
    else if (e.source === "keep") m.refusal = "list";
    else if (e.mapToolId === undefined) {
        const npcReq = trig.find((t) => rules.npcT.has(t.profile));
        if (npcReq) m.refusal = !npcReq.npcIndex ? "npc-unknown-entry" : rules.npcAllowlist === 0 ? "npc-not-allowlisted" : "npc-allowlist";
        else m.refusal = e.source === "actor" ? "list" : "not-a-map-event";
    } else if (e.mapType === "camera") m.refusal = "camera";
    else if (!e.list || e.list === "keep") m.refusal = "list";
    else if (kinds.size === 0) m.notes.push("no placed trigger found: ordered from code or by an unknown actor");
    else if (![...kinds].some((k) => k === "arrival" || k === "tag")) m.refusal = kinds.has("named") ? "not-a-map-event" : "actor-requester";
    else if (kinds.has("arrival") && !kinds.has("tag") && e.switchNo === NONE && e.exit === NONE) m.refusal = "repeatable-arrival";
    m.pullIn = m.refusal ? "no" : kinds.size ? "likely" : "unverified";
    if (m.pullIn === "likely" && kinds.has("tag")) m.notes.push("B's own tag must pass its trigger terms (same flags and switches)");
    if (m.pullIn === "likely" && kinds.size > 1 && [...kinds].some((k) => k !== "arrival" && k !== "tag")) m.notes.push("other requesters of the same map event are refused (actor-requester)");
    if (e.switchNo !== undefined && e.switchNo !== NONE) m.notes.push(`one-shot: switch ${e.switchNo} set when it starts`);
    if (e.source === "boss") m.notes.push("bosses stay local (enemy sync decision); only the warp out is a story move");
    // Elsewhere: STORY_MOVE only when the event changes the stage (StoryTracker captureDeparture).
    const d = e.dest;
    if (d && d.stage) {
        // matchCuratedMove; a row that also needs an event bit is only a maybe (bits set by code are not decoded).
        const reqs = new Set(e.triggers.map((t) => t.profile).filter(Boolean));
        const cur = rules.curated.find((c) => (!c.toStage || c.toStage === d.stage) && (c.toRoom < 0 || c.toRoom === d.room) &&
            (!c.fromStage || c.fromStage === e.stage) && (!c.eventPrefix || e.name.startsWith(c.eventPrefix)) &&
            !(c.toStage === null && d.stage === e.stage) &&
            !(c.followPoint >= 0 && !c.eventPrefix && !c.fromStage && d.point !== c.followPoint) &&
            (c.toPoint < 0 || c.toPoint === d.point) && (!c.requester || reqs.has(c.requester)));
        const side = rules.sidePoints.has(`${d.stage}/${d.room}/${d.point}`);
        const boss = rules.bossStages.has(e.stage) && d.stage !== e.stage;
        const strong = cur ? `curated ${cur.id}${cur.bit ? ` if it sets ${hex(cur.bit)}` : ""}` : side ? "side-effect entrance" : boss ? "boss clear" : null;
        m.elsewhere = strong ? `prompt (${strong})` : "menu row, or a prompt if the move changes form or levels";
        if (cur) m.curated = cur.id;
    } else if (e.relocates) {
        m.elsewhere = "story move to a place decided in code: prompt if strong, else a menu row";
    } else {
        m.elsewhere = "none: no stage change; flags reach teammates by world sync";
    }
    if (trig.some((t) => rules.notStory.has(t.profile))) m.elsewhere = "none: the requester is a transport or minigame (kNotStoryRequesters)";
    return m;
}

// ---- main ----

function main() {
    const opts = parseArgs(process.argv.slice(2));
    const t0 = Date.now();
    const disc = new Disc(opts.iso);
    const names = decomp.objectNames(opts.decomp);
    const bits = decomp.eventBits(opts.decomp);
    const files = decomp.actorFiles(opts.decomp);
    const tables = decomp.eventTables(files);
    const maps = decomp.mapNames(opts.decomp);
    const profileNo = profileValues(opts.decomp);
    const rules = modRules();
    const L = (i) => bits.labels.get(bits.index[i])?.value ?? -1;
    const bitInfo = (v) => {
        const e = bits.byValue.get(v);
        return e ? `${hex(v)} ${e.label}` : hex(v);
    };
    const decoders = JSON.parse(fs.readFileSync(path.join(__dirname, "decoders.json"), "utf8"));
    const decoderByProfile = new Map();
    for (const dcd of decoders) {
        if (!decoderByProfile.has(dcd.profile)) decoderByProfile.set(dcd.profile, []);
        decoderByProfile.get(dcd.profile).push(dcd);
    }
    const { stages, stb, objectEvents, keep } = loadDisc(disc);
    const recipe = makeRecipes(L);

    const placements = [];
    for (const s of stages.values()) {
        const add = (dzx, room) => {
            for (const a of dzx.actors) placements.push({ ...a, stage: s.name, room, profile: names.get(a.name)?.profile ?? null });
        };
        if (s.stage) add(s.stage.dzx, -1);
        for (const [r, room] of s.rooms) add(room.dzx, r);
    }
    const byProfile = new Map();
    for (const p of placements) {
        if (!byProfile.has(p.profile)) byProfile.set(p.profile, []);
        byProfile.get(p.profile).push(p);
    }
    // Every scene exit (SCLS) by its destination, with the layer it names.
    const sclsTo = new Map();
    for (const s of stages.values()) {
        for (const [room, d] of [[-1, s.stage], ...s.rooms]) {
            (d?.dzx.scls ?? []).forEach((x, i) => {
                const key = `${x.stage}/${x.room}/${x.point}`;
                if (!sclsTo.has(key)) sclsTo.set(key, []);
                sclsTo.get(key).push({ from: `${s.name}/${room}`, exit: i, layer: x.layer });
            });
        }
    }
    const literalFiles = (name) => [...tables].filter(([, t]) => t.literals.has(name.replace(/@$/, ""))).map(([f]) => f);
    const cutscenes = [];

    // ---- map events (REVT) ----
    for (const s of [...stages.values()].sort((a, b) => a.name.localeCompare(b.name))) {
        const roomRevt = new Map([...s.rooms].map(([r, room]) => [r, new Map(room.dzx.revt.map((e) => [e.mapToolId, e]))]));
        const stageRevt = new Map((s.stage?.dzx.revt ?? []).map((e) => [e.mapToolId, e]));
        const eventIn = (room, name) => {
            const pick = (list, where) => {
                const ev = list?.find((x) => x.name === name);
                return ev ? { list: where, ev } : null;
            };
            return (room >= 0 && pick(s.rooms.get(room)?.events, "room")) || pick(s.stage?.events, "stage") ||
                pick(keep, "keep") || { list: null, ev: null };
        };
        const scopes = [[-1, s.stage?.dzx.revt ?? []], ...[...s.rooms].map(([r, room]) => [r, room.dzx.revt])];
        for (const [scopeRoom, revt] of scopes) {
            for (const r of revt) {
                // searchMapEventData looks in the room first, then the stage.
                const rooms = scopeRoom >= 0 ? [scopeRoom] : [...s.rooms.keys()].filter((k) => !roomRevt.get(k).has(r.mapToolId));
                const triggers = [];
                for (const room of rooms) {
                    for (const p of s.rooms.get(room).dzx.players) {
                        if ((p.params >>> 24) === r.mapToolId) {
                            triggers.push({ kind: "arrival", room, point: p.angle[2] & 0xff, mode: (p.params >>> 12) & 0x1f, pos: p.pos });
                        }
                    }
                }
                for (const a of placements) {
                    if (a.stage !== s.name || !(rooms.includes(a.room) || (a.room === -1 && scopeRoom === -1))) continue;
                    if (a.profile === "fpcNm_TAG_EVENT_e") {
                        const t = decodeTagEvent(a, bits);
                        if (t.mapToolId === r.mapToolId) triggers.push({ kind: "tag", actor: a.name, profile: a.profile, room: a.room, layer: a.layer, pos: a.pos, ...t });
                        continue;
                    }
                    for (const dcd of decoderByProfile.get(a.profile) ?? []) {
                        const ids = dcd.mapToolIds.map((spec) => field(a, spec)).filter((v) => v !== NONE);
                        if (!ids.includes(r.mapToolId)) continue;
                        const switches = (dcd.switches ?? []).filter((sw) => sw.src).map((sw) => ({ role: sw.role, no: field(a, sw) })).filter((sw) => sw.no !== NONE);
                        triggers.push({ kind: "actor", actor: a.name, profile: a.profile, room: a.room, layer: a.layer, pos: a.pos, trigger: dcd.trigger, switches, file: dcd.file });
                    }
                }
                const chainScopes = [...roomRevt].filter(([k]) => rooms.includes(k)).concat([[-1, stageRevt]]);
                for (const [room, map] of chainScopes) {
                    for (const other of map.values()) {
                        if (other.next === r.mapToolId && other !== r) triggers.push({ kind: "chain", from: other.name, fromMapToolId: other.mapToolId, room });
                    }
                }
                if (!triggers.some((t) => t.kind !== "chain") && r.type !== 0) {
                    for (const f of literalFiles(r.name)) {
                        for (const prof of files.get(f).profiles) {
                            const here = (byProfile.get(prof) ?? []).filter((p) => p.stage === s.name);
                            triggers.push({ kind: "named", file: f, profile: prof, placements: groupPlacements(here) });
                        }
                    }
                }
                const found = r.type === 0 ? { list: "none", ev: null } : eventIn(scopeRoom, r.name);
                const scls = (scopeRoom >= 0 ? s.rooms.get(scopeRoom).dzx.scls : s.stage?.dzx.scls) ?? [];
                const exitTo = (x) => (x !== NONE && scls[x] ? { stage: scls[x].stage, room: scls[x].room, point: scls[x].point, layer: scls[x].layer } : null);
                const summary = summarize(found.ev);
                const entry = {
                    id: `${s.name}/${scopeRoom < 0 ? "S" : scopeRoom}/m${r.mapToolId}`,
                    source: "map", name: r.name, stage: s.name, room: scopeRoom, table: scopeRoom < 0 ? "stage" : "room",
                    mapToolId: r.mapToolId, mapType: ["camera", "zev", "stb"][r.type] ?? String(r.type), switchNo: r.switchNo,
                    next: r.next, exit: r.exit, dest: exitTo(r.exit) ?? (summary?.next?.exit != null ? exitTo(summary.next.exit) : null),
                    skipDest: exitTo(r.skipExit), list: found.list, summary, triggers,
                };
                if (entry.mapType === "stb") entry.stbArchives = (stb.get(r.name) ?? []).map((w) => w.arc);
                cutscenes.push(entry);
            }
        }
    }

    // ---- named events in stage and room lists that no map event names ----
    for (const s of stages.values()) {
        const revtNames = new Set([...(s.stage?.dzx.revt ?? []), ...[...s.rooms.values()].flatMap((r) => r.dzx.revt)].map((r) => r.name));
        const lists = [[-1, s.stage?.events ?? []], ...[...s.rooms].map(([r, room]) => [r, room.events])];
        for (const [room, events] of lists) {
            for (const ev of events) {
                if (revtNames.has(ev.name) || ev.name.includes("$") || ev.name.endsWith("@")) continue;
                const triggers = [];
                for (const f of literalFiles(ev.name)) {
                    for (const prof of files.get(f).profiles) {
                        const here = (byProfile.get(prof) ?? []).filter((p) => p.stage === s.name && (room < 0 || p.room === room || p.room === -1));
                        triggers.push({ kind: "named", file: f, profile: prof, placements: groupPlacements(here) });
                    }
                }
                const summary = summarize(ev);
                const scls = (room >= 0 ? s.rooms.get(room).dzx.scls : s.stage?.dzx.scls) ?? [];
                const x = summary.next?.exit;
                cutscenes.push({
                    id: `${s.name}/${room < 0 ? "S" : room}/${ev.name}`, source: "named", name: ev.name, stage: s.name, room,
                    table: room < 0 ? "stage" : "room", list: room < 0 ? "stage" : "room", summary, triggers,
                    dest: x != null && scls[x] ? { stage: scls[x].stage, room: scls[x].room, point: scls[x].point, layer: scls[x].layer } : null,
                });
            }
        }
    }

    // ---- actor archive events (NPCs, objects) ----
    for (const [arcName, events] of objectEvents) {
        for (const [idx, ev] of events.entries()) {
            if (ev.name.includes("$") || (ev.name.endsWith("@") && events.some((o) => o.name === ev.name.slice(0, -1)))) continue;
            const triggers = [];
            const base = ev.name.replace(/@$/, "");
            for (const [f, t] of tables) {
                const viaRes = t.resNames?.some((n) => n.toLowerCase() === arcName.toLowerCase());
                const npcIndex = t.npcList ? t.npcList.findIndex((x) => x.name === ev.name && t.resNames?.[x.arc]?.toLowerCase() === arcName.toLowerCase()) : -1;
                if (!(npcIndex > 0 || (viaRes && t.literals.has(base)) || (!viaRes && t.literals.has(base) && t.literals.has(arcName)))) continue;
                for (const prof of files.get(f).profiles) {
                    const where = byProfile.get(prof) ?? [];
                    triggers.push({ kind: "actor-event", file: f, profile: prof, npcIndex: npcIndex > 0 ? npcIndex : null, placements: groupPlacements(where), placementCount: where.length });
                }
            }
            cutscenes.push({ id: `@${arcName}/${ev.name}`, source: "actor", name: ev.name, arc: arcName, list: "actor", summary: summarize(ev), triggers, index: idx });
        }
    }
    for (const ev of keep) {
        cutscenes.push({ id: `@Event/${ev.name}`, source: "keep", name: ev.name, arc: "Event", list: "keep", summary: summarize(ev), triggers: [] });
    }
    // ---- bosses: their intros and deaths run on potential events with their own camera ----
    for (const [f, info] of files) {
        if (!/^d_a_b_\w+\.cpp$/.test(f) || !/fopAcM_orderPotentialEvent/.test(info.text)) continue;
        for (const prof of info.profiles) {
            const where = byProfile.get(prof) ?? [];
            const first = where[0];
            cutscenes.push({
                id: `boss/${prof.replace(/^fpcNm_|_e$/g, "")}`, source: "boss", name: `${prof.replace(/^fpcNm_|_e$/g, "")} intro and defeat`,
                stage: first?.stage ?? null, room: first?.room ?? null, list: "none", summary: null,
                triggers: [{ kind: "boss", file: f, profile: prof, placements: groupPlacements(where) }],
            });
        }
    }

    // ---- classify and the mod's handling ----
    for (const e of cutscenes) {
        e.relocates = !!e.dest || e.triggers.some((t) => decoderByProfile.get(t.profile)?.some((d) => d.relocates)) ||
            e.triggers.some((t) => t.profile === "fpcNm_TAG_EVT_e") || !!e.summary?.next;
        const c = classify(e);
        e.class = c.cls;
        e.classWhy = c.why;
        e.mod = modHandling(e, rules);
    }

    // A map event reached only as another's `next` continues that instance: it shares its fate.
    const mapEvent = new Map(cutscenes.filter((e) => e.source === "map").map((e) => [`${e.stage}/${e.room}/${e.mapToolId}`, e]));
    for (const e of cutscenes) {
        if (e.source !== "map" || !e.triggers.length || e.triggers.some((t) => t.kind !== "chain")) continue;
        const roots = e.triggers.map((t) => mapEvent.get(`${e.stage}/${t.room}/${t.fromMapToolId}`)).filter(Boolean);
        const root = roots.find((r) => r.mod.pullIn === "likely") ?? roots[0];
        if (!root) continue;
        e.chainOf = root.id;
        e.mod.pullIn = root.mod.pullIn === "likely" ? "likely" : root.mod.pullIn === "no" ? "no" : "unverified";
        e.mod.refusal = !root.mod.refusal ? null : root.mod.refusal.startsWith("continuation") ? root.mod.refusal : `continuation (${root.mod.refusal})`;
        e.mod.notes = e.mod.notes.filter((n) => !n.startsWith("no placed trigger"));
        e.mod.notes.push(`plays as the next event of ${root.id} ${root.name}`);
    }

    // ---- when: milestone, layer and conditions per trigger; the test recipe ----
    const arrivalKey = (stage, room, point) => `${stage}/${room}/${point}`;
    const byArrival = new Map();
    for (const e of cutscenes) {
        for (const t of e.triggers) {
            if (t.kind !== "arrival") continue;
            const k = arrivalKey(e.stage, t.room, t.point);
            if (!byArrival.has(k)) byArrival.set(k, []);
            byArrival.get(k).push(e);
        }
    }
    const reachedBy = new Map();
    for (const e of cutscenes) {
        if (e.dest?.stage) {
            const k = arrivalKey(e.dest.stage, e.dest.room, e.dest.point);
            if (!reachedBy.has(k)) reachedBy.set(k, []);
            reachedBy.get(k).push(e);
        }
    }

    const stbLayersOf = (e) => {
        const demoArcs = (stb.get(e.name) ?? []).filter((w) => w.bank !== undefined);
        const local = (stb.get(e.name) ?? []).some((w) => w.stage === e.stage);
        if (e.mapType !== "stb" || !demoArcs.length || local) return null;
        const out = new Map();
        const rooms = e.room >= 0 ? [e.room] : [...stages.get(e.stage).rooms.keys()];
        for (const r of rooms) {
            (stages.get(e.stage).rooms.get(r).dzx.lbnk ?? []).forEach((b, layer) => {
                if (demoArcs.some((w) => w.bank === b.bank && w.bank2 === b.bank2)) {
                    if (!out.has(r)) out.set(r, new Set());
                    out.get(r).add(layer);
                }
            });
        }
        return out;
    };

    const computeMapWhen = (e) => {
        e.when = [];
        e.test = null;
        const stbLayers = stbLayersOf(e);
        if (stbLayers) e.demoLayers = Object.fromEntries([...stbLayers].map(([r, ls]) => [r, [...ls].sort((a, b) => a - b)]));
        for (const t of e.triggers) {
            if (t.kind === "chain" || t.kind === "named") continue;
            const tRoom = t.room >= 0 ? t.room : e.room >= 0 ? e.room : 0;
            let layers = t.layer >= 0 ? new Set([t.layer]) : null;
            const via = [];
            if (t.kind === "arrival") {
                const k = arrivalKey(e.stage, tRoom, t.point);
                const named = new Set();
                for (const x of sclsTo.get(k) ?? []) {
                    if (x.layer >= 0) named.add(x.layer);
                    via.push(`exit ${x.exit} of ${x.from}${x.layer >= 0 ? ` (layer ${x.layer})` : ""}`);
                }
                for (const p of reachedBy.get(k) ?? []) if (p.dest.layer >= 0) named.add(p.dest.layer);
                if (named.size) layers = named;
            }
            if (stbLayers) {
                const ok = stbLayers.get(tRoom) ?? new Set();
                const both = layers ? new Set([...layers].filter((l) => ok.has(l))) : ok;
                layers = both.size ? both : ok;
            }
            const need = [];
            const forbid = [];
            if (t.kind === "tag") {
                if (t.validFlag?.bit != null) need.push(t.validFlag.bit);
                if (t.invalidFlag?.bit != null) forbid.push(t.invalidFlag.bit);
            }
            const r = recipe(e.stage, tRoom, { layers, need, forbid, minOrder: e.minOrder });
            const w = { trigger: t.kind, room: tRoom, layers: layers ? [...layers].sort((a, b) => a - b) : "any" };
            if (t.kind === "arrival") w.point = t.point;
            if (via.length) w.reachedBy = via.slice(0, 3);
            const conditions = [];
            if (t.kind === "tag") {
                if (t.swbit !== NONE) conditions.push(`tag switch ${t.swbit} off (set when it starts)`);
                if (t.swbit2 !== NONE) conditions.push(`switch ${t.swbit2} on`);
                if (t.validFlag) conditions.push(`event bit ${t.validFlag.bit != null ? bitInfo(t.validFlag.bit) : t.validFlag.label} set`);
                if (t.invalidFlag) conditions.push(`event bit ${t.invalidFlag.bit != null ? bitInfo(t.invalidFlag.bit) : t.invalidFlag.label} not set`);
                if (t.horseRodeo) conditions.push("horse rodeo tag (gone once M_023, Epona rescued, is set)");
            }
            if (t.kind === "actor") for (const sw of t.switches ?? []) conditions.push(`switch ${sw.no}: ${sw.role}`);
            if (e.switchNo !== NONE) conditions.push(`map event switch ${e.switchNo} off`);
            if (conditions.length) w.conditions = conditions;
            if (r) Object.assign(w, { milestone: r.milestone, layer: r.layer, natural: r.natural, order: r.order }, r.night ? { night: true } : {});
            else w.unverified = "no milestone or layer found";
            e.when.push(w);
            if (e.test || !r || !["arrival", "tag"].includes(t.kind) || e.class === "d") continue;
            const roomData = stages.get(e.stage).rooms.get(tRoom);
            if (!roomData) continue;
            // A start point in the room with no arrival event of its own, outside the tag's area.
            const outside = (p) => {
                if (t.kind !== "tag") return true;
                const reach = t.area.shape === "box" ? Math.hypot(t.area.x, t.area.z) / 2 : t.area.radius;
                return Math.hypot(p.pos[0] - t.pos[0], p.pos[2] - t.pos[2]) > reach + 300;
            };
            const dist = (p) => Math.hypot(p.pos[0] - t.pos[0], p.pos[2] - t.pos[2]);
            const points = startPoints(roomData, dist, outside);
            if (!points.length) {
                w.noStartPoint = true;
                continue;
            }
            const start = checkpoint(e.stage, tRoom, points[0].angle[2] & 0xff, r);
            if (t.kind === "arrival") start.layer = r.layer;
            if (t.kind === "tag" && t.swbit2 !== NONE) start.saveSwitches = [{ stage: e.stage, no: t.swbit2 }];
            e.test = {
                start,
                trigger: t.kind === "tag" ?
                    { op: "triggerStory", via: "walk", mapToolId: e.mapToolId } :
                    { op: "triggerStory", via: "arrival", room: tRoom, point: t.point, layer: r.layer },
                expectEvent: e.name,
                startDist: Math.round(dist(points[0])),
            };
            if (!r.natural) e.test.unverified = "cutscene layer set explicitly; story flags approximate";
        }
        e.order = Math.min(...e.when.map((w) => w.order ?? 999), 999);
    };

    for (const e of cutscenes) {
        e.when = [];
        e.test = null;
        if (CURATED_AFTER[e.mod.curated]) e.minOrder = MILESTONE_INDEX.get(CURATED_AFTER[e.mod.curated]);
        if (e.source === "map" && e.class !== "d") computeMapWhen(e);
    }
    // An arrival reached by another cutscene's exit comes no earlier than that cutscene.
    for (let pass = 0; pass < 8; pass++) {
        let changed = false;
        for (const e of cutscenes) {
            if (!e.dest?.stage || !(e.order < 999)) continue;
            for (const t of byArrival.get(arrivalKey(e.dest.stage, e.dest.room, e.dest.point)) ?? []) {
                if (t === e || t.class === "d" || (t.minOrder ?? -1) >= e.order || t.order >= e.order) continue;
                t.minOrder = e.order;
                computeMapWhen(t);
                changed = true;
            }
        }
        if (!changed) break;
    }

    // NPC events: when and where from the NPC's placements; the test orders the table entry.
    for (const e of cutscenes) {
        if (e.source !== "actor" || e.class === "d") continue;
        const t = e.triggers.find((x) => x.npcIndex && rules.npcT.has(x.profile) && x.placements.length);
        if (!t) continue;
        for (const g of t.placements) {
            const layers = g.layers === "any" ? null : new Set(g.layers);
            const r = recipe(g.stage, Math.max(g.room, 0), { layers });
            e.when.push({ trigger: "npc", stage: g.stage, room: g.room, layers: g.layers, milestone: r?.milestone, layer: r?.layer, natural: r?.natural, order: r?.order });
            if (e.test || !r || g.room < 0) continue;
            const roomData = stages.get(g.stage)?.rooms.get(g.room);
            const dist = (p) => Math.hypot(p.pos[0] - g.pos[0], p.pos[2] - g.pos[2]);
            const points = roomData ? startPoints(roomData, dist, () => true) : [];
            if (!points.length) continue;
            e.test = {
                start: checkpoint(g.stage, g.room, points[0].angle[2] & 0xff, r),
                trigger: { op: "triggerStory", via: "npc", profile: profileNo.get(t.profile), index: t.npcIndex },
                allowNpc: { profile: profileNo.get(t.profile), event: e.name },
                expectEvent: e.name,
                startDist: Math.round(dist(points[0])),
            };
            if (!r.natural) e.test.unverified = "cutscene layer set explicitly; story flags approximate";
        }
        e.order = Math.min(...e.when.map((w) => w.order ?? 999), 999);
    }

    // ---- region and story order ----
    const REGION_ORDER = ["Ordon", "Faron", "Hyrule Castle (prison and sewers)", "Forest Temple", "Hyrule Field", "Eldin", "Goron Mines",
        "Lanayru", "Lakebed Temple", "Gerudo Desert", "Arbiter's Grounds", "Snowpeak", "Snowpeak Ruins", "Temple of Time",
        "City in the Sky", "Palace of Twilight", "Hyrule Castle", "Mini-Dungeons and Grottos", "Misc", "Unplaced", "Common"];
    for (const e of cutscenes) {
        const st = e.stage ?? e.test?.start.stage ?? e.when[0]?.stage ?? e.triggers[0]?.placements?.[0]?.stage ?? null;
        const room = e.room ?? e.test?.start.room ?? e.when[0]?.room ?? 0;
        e.region = st === "R_SP107" ? "Hyrule Castle (prison and sewers)" : maps.stages.get(st)?.region ?? (e.source === "keep" ? "Common" : "Unplaced");
        e.place = st ? maps.rooms.get(`${st}/${room}`)?.name ?? maps.stages.get(st)?.name ?? st : null;
        e.at = st;
        e.order ??= 999;
    }
    const rank = (e) => REGION_ORDER.indexOf(e.region) >= 0 ? REGION_ORDER.indexOf(e.region) : 50;
    cutscenes.sort((a, b) => rank(a) - rank(b) || a.order - b.order || (a.at ?? "~").localeCompare(b.at ?? "~") ||
        (a.room ?? 0) - (b.room ?? 0) || (a.mapToolId ?? 999) - (b.mapToolId ?? 999) || a.name.localeCompare(b.name));

    const counts = {};
    for (const e of cutscenes) counts[e.class] = (counts[e.class] ?? 0) + 1;
    // System events keep only what identifies them; placement groups lose their sample position.
    const slim = ({ minOrder, at, ...e }) => {
        for (const t of e.triggers) for (const g of t.placements ?? []) delete g.pos;
        if (e.class !== "d") return e;
        const { summary, when, test, mod, ...rest } = e;
        return {
            ...rest, triggers: e.triggers.map((t) => ({ kind: t.kind, actor: t.actor, file: t.file, room: t.room, point: t.point })),
            mod: { pullIn: mod.pullIn, refusal: mod.refusal, elsewhere: mod.elsewhere },
        };
    };
    const catalog = {
        about: "Generated by tools/cutscenes/extract.js from the disc image and the decomp; metadata only. Read docs/cutscenes.md.",
        disc: disc.gameId,
        milestones: MILESTONES.map((m) => ({ id: m.id, text: m.text })),
        counts,
        cutscenes: cutscenes.map(slim),
    };
    fs.mkdirSync(path.dirname(opts.out), { recursive: true });
    const head = JSON.stringify({ ...catalog, cutscenes: [] }, null, 1).replace(/\[\]\n\}$/, "");
    fs.writeFileSync(opts.out, `${head}[\n${catalog.cutscenes.map((e) => JSON.stringify(e)).join(",\n")}\n]}\n`);
    fs.writeFileSync(opts.docs, renderDocs(catalog, { bitInfo }));
    console.log(`${cutscenes.length} events (${Object.entries(counts).map(([k, v]) => `${k}: ${v}`).join(", ")}) in ${((Date.now() - t0) / 1000).toFixed(1)} s`);
    console.log(`wrote ${opts.out} and ${opts.docs}`);
}

main();
