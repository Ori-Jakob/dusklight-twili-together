// dComIfG_play_c::getLayerNo_common_common (src/d/d_com_inf_game.cpp) and dKy_darkworld_stage_check
// (src/d/d_kankyo.cpp), transcribed, plus the story milestones a synthetic checkpoint is built from.

// l_darkworld_tbl (src/d/d_kankyo_data.cpp): stage -> dark level (0 Faron, 1 Eldin, 2 Lanayru, 6 never cleared).
const DARKWORLD = {
    R_SP107: 2, F_SP108: 0, R_SP108: 0, F_SP109: 1, F_SP110: 1, R_SP109: 1, F_SP111: 1, F_SP112: 2,
    F_SP113: 2, F_SP115: 2, F_SP116: 2, F_SP122: 2, F_SP121: 1, F_SP126: 2, R_SP209: 1, R_SP116: 2,
    D_MN08: 6, D_MN08B: 6, D_MN08C: 6, D_MN08A: 6, D_SB10: 0,
};

// f: {bits: Set, tmp: Set, dark: Set, transform: Set, night: bool}; L(i): saveBitLabels[i].
function darkworld(stage, room, f) {
    if (!(stage in DARKWORLD)) return false;
    let lv = DARKWORLD[stage];
    if (stage === "F_SP121") {
        if (room === 0 || (room >= 2 && room <= 5) || room === 7) lv = 1;
        else if (room >= 9 && room <= 14) lv = 2;
        else return false;
    }
    if (DARKWORLD[stage] === 0 && !f.bits.has(0x4510)) return false;
    return !f.dark.has(lv);
}

