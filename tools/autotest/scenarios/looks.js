// Look sharing's host pieces: ModsService, base aliases and entry overlay churn (src/autotest/StepsLooks.cpp);
// look mods between players: detection, look states, per-player choices and the game's own archives on
// puppets (src/autotest/StepsLookShare.cpp).

const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const { MOD_ID, MODS, STAGES, COMMON_CVARS, tt, escapeModId, waitStage, barrier, meetIn, connect } = require("../lib");

const FIXTURE_A = path.join(__dirname, "..", "mods", "looks_a");
const sha256 = (bytes) => crypto.createHash("sha256").update(bytes).digest("hex");
const aOnly = fs.readFileSync(path.join(FIXTURE_A, "overlay", "tt_fixture", "a_only.bin"));
const cosmetic = (name, value) => `mod.${escapeModId(MODS.cosmetics)}.${name}=${value}`;
const RUNTIME_BYTES = "twili runtime overlay\n";
const faron = STAGES.southFaron;
const C = MODS.looksFixtureC;
const D = MODS.looksFixtureD;
// Fixtures C and D put entries under a root no archive has: nothing reads them, so they are no look.
const C_ARCHIVES = ["Kmdl", "Alink", "CWShd", "Wmdl", "Midna"];
// Our own entry overlays at the paths those archives read, one texel changed.
const ENTRIES = [["Kmdl", "al_head.bmd"], ["Alink", "al_swa.bmd"], ["CWShd", "al_shb.bmd"], ["Wmdl", "wl.bmd"],
    ["Midna", "s_md.bmd"]];
const entryOverrides = ENTRIES.map(([arc, bmd]) => ({ op: "looksEntryOverride", arc, bmd }));

