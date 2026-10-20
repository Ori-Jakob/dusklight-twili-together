// Look sharing's host pieces: ModsService, base aliases and entry overlay churn (src/autotest/StepsLooks.cpp).

const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const { MOD_ID, MODS, STAGES, COMMON_CVARS, escapeModId, waitStage } = require("../lib");

const FIXTURE_A = path.join(__dirname, "..", "mods", "looks_a");
const sha256 = (bytes) => crypto.createHash("sha256").update(bytes).digest("hex");
const aOnly = fs.readFileSync(path.join(FIXTURE_A, "overlay", "tt_fixture", "a_only.bin"));
const cosmetic = (name, value) => `mod.${escapeModId(MODS.cosmetics)}.${name}=${value}`;
const RUNTIME_BYTES = "twili runtime overlay\n";

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
            + "after each rebuild allocate no entry overlay copies",
        timeoutSec: 150,
        cvars: COMMON_CVARS,
        instances: [{
            name: "solo",
            steps: [
                waitStage(STAGES.linksHouse),
                { op: "overlayChurnAlloc", cycles: 50, maxGrowth: 0 },
                { op: "looksDump" },
                { op: "quit" },
            ],
        }],
    },
];
