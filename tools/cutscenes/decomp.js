// What the extractor reads from the zeldaret/tp decomp sources of the Dusklight fork.

const fs = require("node:fs");
const path = require("node:path");

const read = (root, rel) => fs.readFileSync(path.join(root, rel), "utf8");

// l_objectName in src/d/d_stage.cpp: stage actor name -> {profile, sub}.
function objectNames(root) {
    const map = new Map();
    const re = /OBJNAME\("([^"]*)",\s*(\w+),\s*(-?\w+)\)/g;
    for (const m of read(root, "src/d/d_stage.cpp").matchAll(re)) {
        if (!map.has(m[1])) map.set(m[1], { profile: m[2], sub: parseInt(m[3], m[3].startsWith("0x") ? 16 : 10) });
    }
    return map;
}

// d_save_bit_labels.inc (label -> value, place, text) and saveBitLabels[] (index -> label).
function eventBits(root) {
    const labels = new Map();
    const byValue = new Map();
    const re = /^\s*(\w+)\s*=\s*(0x[0-9a-fA-F]+),\s*(?:\/\/\s*(.*?)\s*-\s*(.*))?$/;
    for (const line of read(root, "include/d/d_save_bit_labels.inc").split(/\r?\n/)) {
        const m = line.match(re);
        if (!m) continue;
        const e = { label: m[1], value: parseInt(m[2], 16), place: (m[3] ?? "").trim(), text: (m[4] ?? "").trim() };
        labels.set(e.label, e);
        if (!byValue.has(e.value)) byValue.set(e.value, e);
    }
    const save = read(root, "src/d/d_save.cpp");
    const start = save.indexOf("dSv_event_flag_c::saveBitLabels[822] = {");
    const body = save.slice(save.indexOf("{", start) + 1, save.indexOf("};", start));
    const index = body.split(",").map((s) => s.trim()).filter(Boolean);
    return { labels, byValue, index };
}

// Profile per actor source file from its g_profile_* definition.
function actorFiles(root) {
    const dir = path.join(root, "src/d/actor");
    const files = new Map();
    for (const f of fs.readdirSync(dir)) {
        if (!/\.(cpp|inc)$/.test(f)) continue;
        const text = fs.readFileSync(path.join(dir, f), "utf8");
        const profiles = [...text.matchAll(/Proc Name\s*\*\/\s*(fpcNm_\w+)/g)].map((m) => m[1]);
        files.set(f, { text, profiles });
    }
    return files;
}

// The event names each actor file mentions: string literals and its event tables.
function eventTables(files) {
    const out = new Map();
    for (const [f, { text }] of files) {
        const literals = new Set([...text.matchAll(/"([A-Za-z0-9_$]{2,31})"/g)].map((m) => m[1]));
        // daNpcT_evtData_c l_evtList[] = {{"", 0}, {"NAME", arc}, ...}: entry index -> name and arc.
        let npcList = null;
        const m = text.match(/l_evtList\[\w*\]\s*=\s*\{([\s\S]*?)\n\};/);
        if (m && m[1].includes("{")) {
            npcList = [...m[1].matchAll(/\{\s*"([^"]*)"\s*,\s*(\d+)\s*\}/g)].map((e) => ({ name: e[1], arc: +e[2] }));
        }
        const res = text.match(/l_resNameList\[\w*\]\s*=\s*\{([\s\S]*?)\};/);
        const resNames = res ? [...res[1].matchAll(/"([^"]*)"|NULL/g)].map((e) => e[1] ?? "") : null;
        out.set(f, { literals, npcList, resNames });
    }
    return out;
}

// src/dusk/map_loader_definitions.h: region and map name per stage room.
function mapNames(root) {
    const text = read(root, "src/dusk/map_loader_definitions.h");
    const rooms = new Map();
    const stages = new Map();
    let region = "";
    let order = 0;
    const re = /RegionEntry\("([^"]+)"|MapEntry\("([^"]+)",\s*"(\w+)",\s*\{([\s\S]*?)\}\s*(?:,\s*"\w+")?\)/g;
    for (const m of text.matchAll(re)) {
        if (m[1]) {
            region = m[1];
            continue;
        }
        const [, , name, stage, body] = m;
        if (!stages.has(stage)) stages.set(stage, { name, region, order: order++ });
        for (const r of body.matchAll(/\{\s*(\d+)\s*[,}]/g)) {
            const key = `${stage}/${r[1]}`;
            if (!rooms.has(key)) rooms.set(key, { name, region });
        }
    }
    return { stages, rooms };
}

module.exports = { objectNames, eventBits, actorFiles, eventTables, mapNames };
