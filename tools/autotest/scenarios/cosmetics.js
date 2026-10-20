// Teammates' cosmetics on puppets (src/autotest/StepsCosmetics.cpp); COSMETICS_HOLD <what> (on A)
// marks a moment worth a capture.

const { MODS, STAGES, COMMON_CVARS, tt, escapeModId, barrier, meetIn, connect } = require("../lib");

const cos = (name, value) => `mod.${escapeModId(MODS.cosmetics)}.${name}=${value}`;
const cosCvars = (vars) => Object.entries(vars).map(([name, value]) => cos(name, value));
const colorCvars = (hex) => [tt("color", hex.slice(1).toLowerCase())];
// Share My Looks: Everyone in Room (the default is Off).
const SHARE_ROOM = [tt("share_looks", 2)];
const RED = "#E53935";
const BLUE = "#1E88E5";
const WHITE = "#FFFFFF";
// dItemNo_*.
const CLOTHES = { casual: 0x2e, kokiri: 0x2f, armor: 0x30, zora: 0x31 };
// INVINCIBLE: the armor stays powered with no rupees.
const ARMOR_CVARS = ["game.armorRupeeDrain=3"];
// The minimap has to show for expectMapCursor (color.js).
const MAP_CVARS = ["game.minimalHUD=false", "game.recordingMode=false", "game.debugFlyCam=false",
    "game.enableMirrorMode=false"];
const field = STAGES.faronField;
const faron = STAGES.southFaron;
const hold = (what, sec = 2) => [
    { op: "recolorCamera", target: "dummy", dist: 150, height: 75, up: 25 },
    { op: "wait", frames: 10 },
    { op: "mark", msg: `COSMETICS_HOLD ${what}` },
    { op: "wait", sec },
];
// meetIn leaves both on the same spot: B steps away so A's Link does not hide the dummy.
const stepAway = [{ op: "walk", frames: 40, stickY: 1 }, { op: "wait", frames: 30 }];
const recolor = (hex) => ({ state: "recolor", rgb: `#${hex}`, source: "pointer", vanilla: true });
const BLOCK = { state: "block", source: "pointer" };
const TINTED = { state: "none" };

// Every colour our recolour covers in M1, with the two hair indices.
const SHARED = {
    herosTunicTorsoColor: "1e88e5",
    linkHairColor: "ab706e",
    woodenSwordColor: "b9ab00",
    ordonSwordBladeColor: "ff0000",
    ironBootsColor: "505154",
    boomerangColor: "94749a",
    spinnerColor: "ec8644",
    zoraArmorScalesColor: "6382a0",
    wolfLinkColor: "91723e",
    eponaColor: "ec9fc8",
    midnaHairBaseColor: 5,
    midnaHairTipsColor: 2,
};
// Every TEX1 name a colour targets on the player puppet and on its Epona (kTargets).
const PLAYER_TEXTURES = ["al_cap", "al_upbody", "al_lowbody", "zl_cap", "zl_helmet", "zl_armor", "zl_armL",
    "zl_body", "zl_boots", "al_SWB", "al_SWA", "al_SWgripA", "al_SWM", "al_SWgripM", "L_al_boom00", "al_bootsH",
    "al_SP", "al_hair", "bl_hair", "wl_body", "wl_eye.1", "wl_eye.2", "wl_eye.3", "wl_eye.4", "wl_eye.5"];
const HORSE_TEXTURES = ["hs_body", "hs_eye.1", "hs_eye.2", "hs_eye.3"];
const present = (names) => Object.fromEntries(names.map((name) => [name, {}]));
const sharedOnLink = {
    herosTunicTorsoColor: recolor(SHARED.herosTunicTorsoColor),
    linkHairColor: recolor(SHARED.linkHairColor),
    woodenSwordColor: recolor(SHARED.woodenSwordColor),
    ordonSwordBladeColor: recolor(SHARED.ordonSwordBladeColor),
    ironBootsColor: recolor(SHARED.ironBootsColor),
    boomerangColor: recolor(SHARED.boomerangColor),
    spinnerColor: recolor(SHARED.spinnerColor),
    zoraArmorScalesColor: recolor(SHARED.zoraArmorScalesColor),
    wolfLinkColor: recolor(SHARED.wolfLinkColor),
    // Left at their default: the pristine bytes, so our own cosmetics cannot land there.
    herosTunicCapColor: BLOCK,
    herosTunicSkirtColor: BLOCK,
    zoraArmorCapColor: BLOCK,
    zoraArmorHelmetColor: BLOCK,
    zoraArmorTorsoColor: BLOCK,
    zoraArmorFlippersColor: BLOCK,
    ordonSwordHandleColor: BLOCK,
    msBladeColor: BLOCK,
    msHandleColor: BLOCK,
};