module.exports = [
    {
        name: "looks-inventory",
        description: "ModsService lists two fixture mods (one wins their shared path), hashes and reads their files, "
            + "sees our runtime overlay, base alias and POINTER/SOURCE textures, reads the disc under an overlay and "
            + "gives out only allowlisted cosmetics vars; the alias opens and mounts as the disc file and reads an "
            + "entry overlay under its own directory that the real file does not",
        timeoutSec: 150,
        cvars: [...COMMON_CVARS, cosmetic("herosTunicCapColor", "C03020"), cosmetic("midnaHairBaseColor", 3)],
        enableMods: [MODS.cosmetics],
        instances: [{
            name: "solo",
            enableMods: [MODS.looksFixtureA, MODS.looksFixtureB],
            steps: [
                waitStage(STAGES.linksHouse),
                { op: "looksDump", hashes: true },
                {
                    op: "checkLooksInventory",
                    fixtureA: MODS.looksFixtureA,
                    fixtureB: MODS.looksFixtureB,
                    manifestMarker: "looks-a-marker",
                    sharedPath: "/tt_fixture/shared.bin",
                    aOnlyPath: "/tt_fixture/a_only.bin",
                    aOnlySize: aOnly.length,
                    aOnlySha: sha256(aOnly),
                    arcEntryPath: "/res/Object/Mmdl/tt_fixture.bin",
                    arcEntryArchive: "/res/Object/Mmdl.arc",
                    textureBundlePath: "textures/tex1_4x4_0123456789abcdef_6.png",
                    textureHash: "0123456789abcdef",
                    textureWidth: 4,
                    textureHeight: 4,
                    textureFormat: 6,
                    baseFile: "/res/Object/Mmdl.arc",
                    aliasPath: "/tt_autotest/alias/Mmdl.arc",
                    discOverlayFile: "/res/Object/E_fz.arc",
                    runtimePath: "/tt_autotest/runtime.bin",
                    runtimeBytes: RUNTIME_BYTES,
                    runtimeSha: sha256(Buffer.from(RUNTIME_BYTES)),
                    cosmetics: {
                        strings: { herosTunicCapColor: "C03020" },
                        ints: { midnaHairBaseColor: 3 },
                        refused: [
                            { mod: MODS.cosmetics, name: "aButtonColor" },
                            { mod: MODS.cosmetics, name: "enabled" },
                            { mod: MODS.looksFixtureA, name: "herosTunicCapColor" },
                            { mod: MOD_ID, name: "server_url" },
                        ],
                    },
                },
                { op: "quit" },
            ],
        }],
    },
    {
        name: "overlay-churn-alloc",
        description: "an AlAnm entry overlay stays installed through 50 overlay add/remove cycles; copy-style reads "
            + "reuse the archive's copy within a frame and each rebuild replaces it, so live copies do not grow",
        timeoutSec: 150,
        cvars: COMMON_CVARS,
        instances: [{
            name: "solo",
            steps: [
                waitStage(STAGES.linksHouse),
                { op: "overlayChurnAlloc", cycles: 50, maxLiveGrowth: 0 },
                { op: "looksDump" },
                { op: "quit" },
            ],
        }],
    },
    {
        name: "looks-detect",
        description: "A overlays Link's head, the Ordon Sword and Shield, the wolf and Midna at the paths their "
            + "archives read (one texel changed), replaces SWShd whole with the disc's archive with one texel changed, "
            + "and has fixtures C and D (the same names under a root no archive has; D's mod.json forbids sharing): "
            + "A's scan lists Link 4 files, wolf 1 and Midna 1 (shared, Share My Looks being Everyone in Room), Epona "
            + "as the game's, neither fixture as a look mod, and knows which archives are its own; B gets A's install "
            + "id and manifest with those states, files and mods, tags A's row with A's look, keeps its own default "
            + "archives for A's puppet, blocks A's look (6 files in looks.json), sees A's mod list once A shows it, "
            + "and no looks once A turns sharing off",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: faron,
                enableMods: [C, D],
                cvars: [tt("share_looks", 2)],
                steps: [
                    { op: "looksSelfTest" },
                    waitStage(faron),
                    ...entryOverrides,
                    { op: "looksArchiveOverride", arc: "SWShd", bmd: "al_shc.bmd" },
                    { op: "expectLocalLooks", types: {
                        link: { state: "shared", files: [["Kmdl", "al_head.bmd"], ["Alink", "al_swa.bmd"],
                            ["CWShd", "al_shb.bmd"], ["SWShd", "al_shc.bmd"]] },
                        wolf: { state: "shared", files: [["Wmdl", "wl.bmd"]] },
                        midna: { state: "shared", files: [["Midna", "s_md.bmd"]] },
                        epona: { state: "vanilla", files: [] },
                    }, mods: {
                        [MOD_ID]: { types: ["link", "wolf", "midna"], shareable: true },
                    }, notMods: [C, D, MODS.cosmetics], dirty: [...C_ARCHIVES, "SWShd", "Horse"], timeoutSec: 20 },
                    ...connect,
                    ...meetIn(faron, "B"),
                    { op: "expectLocalLooks", sent: { link: "shared", wolf: "shared", midna: "shared", epona: "vanilla" },
                      timeoutSec: 20 },
                    ...barrier("sent", "B"),
                    { op: "lookOption", modList: true },
                    ...barrier("mod-list", "B"),
                    { op: "lookOption", share: "off" },
                    { op: "expectLocalLooks", sent: [], types: { link: { state: "unshared" } }, timeoutSec: 10 },
                    ...barrier("unshared", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: faron,
                steps: [
                    ...connect,
                    ...meetIn(faron, "A"),
                    { op: "expectPeerLooks", name: "A", installId: true, types: {
                        link: { state: "shared", files: 4, mods: [MOD_ID] },
                        wolf: { state: "shared", files: 1, mods: [MOD_ID] },
                        midna: { state: "shared", files: 1, mods: [MOD_ID] },
                        epona: { state: "vanilla" },
                    }, tag: "look: Twili-Together", noTag: "unshared", timeoutSec: 20 },
                    { op: "expectDummyArcPath", name: "A", base: [], frames: 15, timeoutSec: 10 },
                    { op: "showWindow", tab: 2 },
                    { op: "expectPlayersTab", rows: [{ name: "A", text: "look: Twili-Together", actions: true }],
                      timeoutSec: 5 },
                    { op: "hideWindow" },
                    { op: "lookOption", player: "A", block: true },
                    { op: "expectPeerLooks", name: "A", tag: "look blocked: Twili-Together", timeoutSec: 5 },
                    { op: "expectLooksFile", player: "A", none: true, blocked: 6, timeoutSec: 5 },
                    ...barrier("sent", "A"),
                    { op: "expectPeerLooks", name: "A", modList: [C, D, MOD_ID], timeoutSec: 10 },
                    ...barrier("mod-list", "A"),
                    { op: "expectPeerLooks", name: "A", none: true, modList: [C, D, MOD_ID], timeoutSec: 10 },
                    ...barrier("unshared", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "looks-default",
        description: "A overlays Kmdl, Alink, CWShd, Wmdl and Midna entries at their real paths and has a "
            + "content-keyed texture of its own on the Hylian Shield; B uses no look mod and shares with the room, so "
            + "it lists the game's look for every type. A's puppet of B mounts the disc's own copies "
            + "(/res/Object/tt/base) of exactly the archives A's mods touch, and no texture of it renders from a "
            + "content replacement (the shield's is blocked); My Own respawns it on the default paths with A's "
            + "texture showing; Theirs with Wolf: My Own keeps Wmdl on its default path; looks.json keeps the choices "
            + "under B's install id; once B stops sharing, the fallback setting decides (My Own, with A's texture "
            + "showing, then Game Default); Game Default for all at the end",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: faron,
                steps: [
                    waitStage(faron),
                    ...entryOverrides,
                    { op: "looksSourceTexture", register: { arc: "HyShd", bmd: "al_sha.bmd" } },
                    ...connect,
                    ...meetIn(faron, "B"),
                    { op: "expectPeerLooks", name: "B", installId: true, types: {
                        link: { state: "vanilla" }, wolf: { state: "vanilla" }, midna: { state: "vanilla" },
                        epona: { state: "vanilla" },
                    }, timeoutSec: 20 },
                    { op: "expectDummyArcPath", name: "B", base: C_ARCHIVES, frames: 15, timeoutSec: 30 },
                    { op: "expectDummyCosmetics", name: "B", noSource: true, frames: 15, timeoutSec: 20 },
                    { op: "looksSourceTexture", name: "B", source: "pointer", frames: 5, timeoutSec: 10 },
                    { op: "lookOption", player: "B", choice: "mine" },
                    { op: "expectDummyArcPath", name: "B", base: [], frames: 15, timeoutSec: 30 },
                    { op: "looksSourceTexture", name: "B", source: "source", frames: 5, timeoutSec: 20 },
                    { op: "lookOption", player: "B", choice: "theirs" },
                    { op: "lookOption", player: "B", type: "wolf", choice: "mine" },
                    { op: "expectDummyArcPath", name: "B", base: ["Kmdl", "Alink", "CWShd", "Midna"], frames: 15,
                      timeoutSec: 30 },
                    { op: "looksSourceTexture", name: "B", source: "pointer", frames: 5, timeoutSec: 20 },
                    { op: "showWindow", tab: 2 },
                    { op: "expectPlayersTab", rows: [{ name: "B", text: "Wolf: My Own", actions: true }], timeoutSec: 5 },
                    { op: "hideWindow" },
                    { op: "expectLooksFile", player: "B", look: "theirs", types: { wolf: "mine" }, timeoutSec: 5 },
                    ...barrier("a-choices", "B"),
                    ...barrier("b-off", "B"),
                    { op: "expectPeerLooks", name: "B", none: true, timeoutSec: 10 },
                    // Nothing listed: the fallback (My Own by default); a manifest's respawn waits up to 10 s.
                    { op: "expectDummyArcPath", name: "B", base: [], frames: 15, timeoutSec: 30 },
                    { op: "looksSourceTexture", name: "B", source: "source", frames: 5, timeoutSec: 20 },
                    { op: "lookOption", fallback: "default" },
                    { op: "expectDummyArcPath", name: "B", base: ["Kmdl", "Alink", "CWShd", "Midna"], frames: 15,
                      timeoutSec: 30 },
                    { op: "lookOption", player: "B", choice: "default" },
                    { op: "lookOption", player: "B", type: "wolf", choice: "same" },
                    { op: "expectDummyArcPath", name: "B", base: C_ARCHIVES, frames: 15, timeoutSec: 30 },
                    { op: "expectDummyCosmetics", name: "B", noSource: true, frames: 15, timeoutSec: 20 },
                    { op: "showWindow", tab: 2 },
                    { op: "expectPlayersTab", rows: [{ name: "B", text: "game default look", actions: true }],
                      timeoutSec: 5 },
                    { op: "hideWindow" },
                    { op: "expectLooksFile", player: "B", look: "default", types: {}, timeoutSec: 5 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: faron,
                cvars: [tt("share_looks", 2)],
                steps: [
                    ...connect,
                    ...meetIn(faron, "A"),
                    { op: "walk", frames: 40, stickY: 1 },
                    { op: "expectLocalLooks", sent: { link: "vanilla", wolf: "vanilla", midna: "vanilla", epona: "vanilla" },
                      timeoutSec: 20 },
                    ...barrier("a-choices", "A"),
                    { op: "lookOption", share: "off" },
                    { op: "expectLocalLooks", sent: [], timeoutSec: 10 },
                    ...barrier("b-off", "A"),
                    ...barrier("done", "A", 240),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "looks-stopgap",
        description: "A makes base copies the way a host without add_base_alias does (reading the archive from "
            + "the disc and overlaying it as a buffer), overlays a Kmdl entry at its real path and replaces SWShd "
            + "whole; B shares the game's look: A's puppet of B mounts Kmdl from a copy read from the disc with "
            + "exactly the disc's bytes, and SWShd from its default path, as the stopgap cannot read under a "
            + "whole-archive override of ours",
        timeoutSec: 240,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: faron,
                steps: [
                    { op: "looksBaseCopy", stopgap: true },
                    waitStage(faron),
                    { op: "looksEntryOverride", arc: "Kmdl", bmd: "al_head.bmd" },
                    { op: "looksArchiveOverride", arc: "SWShd", bmd: "al_shc.bmd" },
                    ...connect,
                    ...meetIn(faron, "B"),
                    { op: "expectPeerLooks", name: "B", types: { link: { state: "vanilla" } }, timeoutSec: 20 },
                    { op: "expectDummyArcPath", name: "B", base: ["Kmdl"], frames: 15, timeoutSec: 30 },
                    { op: "looksBaseCopy", arc: "Kmdl", fromDisc: true, timeoutSec: 10 },
                    { op: "looksBaseCopy", arc: "SWShd", failed: true, timeoutSec: 10 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: faron,
                cvars: [tt("share_looks", 2)],
                steps: [
                    ...connect,
                    ...meetIn(faron, "A"),
                    ...barrier("done", "A", 180),
                    { op: "quit" },
                ],
            },
        ],
    },
];
