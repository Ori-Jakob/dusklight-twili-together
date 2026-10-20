#!/usr/bin/env node
// Lists every placed heart piece and heart container on the disc: field actors (htPiece, htCase, item,
// witem), chests and the roasted boar. NPC and minigame rewards live in code, not stage data.
//   node tools/cutscenes/heart-pieces.js [--iso <image>] [--json]

const fs = require("node:fs");
const path = require("node:path");
const { Disc, rarc, arcFile } = require("./gc");
const { parseDzx } = require("./stage");

const KAKERA_HEART = 0x21;
const UTAWA_HEART = 0x22;
const FIELD = new Set(["htPiece", "htCase", "item", "witem"]);
const hex = (v) => `0x${v.toString(16).padStart(2, "0")}`;

function parseArgs(argv) {
    const o = { iso: null, json: false };
    for (let i = 0; i < argv.length; i++) {
        if (argv[i] === "--json") o.json = true;
        else if (argv[i] === "--iso") o.iso = path.resolve(argv[++i]);
        else throw new Error(`unknown option ${argv[i]}`);
    }
    if (!o.iso) {
        const cfg = path.join(process.env.APPDATA ?? "", "TwilitRealm", "Dusklight", "config.json");
        o.iso = JSON.parse(fs.readFileSync(cfg, "utf8"))["backend.isoPath"];
    }
    return o;
}

// One row per heart actor; null for anything else.
function heartOf(a) {
    const p = a.params >>> 0;
    if (FIELD.has(a.name) && ((p & 0xff) === KAKERA_HEART || (p & 0xff) === UTAWA_HEART)) {
        return { kind: a.name.startsWith("ht") ? a.name : "item", item: p & 0xff, bit: (p >>> 8) & 0xff };
    }
    // daTbox_c: item in angle.z >> 8, number in params bits 6-11, appear switch in bits 12-19.
    if (/^tbox[ABJPW]/.test(a.name) && ((a.angle[2] >>> 8) & 0xff) === KAKERA_HEART) {
        return { kind: "chest", item: KAKERA_HEART, tbox: (p >>> 6) & 0x3f, sw: (p >>> 12) & 0xff, swType: a.angle[0] & 0xf };
    }
    if (/^tboxEL/.test(a.name) && (p & 0xff) === KAKERA_HEART) {
        return { kind: "chest", item: KAKERA_HEART, tbox: (p >>> 16) & 0xff, sw: 0xff };
    }
    // daOBJ_RW: the heart piece's item bit in bits 16-23, its switch in 24-31.
    if (a.name === "Obj_rw") {
        return { kind: "Obj_rw", item: KAKERA_HEART, bit: (p >>> 16) & 0xff, sw: (p >>> 24) & 0xff };
    }
    return null;
}

function main() {
    const opts = parseArgs(process.argv.slice(2));
    const disc = new Disc(opts.iso);
    const rows = [];
    for (const f of disc.files.keys()) {
        const m = f.match(/^res\/Stage\/(\w+)\/(?:STG_00|R(\d+)_00)\.arc$/);
        if (!m) continue;
        const dz = arcFile(rarc(disc.file(f)), m[2] === undefined ? "stage.dzs" : "room.dzr");
        if (!dz) continue;
        for (const a of parseDzx(dz).actors) {
            const h = heartOf(a);
            if (h) rows.push({ stage: m[1], room: m[2] === undefined ? -1 : +m[2], layer: a.layer, name: a.name, ...h, pos: a.pos });
        }
    }
    rows.sort((x, y) => x.stage.localeCompare(y.stage) || x.room - y.room || x.layer - y.layer);
    if (opts.json) {
        console.log(JSON.stringify(rows, null, 1));
        return;
    }
    for (const r of rows) {
        const where = `${r.stage} room ${r.room} layer ${r.layer < 0 ? "all" : r.layer}`;
        const what = r.kind === "chest" ? `chest ${r.tbox} (${r.name}, appear sw ${hex(r.sw)})` :
            `${r.name} item ${hex(r.item)} bit ${hex(r.bit)}${r.sw !== undefined ? ` sw ${hex(r.sw)}` : ""}`;
        console.log(`${where.padEnd(28)} ${what}`);
    }
}

main();