module.exports = [
    {
        name: "cosmetics-share",
        description: "B (red, sharing with the room) shares a colour for every cosmetics target and two Midna hair indices: A gets them in B's manifest and its dummy of B wears them as POINTER replacements checked against the disc's own textures (every target's texture found on the disc and on the dummy; hero's clothes untinted, the rest of the set and the defaults blocked against A's own colours), Midna's palette, the Magic Armor still red, B's Epona and wolf in B's colours; A has colours of its own but shares none, as sharing is off by default",
        timeoutSec: 540,
        cvars: [...COMMON_CVARS, ...ARMOR_CVARS],
        enableMods: [MODS.cosmetics],
        instances: [
            {
                name: "A",
                start: field,
                cvars: cosCvars({ herosTunicCapColor: "2e7d32", msBladeColor: "2e7d32" }),
                steps: [
                    { op: "cosmeticsSelfTest" },
                    ...connect,
                    ...meetIn(field, "B"),
                    { op: "clearEnemies" },
                    ...barrier("apart", "B"),
                    { op: "expectCosmetics", name: "B", vars: SHARED, version: "1.0.0", timeoutSec: 20 },
                    { op: "expectCosmetics", self: true, none: true },
                    { op: "expectDummyCosmetics", name: "B", keys: "disc", allKeys: true, vars: sharedOnLink,
                      textures: present(PLAYER_TEXTURES), hair: [5, 2], noSource: true, frames: 15, timeoutSec: 30 },
                    { op: "expectDummyRecolor", name: "B", set: "kokiri", vanilla: true, frames: 15 },
                    ...hold("kokiri"),
                    ...barrier("saw-kokiri", "B"),
                    { op: "waitSignal", name: "armor", from: "B", timeoutSec: 60 },
                    // Magic Armor keeps the tint; the sword and hair in it take B's colours.
                    { op: "expectDummyRecolor", name: "B", set: "magic", rgb: RED, frames: 15, timeoutSec: 20 },
                    { op: "expectDummyCosmetics", name: "B", vars: {
                        woodenSwordColor: sharedOnLink.woodenSwordColor,
                        linkHairColor: sharedOnLink.linkHairColor,
                    }, frames: 5 },
                    ...hold("armor"),
                    ...barrier("saw-armor", "B"),
                    { op: "waitSignal", name: "horse", from: "B", timeoutSec: 90 },
                    // Her mane is not tinted: B's Epona colour replaces it.
                    { op: "expectRemoteHorse", ridden: false, vanilla: true, frames: 15, timeoutSec: 30 },
                    { op: "expectDummyCosmetics", name: "B", horse: true, keys: "disc", allKeys: true,
                      vars: { eponaColor: recolor(SHARED.eponaColor) }, textures: present(HORSE_TEXTURES),
                      noSource: true, frames: 15 },
                    ...barrier("saw-horse", "B"),
                    { op: "waitSignal", name: "wolf", from: "B", timeoutSec: 90 },
                    { op: "expectDummyRecolor", name: "B", set: "wolf", vanilla: true, frames: 15, timeoutSec: 20 },
                    { op: "expectDummyCosmetics", name: "B",
                      vars: { wolfLinkColor: sharedOnLink.wolfLinkColor }, hair: [5, 2], frames: 15 },
                    ...hold("wolf"),
                    ...barrier("saw-wolf", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: field,
                cvars: [...colorCvars(RED), ...SHARE_ROOM, ...cosCvars(SHARED)],
                steps: [
                    ...connect,
                    ...meetIn(field, "A"),
                    { op: "clearEnemies" },
                    ...stepAway,
                    ...barrier("apart", "A"),
                    { op: "expectCosmetics", self: true, vars: SHARED, version: "1.0.0", timeoutSec: 20 },
                    ...barrier("saw-kokiri", "A"),
                    { op: "setClothes", item: CLOTHES.armor, timeoutSec: 30 },
                    { op: "signal", name: "armor" },
                    ...barrier("saw-armor", "A"),
                    { op: "setClothes", item: CLOTHES.kokiri, timeoutSec: 30 },
                    { op: "spawnHorse", dist: 300, side: 200 },
                    { op: "signal", name: "horse" },
                    ...barrier("saw-horse", "A"),
                    { op: "transform", form: "wolf", timeoutSec: 30 },
                    { op: "signal", name: "wolf" },
                    ...barrier("saw-wolf", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "cosmetics-color-rule",
        description: "B in red shares a blue tunic: A's dummy of B wears the blue tunic untinted, while B's client entry (the players list card), name tag and map marker stay red, and the Ordon clothes, Magic Armor and Zora armour (no cosmetics colour of theirs) keep the red tint",
        timeoutSec: 420,
        cvars: [...COMMON_CVARS, ...MAP_CVARS, ...ARMOR_CVARS],
        enableMods: [MODS.cosmetics],
        instances: [
            {
                name: "A",
                start: STAGES.forestTemple,
                steps: [
                    { op: "waitStage", stage: STAGES.forestTemple.stage, timeoutSec: 90 },
                    { op: "showMinimap" },
                    ...connect,
                    ...meetIn(STAGES.forestTemple, "B"),
                    { op: "expectDummyCosmetics", name: "B", keys: "disc",
                      vars: { herosTunicTorsoColor: recolor("1e88e5") }, frames: 15 },
                    { op: "expectDummyRecolor", name: "B", set: "kokiri", vanilla: true, frames: 15 },
                    { op: "expectPeerColor", name: "B", rgb: RED },
                    { op: "expectNameTag", name: "B", rgb: RED, holdTicks: 10 },
                    { op: "expectMapCursor", target: "B", color: RED, timeoutSec: 5 },
                    ...barrier("checked-kokiri", "B"),
                    ...[["casual", "ordon"], ["armor", "magic"], ["zora", "zora"]].flatMap(([clothes, set]) => [
                        { op: "waitSignal", name: `wear-${clothes}`, from: "B", timeoutSec: 60 },
                        { op: "expectDummyRecolor", name: "B", set, rgb: RED, frames: 15, timeoutSec: 20 },
                        { op: "expectPeerColor", name: "B", rgb: RED },
                        ...barrier(`saw-${clothes}`, "B"),
                    ]),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.forestTemple,
                cvars: [...colorCvars(RED), ...SHARE_ROOM, cos("herosTunicTorsoColor", "1e88e5")],
                steps: [
                    { op: "waitStage", stage: STAGES.forestTemple.stage, timeoutSec: 90 },
                    ...connect,
                    ...meetIn(STAGES.forestTemple, "A"),
                    ...barrier("checked-kokiri", "A"),
                    ...["casual", "armor", "zora"].flatMap((clothes) => [
                        { op: "setClothes", item: CLOTHES[clothes], timeoutSec: 30 },
                        { op: "signal", name: `wear-${clothes}` },
                        ...barrier(`saw-${clothes}`, "A"),
                    ]),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "cosmetics-defaults",
        description: "B shares with the room, but has the cosmetics mod on with every colour at its default (the one it set is not a colour): B shares none, and A's dummy of B shows B's red tint, its tunic textures left to the tint and the rest blocked against A's own red hair",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        enableMods: [MODS.cosmetics],
        instances: [
            {
                name: "A",
                start: faron,
                cvars: cosCvars({ linkHairColor: "ff0000" }),
                steps: [
                    ...connect,
                    ...meetIn(faron, "B"),
                    { op: "wait", sec: 3 },
                    { op: "expectCosmetics", name: "B", none: true, frames: 30 },
                    { op: "expectDummyRecolor", name: "B", set: "kokiri", rgb: RED, frames: 15 },
                    { op: "expectDummyCosmetics", name: "B", keys: "disc", vars: {
                        herosTunicTorsoColor: TINTED, herosTunicCapColor: TINTED, linkHairColor: BLOCK,
                        woodenSwordColor: BLOCK, ironBootsColor: BLOCK,
                    }, hair: [0, 0], noSource: true, frames: 15 },
                    ...barrier("checked", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: faron,
                cvars: [...colorCvars(RED), ...SHARE_ROOM, cos("herosTunicTorsoColor", "zzzzzz")],
                steps: [
                    ...connect,
                    ...meetIn(faron, "A"),
                    { op: "expectCosmetics", self: true, none: true, timeoutSec: 10 },
                    ...barrier("checked", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "cosmetics-leak",
        description: "A's own cosmetics are red, B (white) shares blue: A sees blue on B's dummy and none of A's red (no texture of the dummy renders from a content replacement); B turns sharing off: B's exact white leaves the tunic as on disc, which A's red would match, and A's blockers keep it off; B turns blue: A shows B's tint, still no red; A picks My Own for B: A's red hair lands on B's dummy (no blockers), the tinted tunic stays B's blue, and back to Theirs blocks it again",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        enableMods: [MODS.cosmetics],
        instances: [
            {
                name: "A",
                start: faron,
                cvars: cosCvars({ herosTunicTorsoColor: "ff0000", herosTunicCapColor: "ff0000",
                    linkHairColor: "ff0000", woodenSwordColor: "ff0000", ironBootsColor: "ff0000" }),
                steps: [
                    ...connect,
                    ...meetIn(faron, "B"),
                    { op: "expectDummyCosmetics", name: "B", keys: "disc", vars: {
                        herosTunicTorsoColor: recolor("1e88e5"), linkHairColor: recolor("1e88e5"),
                        herosTunicCapColor: BLOCK, woodenSwordColor: BLOCK, ironBootsColor: BLOCK,
                    }, noSource: true, frames: 30, timeoutSec: 30 },
                    ...hold("blue"),
                    ...barrier("saw-blue", "B"),
                    { op: "waitSignal", name: "unshared", from: "B", timeoutSec: 60 },
                    { op: "expectCosmetics", name: "B", none: true, timeoutSec: 10 },
                    { op: "expectDummyRecolor", name: "B", set: "kokiri", vanilla: true, frames: 15 },
                    { op: "expectDummyCosmetics", name: "B", vars: {
                        herosTunicTorsoColor: BLOCK, herosTunicCapColor: BLOCK, linkHairColor: BLOCK,
                        woodenSwordColor: BLOCK, ironBootsColor: BLOCK,
                    }, noSource: true, frames: 30 },
                    ...hold("white"),
                    ...barrier("saw-white", "B"),
                    { op: "waitSignal", name: "blue-tint", from: "B", timeoutSec: 60 },
                    { op: "expectDummyRecolor", name: "B", set: "kokiri", rgb: BLUE, frames: 15, timeoutSec: 10 },
                    { op: "expectDummyCosmetics", name: "B", vars: {
                        herosTunicTorsoColor: TINTED, linkHairColor: BLOCK,
                    }, noSource: true, frames: 15 },
                    { op: "cosmeticsOption", player: "B", choice: "mine" },
                    { op: "expectDummyCosmetics", name: "B", vars: {
                        herosTunicTorsoColor: TINTED, linkHairColor: { state: "none", source: "source" },
                    }, frames: 15, timeoutSec: 10 },
                    { op: "expectDummyRecolor", name: "B", set: "kokiri", rgb: BLUE, frames: 15 },
                    ...hold("mine"),
                    { op: "cosmeticsOption", player: "B", choice: "theirs" },
                    { op: "expectDummyCosmetics", name: "B", vars: { linkHairColor: BLOCK }, noSource: true,
                      frames: 15, timeoutSec: 10 },
                    ...barrier("saw-tint", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: faron,
                cvars: [...colorCvars(WHITE), ...SHARE_ROOM, ...cosCvars({ herosTunicTorsoColor: "1e88e5", linkHairColor: "1e88e5" })],
                steps: [
                    ...connect,
                    ...meetIn(faron, "A"),
                    ...stepAway,
                    ...barrier("saw-blue", "A"),
                    { op: "cosmeticsOption", share: "off" },
                    { op: "expectCosmetics", self: true, none: true, timeoutSec: 10 },
                    { op: "signal", name: "unshared" },
                    ...barrier("saw-white", "A"),
                    { op: "setColor", rgb: BLUE },
                    { op: "signal", name: "blue-tint" },
                    ...barrier("saw-tint", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "cosmetics-absent",
        description: "A has no cosmetics mod, B (red) shares a blue tunic and brown hair with the room: A still shows them (receiving needs no cosmetics mod), shares none itself, and with nothing of its own that could land on B's dummy registers no blockers; B sees A's blue tint and blocks the rest",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: faron,
                cvars: colorCvars(BLUE),
                steps: [
                    ...connect,
                    ...meetIn(faron, "B"),
                    { op: "expectCosmetics", name: "B", vars: { herosTunicTorsoColor: "1e88e5", linkHairColor: "91723e" },
                      timeoutSec: 20 },
                    { op: "expectCosmetics", self: true, none: true },
                    { op: "expectDummyCosmetics", name: "B", keys: "disc", vars: {
                        herosTunicTorsoColor: recolor("1e88e5"), linkHairColor: recolor("91723e"),
                        woodenSwordColor: { state: "none" }, ironBootsColor: { state: "none" },
                    }, frames: 15, timeoutSec: 30 },
                    { op: "expectDummyRecolor", name: "B", set: "kokiri", vanilla: true, frames: 15 },
                    ...barrier("checked", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: faron,
                enableMods: [MODS.cosmetics],
                cvars: [...colorCvars(RED), ...SHARE_ROOM, ...cosCvars({ herosTunicTorsoColor: "1e88e5", linkHairColor: "91723e" })],
                steps: [
                    ...connect,
                    ...meetIn(faron, "A"),
                    { op: "wait", sec: 3 },
                    { op: "expectCosmetics", name: "A", none: true, frames: 30 },
                    { op: "expectDummyRecolor", name: "A", set: "kokiri", rgb: BLUE, frames: 15 },
                    { op: "expectDummyCosmetics", name: "A", vars: {
                        herosTunicTorsoColor: TINTED, linkHairColor: BLOCK, woodenSwordColor: BLOCK,
                    }, frames: 15 },
                    ...barrier("checked", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "cosmetics-off",
        description: "B (red) shares a blue tunic with the room; A has red hair of its own: A turns Show Other Players' Cosmetics off and on, then picks Game Default for B (the Players tab tags B 'game default look') and back to Theirs; B turns Share My Looks off, then to My Team (both without a team, so A still gets it); each off shows B's red tint with A's hair blocked, each on the blue tunic again",
        timeoutSec: 420,
        cvars: COMMON_CVARS,
        enableMods: [MODS.cosmetics],
        instances: [
            {
                name: "A",
                start: faron,
                cvars: cosCvars({ linkHairColor: "ff0000" }),
                steps: [
                    ...connect,
                    ...meetIn(faron, "B"),
                    ...(function onOff() {
                        const blue = [
                            { op: "expectDummyRecolor", name: "B", set: "kokiri", vanilla: true, frames: 15, timeoutSec: 10 },
                            { op: "expectDummyCosmetics", name: "B", vars: { herosTunicTorsoColor: recolor("1e88e5") },
                              frames: 15, timeoutSec: 10 },
                        ];
                        const red = [
                            { op: "expectDummyRecolor", name: "B", set: "kokiri", rgb: RED, frames: 15, timeoutSec: 10 },
                            { op: "expectDummyCosmetics", name: "B", vars: {
                                herosTunicTorsoColor: TINTED, linkHairColor: BLOCK,
                            }, noSource: true, frames: 15, timeoutSec: 10 },
                        ];
                        return [
                            ...blue,
                            { op: "cosmeticsOption", show: false },
                            ...red,
                            { op: "cosmeticsOption", show: true },
                            ...blue,
                            { op: "cosmeticsOption", player: "B", choice: "default" },
                            ...red,
                            { op: "showWindow", tab: 2 },
                            { op: "expectPlayersTab", rows: [{ name: "B", text: "game default look", actions: true }],
                              timeoutSec: 5 },
                            { op: "hideWindow" },
                            { op: "cosmeticsOption", player: "B", choice: "theirs" },
                            ...blue,
                            ...barrier("a-done", "B"),
                            { op: "waitSignal", name: "unshared", from: "B", timeoutSec: 60 },
                            { op: "expectCosmetics", name: "B", none: true, timeoutSec: 10 },
                            ...red,
                            ...barrier("saw-unshared", "B"),
                            { op: "waitSignal", name: "shared", from: "B", timeoutSec: 60 },
                            { op: "expectCosmetics", name: "B", vars: { herosTunicTorsoColor: "1e88e5" }, timeoutSec: 10 },
                            ...blue,
                            ...barrier("saw-shared", "B"),
                        ];
                    })(),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: faron,
                cvars: [...colorCvars(RED), ...SHARE_ROOM, cos("herosTunicTorsoColor", "1e88e5")],
                steps: [
                    ...connect,
                    ...meetIn(faron, "A"),
                    ...stepAway,
                    ...barrier("a-done", "A"),
                    { op: "cosmeticsOption", share: "off" },
                    { op: "signal", name: "unshared" },
                    ...barrier("saw-unshared", "A"),
                    { op: "cosmeticsOption", share: "team" },
                    { op: "signal", name: "shared" },
                    ...barrier("saw-shared", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
];
