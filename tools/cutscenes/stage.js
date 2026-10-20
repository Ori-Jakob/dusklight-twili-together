// Stage/room data (dzs, dzr) and event_list.dat parsers, after include/d/d_stage.h and d_event_data.h.

const cstr = (buf, off, max) => {
    const end = buf.indexOf(0, off);
    return buf.toString("latin1", off, end < 0 || end > off + max ? off + max : end);
};

const ENTRY_SIZE = { actor: 0x20, scaled: 0x24, scls: 0x0d, revt: 0x1c };
const LAYER_CHARS = "0123456789abcde";

// Layer chunk tags end in 0-9 or a-e: ACT3 is layer 3, SCOa layer 10.
function layerOf(tag, base) {
    if (tag.startsWith(base) && tag.length === 4) {
        const l = LAYER_CHARS.indexOf(tag[3]);
        if (l >= 0) return l;
    }
    return -1;
}

function readActor(buf, o, scaled) {
    const a = {
        name: cstr(buf, o, 8),
        params: buf.readUInt32BE(o + 8),
        pos: [buf.readFloatBE(o + 0x0c), buf.readFloatBE(o + 0x10), buf.readFloatBE(o + 0x14)].map((v) => Math.round(v)),
        angle: [buf.readInt16BE(o + 0x18), buf.readInt16BE(o + 0x1a), buf.readInt16BE(o + 0x1c)],
        setId: buf.readUInt16BE(o + 0x1e),
    };
    if (scaled) a.scale = [buf[o + 0x20], buf[o + 0x21], buf[o + 0x22]];
    return a;
}

const ACTOR_CHUNKS = { ACTR: false, TGOB: false, TRES: false, SCOB: true, TGSC: true, TGDR: true, Door: true };
const LAYER_ACTOR_CHUNKS = { ACT: false, TRE: false, SCO: true, Doo: true };

// {actors: [{...actor, chunk, layer (-1 = every layer)}], players, scls, revt, lbnk, rtbl}.
function parseDzx(buf) {
    const out = { actors: [], players: [], scls: [], revt: [], lbnk: null, rtbl: [] };
    const count = buf.readUInt32BE(0);
    for (let i = 0; i < count; i++) {
        const h = 4 + i * 12;
        const tag = buf.toString("latin1", h, h + 4);
        const num = buf.readUInt32BE(h + 4);
        const off = buf.readUInt32BE(h + 8);
        let chunk = null;
        if (tag in ACTOR_CHUNKS) {
            chunk = { layer: -1, scaled: ACTOR_CHUNKS[tag] };
        } else {
            for (const [base, scaled] of Object.entries(LAYER_ACTOR_CHUNKS)) {
                if (layerOf(tag, base) >= 0) chunk = { layer: layerOf(tag, base), scaled };
            }
        }
        if (chunk) {
            const size = chunk.scaled ? ENTRY_SIZE.scaled : ENTRY_SIZE.actor;
            for (let k = 0; k < num; k++) {
                out.actors.push({ ...readActor(buf, off + k * size, chunk.scaled), chunk: tag, layer: chunk.layer });
            }
        } else if (tag === "PLYR") {
            for (let k = 0; k < num; k++) out.players.push(readActor(buf, off + k * ENTRY_SIZE.actor, false));
        } else if (tag === "SCLS") {
            for (let k = 0; k < num; k++) {
                const o = off + k * ENTRY_SIZE.scls;
                // dStage_sclsInfo_getSceneLayer: 15 = from the flags.
                const layer = buf[o + 0x0b] & 0x0f;
                out.scls.push({ stage: cstr(buf, o, 8), point: buf[o + 8], room: buf.readInt8(o + 9), layer: layer >= 15 ? -1 : layer });
            }
        } else if (tag === "REVT") {
            for (let k = 0; k < num; k++) {
                const o = off + k * ENTRY_SIZE.revt;
                const type = buf[o];
                out.revt.push({
                    type, mapToolId: buf[o + 4], next: buf[o + 5], priority: buf[o + 6], exit: buf[o + 7],
                    skipExit: buf[o + 9], name: type === 0 ? `MapToolCamera${buf[o + 4]}` : cstr(buf, o + 0x0d, 13),
                    switchNo: buf[o + 0x1b],
                });
            }
        } else if (tag === "LBNK") {
            out.lbnk = [];
            for (let k = 0; k < num; k++) out.lbnk.push({ bank: buf[off + k * 3], bank2: buf[off + k * 3 + 1] });
        } else if (tag === "RTBL") {
            for (let k = 0; k < num; k++) {
                const e = buf.readUInt32BE(off + k * 4);
                const n = buf[e];
                const rooms = buf.readUInt32BE(e + 4);
                out.rtbl.push([...buf.subarray(rooms, rooms + n)].map((r) => r & 0x3f));
            }
        }
    }
    return out;
}

const STAFF_TYPES = ["default", "all", "camera", "effect", "timekeeper", "5", "director", "message", "sound", "light", "10", "package", "create"];

// event_list.dat: events with their staff and each staff's cut names and data.
function parseEventList(buf) {
    if (!buf || buf.length < 0x40) return [];
    const h = (o) => buf.readUInt32BE(o);
    const eventTop = h(0), eventNum = h(4), staffTop = h(8), cutTop = h(0x10), dataTop = h(0x18);
    const fTop = h(0x20), iTop = h(0x28), sTop = h(0x30);
    const readData = (first) => {
        const data = {};
        for (let d = first, guard = 0; d >= 0 && guard < 64; guard++) {
            const o = dataTop + d * 0x40;
            const name = cstr(buf, o, 32);
            const type = buf.readInt32BE(o + 0x24);
            const index = buf.readInt32BE(o + 0x28);
            const number = buf.readInt32BE(o + 0x2c);
            let value = null;
            if (type === 0) value = Array.from({ length: number }, (_, k) => +buf.readFloatBE(fTop + (index + k) * 4).toFixed(2));
            else if (type === 1) value = Array.from({ length: number * 3 }, (_, k) => +buf.readFloatBE(fTop + (index + k) * 4).toFixed(1));
            else if (type === 3) value = Array.from({ length: number }, (_, k) => buf.readInt32BE(iTop + (index + k) * 4));
            else if (type === 4) value = cstr(buf, sTop + index, number);
            data[name] = Array.isArray(value) && value.length === 1 ? value[0] : value;
            d = buf.readInt32BE(o + 0x30);
        }
        return data;
    };
    const events = [];
    for (let e = 0; e < eventNum; e++) {
        const o = eventTop + e * 0xb0;
        const nStaff = buf.readInt32BE(o + 0x7c);
        const staff = [];
        for (let s = 0; s < nStaff && s < 20; s++) {
            const si = buf.readInt32BE(o + 0x2c + s * 4);
            const so = staffTop + si * 0x50;
            const cuts = [];
            for (let c = buf.readInt32BE(so + 0x30), guard = 0; c >= 0 && guard < 200; guard++) {
                const co = cutTop + c * 0x50;
                cuts.push({ name: cstr(buf, co, 32), data: readData(buf.readInt32BE(co + 0x38)) });
                c = buf.readInt32BE(co + 0x3c);
            }
            const type = buf.readInt32BE(so + 0x2c);
            staff.push({ name: cstr(buf, so, 8), type: STAFF_TYPES[type] ?? String(type), cuts });
        }
        events.push({ name: cstr(buf, o, 32), priority: buf.readInt32BE(o + 0x28), staff });
    }
    return events;
}

module.exports = { parseDzx, parseEventList, LAYER_CHARS };