function layerNo(stage, room, f, L) {
    const has = (b) => f.bits.has(b);
    const time = (l) => (f.night ? l + 1 : l);
    let layer = darkworld(stage, room, f) ? 14 : -1;
    if (layer < 13) {
        if (stage === "D_MN11" || stage === "F_SP114") {
            if (has(L(266))) layer = 3;
            else if (has(L(163))) layer = 2;
            else if (has(L(162))) layer = 1;
        } else if (stage === "F_SP108") {
            if (has(0x2008)) layer = 5;
            else if (has(0x1e08)) layer = 3;
            else if (has(0x0602)) layer = 2;
            else if (!has(0x4510)) layer = 1;
        } else if (stage === "R_SP108") {
            if (has(L(55))) layer = 2;
            else if (!has(0x4510)) layer = 1;
        } else if (stage === "F_SP109" || stage === "F_SP111") {
            if (has(0x0804)) layer = time(2);
            else if (has(0x0810)) layer = 4;
            else if (has(0x1320)) layer = time(2);
            else if (has(0x0701)) layer = 12;
            else if (has(0x0a08)) layer = time(2);
            else if (has(0x0608)) layer = 1;
        } else if (stage === "R_SP109" || stage === "R_SP209") {
            if (stage === "R_SP109" && room === 1 && has(0x0904)) layer = time(4);
            else if (has(L(85))) layer = time(2);
            else if (has(L(53))) layer = 1;
        } else if (stage === "F_SP110") {
            if (has(L(64))) layer = 2;
        } else if (stage === "R_SP110") {
            if (has(0x2320)) layer = 3;
            else if (has(0x2004)) layer = 4;
            else if (has(0x2020)) layer = 2;
            else if (has(0x0701)) layer = 1;
        } else if (stage === "F_SP115" || stage === "F_SP116" || (stage === "R_SP116" && room === 5) || stage === "R_SP115") {
            if (stage === "F_SP115" && room === 0) {
                if (has(0x3b08)) layer = 3;
                else if (has(0x3120)) layer = 1;
                else if (has(0x0904)) layer = 2;
            } else if (stage === "R_SP116" && room === 5 && has(0x2020)) {
                layer = 4;
            } else if (has(0x1e08) && stage === "F_SP116") {
                layer = room !== 4 && room !== 3 && room !== 1 ? 0 : 1;
            } else if (has(0x0904)) {
                layer = stage === "F_SP115" && room === 1 && !has(0x0c01) ? 9 : 2;
            } else if (stage === "F_SP116" && room === 3 && has(L(68))) {
                layer = 1;
            }
        } else if (stage === "F_SP113") {
            if (has(0x2008)) layer = 2;
        } else if (stage === "F_SP126") {
            if (has(L(95))) layer = 1;
        } else if (stage === "F_SP124" && room === 0) {
            layer = has(0x4008) ? 0 : 8;
        } else if (stage === "F_SP112") {
            if (has(0x0b01)) layer = 1;
            else if (has(0x0902)) layer = 2;
        } else if (stage === "F_SP103") {
            if (room === 0) {
                if (has(L(56))) layer = time(4);
                else if (f.dark.has(0)) layer = time(2);
                else if (has(L(47))) layer = 1;
                else if (has(0x4510)) layer = 7;
                else if (has(0x4a40)) layer = 0;
                else layer = 6;
            } else if (room === 1) {
                if (f.dark.has(0)) layer = 2;
                else if (has(L(47))) layer = 1;
                else if (has(0x4510)) layer = 0;
                else if (has(0x4a40)) layer = 4;
                else layer = 3;
            }
        } else if (stage === "R_SP01") {
            if (room === 1) {
                if (f.dark.has(0)) layer = 2;
            } else if (room === 2) {
                if (has(0x0780)) layer = 3;
                else if (f.dark.has(0)) layer = 2;
                else if (has(L(47))) layer = 1;
            } else if (room === 5) {
                if (has(0x0601)) layer = 4;
                else if (f.dark.has(0)) layer = 2;
            }
        } else if (stage === "F_SP104") {
            if (room === 1) {
                if (f.dark.has(0)) layer = 2;
                else if (has(L(47))) layer = 4;
                else if (has(0x4510)) layer = 0;
                else if (has(0x4a20)) layer = 3;
                else layer = 1;
            } else if (f.dark.has(0)) layer = 2;
            else if (has(L(47))) layer = 4;
        } else if (stage === "F_SP00") {
            if (f.dark.has(0)) layer = time(2);
            else if (has(L(47))) layer = 1;
            else if (has(L(169))) layer = 10;
            else if (has(0x4510)) layer = 9;
            else if (has(0x4a40)) layer = 11;
            else layer = 12;
        } else if (stage === "F_SP121") {
            if (has(0x1e08)) layer = 6;
            else if (has(0x0c01)) layer = 4;
            else if (has(L(68))) layer = 0;
            else if (f.tmp.has(0x0601)) layer = f.tmp.has(0x0602) ? 2 : 3;
        } else if (stage === "F_SP122") {
            if (room === 8) {
                if (has(0x1e08)) layer = 6;
                else if (has(0x0c01)) layer = 4;
            } else if (room === 16) {
                if (has(0x2204)) layer = 6;
                else if (has(0x2102)) layer = 1;
                else if (has(0x1e08)) layer = 6;
                else if (has(0x0c01)) layer = 4;
            } else if (room === 17) {
                if (has(0x1e08)) layer = 0;
                else if (has(0x0c01)) layer = 4;
            }
        } else if (stage === "F_SP128") {
            if (has(0x2320)) layer = 1;
        } else if (stage === "R_SP160") {
            if (room === 5) layer = has(0x1e08) ? 0 : 1;
            else if (has(0x2210)) layer = 1;
        } else if (stage === "F_SP117") {
            if (has(0x2008)) layer = 2;
        } else if (stage === "F_SP118") {
            if (has(0x2b08)) layer = 3;
            else if (has(0x2010)) layer = 2;
            else if (has(0x0b40)) layer = 1;
        } else if (stage === "D_SB10") {
            if (has(0x4510)) layer = 1;
        } else if (stage === "R_SP107") {
            if (f.transform.has(3)) layer = 13;
        } else if (stage === "D_MN09") {
            if (room !== 11 && room !== 13 && room !== 14) layer = 1;
        }
        // F_SP127 / R_SP127 follow the fishing hole season (not modelled: layer 0).
    }
    if (layer === 14) {
        if (has(L(65))) {
            const lanayru = ["F_SP112", "F_SP113", "F_SP115", "F_SP116", "R_SP115", "F_SP121", "F_SP126", "F_SP122"];
            if (lanayru.includes(stage) || (stage === "R_SP116" && room === 5)) {
                if (stage !== "F_SP121" || (room >= 9 && room <= 14)) layer = 13;
            }
        }
        if (stage === "R_SP107" && room === 0) {
            if (!has(0x4d08)) layer = 11;
        } else if (stage === "D_MN08A" && room === 10) {
            layer = has(0x5410) ? 1 : 0;
        }
    }
    return layer < 0 ? 0 : layer;
}

// Story milestones in play order, each adding to the ones before (lib.js STORY_BITS style).
// bits: event bits; L: saveBitLabels indices; transform/dark: level bits gained.
const MILESTONES = [
    { id: "new-game", text: "a new game: Ordon, day 1" },
    { id: "day1", text: "Ordon day 1 done", bits: [0x4a40, 0x4a20] },
    { id: "day2", text: "Ordon day 2 done (Faron in twilight)", bits: [0x4510] },
    { id: "day3-ilia", text: "Ordon day 3: Ilia at the spring with Epona", L: [169] },
    { id: "captured", text: "captured: taken to the castle cell", transform: [0] },
    { id: "cell-wakeup", text: "woke up in the cell (F_0630)", bits: [0x4d08] },
    { id: "castle-escape", text: "escaped the castle with Midna", L: [47], bits: [0x0502] },
    { id: "faron-light", text: "Faron Spring restored", dark: [0], bits: [0x0c40] },
    { id: "forest-temple", text: "Forest Temple cleared", L: [55], bits: [0x0602] },
    { id: "eldin-twilight", text: "Eldin twilight entered", transform: [1] },
    { id: "eldin-light", text: "Eldin Spring restored", dark: [1], bits: [0x0708] },
    { id: "epona", text: "Epona tamed", L: [56], bits: [0x0601] },
    { id: "kb1-trigger", text: "King Bulblin 1 ride started", L: [53], bits: [0x0608] },
    { id: "kb1-defeated", text: "King Bulblin 1 defeated", L: [85], bits: [0x0a08, 0x0780] },
    { id: "goron-mines", text: "Goron Mines cleared", L: [64], bits: [0x0701] },
    { id: "after-mines", text: "the scene after the Goron Mines", bits: [0x1320] },
    { id: "lanayru-twilight", text: "Lanayru twilight entered", transform: [2] },
    { id: "meteor", text: "the meteor warped to Zora's Domain", L: [65], bits: [0x0880] },
    { id: "lanayru-light", text: "Lanayru Spring restored", dark: [2], bits: [0x0c02] },
    { id: "wagon", text: "Telma's wagon escort done", L: [68], bits: [0x0810] },
    { id: "zora-armor", text: "Zora Armor obtained", bits: [0x0804] },
    { id: "iza", text: "Iza's river ride unlocked", L: [95], bits: [0x0902, 0x0b01] },
    { id: "lakebed", text: "Lakebed Temple cleared", bits: [0x0904] },
    { id: "mdh", text: "Zant appears: Midna's Desperate Hour", transform: [3], bits: [0x0c01] },
    { id: "mdh-done", text: "Zelda healed Midna", bits: [0x1e08] },
    { id: "master-sword", text: "Master Sword drawn", bits: [0x2020, 0x0d04] },
    { id: "desert", text: "Fyer's cannon to the desert", bits: [0x4008] },
    { id: "tent", text: "escaped the burning tent", bits: [0x0b40] },
    { id: "arbiter", text: "Arbiter's Grounds cleared", bits: [0x2010] },
    { id: "yeta-pumpkin", text: "Yeta given the pumpkin", L: [162] },
    { id: "yeta-cheese", text: "Yeta given the cheese", L: [163] },
    { id: "snowpeak", text: "Snowpeak Ruins cleared", L: [266], bits: [0x2008] },
    { id: "time", text: "Temple of Time cleared", bits: [0x2004] },
    { id: "louise", text: "Louise's medicine scent", bits: [0x2102] },
    { id: "wood-statue", text: "wood statue obtained", bits: [0x2204] },
    { id: "ilia-memory", text: "wood statue returned to Ilia", bits: [0x2320] },
    { id: "cannon-lake", text: "the Sky Cannon warped to Lake Hylia", bits: [0x3120] },
    { id: "cannon-fixed", text: "the Sky Cannon repaired", bits: [0x3b08] },
    { id: "city", text: "City in the Sky cleared", bits: [0x2002] },
    { id: "mirror", text: "the Mirror of Twilight restored", bits: [0x2b08] },
    { id: "zant", text: "Zant defeated", bits: [0x5410] },
    { id: "barrier", text: "the castle barrier shattered", bits: [0x4208] },
];

// Flag sets per milestone (cumulative); `L` resolves saveBitLabels indices.
function milestoneFlags(L) {
    const out = [];
    const f = { bits: new Set(), tmp: new Set(), dark: new Set(), transform: new Set(), night: false };
    for (const m of MILESTONES) {
        for (const b of m.bits ?? []) f.bits.add(b);
        for (const i of m.L ?? []) f.bits.add(L(i));
        for (const l of m.transform ?? []) f.transform.add(l);
        for (const l of m.dark ?? []) f.dark.add(l);
        out.push({ milestone: m, flags: { bits: new Set(f.bits), tmp: new Set(), dark: new Set(f.dark), transform: new Set(f.transform), night: false } });
    }
    return out;
}

// The dark level whose twilight the stage room is in with these flags, or null.
function twilightLevel(stage, room, f) {
    if (!darkworld(stage, room, f)) return null;
    if (stage !== "F_SP121") return DARKWORLD[stage];
    return room >= 9 && room <= 14 ? 2 : 1;
}

module.exports = { layerNo, milestoneFlags, twilightLevel, MILESTONES, DARKWORLD };
