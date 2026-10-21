// Steps for look mods between players: detection, look states, the per-player choices and the
// game's own archives on puppets; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "actors/DummyHorse.hpp"
#include "actors/DummyPlayer.hpp"
#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "fx/CosmeticsRecolor.hpp"
#include "looks/Archive.hpp"
#include "looks/Choices.hpp"
#include "looks/Cosmetics.hpp"
#include "looks/Detect.hpp"
#include "looks/Inventory.hpp"
#include "looks/Looks.hpp"
#include "looks/Sha256.hpp"

#include <mods/svc/overlay.h>
#include <mods/svc/texture.h>

#include <dolphin/gx.h>
#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace twili::autotest {
namespace {

using nlohmann::json;
using looks::LookArc;
using looks::LookType;

uint32_t clientIdByName(const std::string& name) {
    for (const auto& [id, c] : Session::instance().clients()) {
        if (!c.self && c.name == name) {
            return id;
        }
    }
    return 0;
}

int sMatchedAt = -1;

// Holds `frames` ticks once it matches; fails on a break after that or at the timeout.
bool holdMatch(StepContext& ctx, const std::string& op, const std::string& why, const std::string& detail) {
    if (!ctx.begun) {
        sMatchedAt = -1;
    }
    if (why.empty()) {
        if (sMatchedAt < 0) {
            sMatchedAt = ctx.ticks;
        }
        if (ctx.ticks - sMatchedAt < ctx.step.value("frames", 0)) {
            return false;
        }
        TwiliLog.info("[autotest] {}: {}", op, detail);
        return true;
    }
    if (sMatchedAt >= 0) {
        ctx.fail(fmt::format("{}: {} ticks after it matched, {} ({})", op, ctx.ticks - sMatchedAt, why, detail));
        return false;
    }
    if (ctx.seconds > ctx.timeout(30.0)) {
        ctx.fail(fmt::format("{}: {} ({})", op, why, detail));
    }
    return false;
}

// --- XXH64 (seed 0): the content key Aurora gives a texture's base mip

constexpr uint64_t kP1 = 0x9E3779B185EBCA87ull, kP2 = 0xC2B2AE3D27D4EB4Full, kP3 = 0x165667B19E3779F9ull,
                   kP4 = 0x85EBCA77C2B2AE63ull, kP5 = 0x27D4EB2F165667C5ull;

uint64_t rotl(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

uint64_t read64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = v << 8 | p[i];
    }
    return v;
}

uint64_t xxRound(uint64_t acc, uint64_t input) {
    return rotl(acc + input * kP2, 31) * kP1;
}

uint64_t xxMerge(uint64_t acc, uint64_t val) {
    return (acc ^ xxRound(0, val)) * kP1 + kP4;
}

uint64_t xxh64(const uint8_t* p, size_t len) {
    const uint8_t* end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = kP1 + kP2, v2 = kP2, v3 = 0, v4 = 0 - kP1;
        for (; p + 32 <= end; p += 32) {
            v1 = xxRound(v1, read64(p));
            v2 = xxRound(v2, read64(p + 8));
            v3 = xxRound(v3, read64(p + 16));
            v4 = xxRound(v4, read64(p + 24));
        }
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = xxMerge(xxMerge(xxMerge(xxMerge(h, v1), v2), v3), v4);
    } else {
        h = kP5;
    }
    h += len;
    for (; p + 8 <= end; p += 8) {
        h = rotl(h ^ xxRound(0, read64(p)), 27) * kP1 + kP4;
    }
    if (p + 4 <= end) {
        const uint64_t k = static_cast<uint64_t>(p[0]) | static_cast<uint64_t>(p[1]) << 8 |
                           static_cast<uint64_t>(p[2]) << 16 | static_cast<uint64_t>(p[3]) << 24;
        h = rotl(h ^ k * kP1, 23) * kP2 + kP3;
        p += 4;
    }
    for (; p < end; p++) {
        h = rotl(h ^ *p * kP5, 11) * kP1;
    }
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

// --- the disc's own archives

std::optional<std::vector<uint8_t>> discArchive(LookArc arc) {
    auto raw = looks::readBaseDiscFile(looks::arcDefaultPath(arc));
    if (!raw) {
        return std::nullopt;
    }
    if (!looks::isYaz0(*raw)) {
        return raw;
    }
    return looks::yaz0Decode(*raw, 64u << 20);
}

const looks::Tex1Entry* firstPlainTexture(const std::vector<looks::Tex1Entry>& list) {
    const auto it = std::ranges::find_if(list, [](const looks::Tex1Entry& t) {
        return !t.palette && t.imageBytes != 0 && looks::gxImageBytes(t.format, t.width, t.height, 1) != 0;
    });
    return it == list.end() ? nullptr : &*it;
}

std::optional<LookArc> arcOf(const json& step, const char* key = "arc") {
    return looks::arcByName(step.value(key, std::string{}));
}

// --- self test

std::string selfTest() {
    const auto sha = [](std::string_view text) {
        return looks::hex(looks::sha256({reinterpret_cast<const uint8_t*>(text.data()), text.size()}));
    };
    if (sha("") != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ||
        sha("abc") != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ||
        sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") !=
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")
    {
        return "sha256 test vectors";
    }
    uint8_t texels[64];
    std::memset(texels, 0x3C, sizeof(texels));
    if (xxh64(texels, sizeof(texels)) != 0xF2FA84DAA598697Aull) {
        return "xxh64 of 64 bytes of 0x3C";
    }
    if (looks::entryType(LookArc::Kmdl, "AL_HEAD.BMD") != LookType::Link ||
        looks::entryType(LookArc::Wmdl, "md.bmd") != LookType::Midna ||
        looks::entryType(LookArc::Wmdl, "wl.bmd") != LookType::Wolf ||
        looks::entryType(LookArc::Kmdl, "al_unlisted.bin") || looks::entryType(LookArc::Horse, "al.bmd") ||
        looks::arcByPath("/res/object/KMDL.arc") != LookArc::Kmdl || looks::arcByPath("/res/Object/Kmdl/al.bmd"))
    {
        return "the allowlist";
    }
    if (!looks::canReceiveLooks()) {
        return {};
    }
    // A client id no relay hands out; updateLooks forgets it.
    constexpr uint32_t kFake = 0xFFFFFFF0u;
    const std::string good(64, 'a');
    const auto manifest = [&](json files) {
        return json::array({{{"slot", "link"}, {"state", "shared"},
                                {"mods", json::array({{{"id", "x"}, {"name", std::string(100, 'n')}}})},
                                {"files", std::move(files)}},
            {{"slot", "wolf"}, {"state", "vanilla"}}, {{"slot", "bogus"}, {"state", "shared"}}});
    };
    looks::takePeerLooks(kFake, manifest(json::array({{{"arc", "Kmdl"}, {"name", "AL.bmd"}, {"sha", good}, {"size", 10}},
        {{"arc", "Wmdl"}, {"name", "wl.bmd"}, {"sha", good}, {"size", 10}},
        {{"arc", "Kmdl"}, {"name", "../x.bmd"}, {"sha", good}, {"size", 10}}})));
    const looks::PeerLooks* peer = looks::peerLooks(kFake);
    std::string why;
    if (peer == nullptr || !peer->slots[0] || peer->slots[0]->files.size() != 1 ||
        peer->slots[0]->files[0].name != "al.bmd" || peer->slots[0]->mods.at(0).name.size() != 64 ||
        !peer->slots[1] || peer->slots[1]->state != looks::SlotState::Vanilla || peer->slots[2])
    {
        why = "a manifest's look entries";
    }
    looks::takePeerLooks(kFake, manifest(json::array({{{"arc", "Kmdl"}, {"name", "al.bmd"}, {"sha", "../../x"}, {"size", 10}}})));
    if (why.empty() && looks::peerLooks(kFake) != nullptr) {
        why = "a bad sha did not drop the looks";
    }
    looks::takePeerLooks(kFake, manifest(json::array({{{"arc", "Kmdl"}, {"name", "al.bmd"}, {"sha", good}, {"size", 9u << 20}}})));
    if (why.empty() && (looks::peerLooks(kFake) == nullptr || !looks::peerLooks(kFake)->slots[0] ||
                           looks::peerLooks(kFake)->slots[0]->state != looks::SlotState::Unshared))
    {
        why = "a file over 8 MB was kept";
    }
    looks::takePeerLooks(kFake, json::array());
    return why;
}

// --- options

bool lookOption(StepContext& ctx) {
    const json& step = ctx.step;
    if (step.contains("share")) {
        const std::string share = step.value("share", std::string{});
        const int64_t scope = share == "off" ? 0 : share == "team" ? 1 : share == "room" ? 2 : -1;
        if (scope < 0) {
            ctx.fail("lookOption: share is off, team or room");
            return true;
        }
        config::setInt(config::Var::ShareLooks, scope);
    }
    if (step.contains("show")) {
        config::setBool(config::Var::ShowLooks, step.value("show", false));
    }
    if (step.contains("fallback")) {
        config::setInt(config::Var::LooksFallback, step.value("fallback", std::string{}) == "default" ? 1 : 0);
    }
    if (step.contains("modList")) {
        config::setBool(config::Var::ShowModList, step.value("modList", false));
    }
    if (step.contains("allowPacks")) {
        config::setBool(config::Var::AllowMyTexturePacks, step.value("allowPacks", false));
    }
    if (!step.contains("player")) {
        return true;
    }
    const uint32_t id = clientIdByName(step.value("player", std::string{}));
    if (id == 0) {
        ctx.fail("lookOption: no such peer");
        return true;
    }
    if (step.contains("block")) {
        looks::setLookBlocked(id, step.value("block", true));
    }
    if (!step.contains("choice")) {
        return true;
    }
    const std::string choiceText = step.value("choice", std::string{});
    const auto choice = looks::parseLookChoice(choiceText);
    if (step.contains("type")) {
        const auto type = looks::parseLookType(step.value("type", std::string{}));
        if (!type || (!choice && choiceText != "same")) {
            ctx.fail("lookOption: type is link, wolf, midna or epona; choice theirs, default, mine or same");
            return true;
        }
        looks::setTypeChoice(id, *type, choice);
    } else if (!choice) {
        ctx.fail("lookOption: choice is theirs, default or mine");
    } else {
        looks::setLookChoice(id, *choice);
    }
    return true;
}

// --- a whole-archive override of ours: the disc's archive with one texel changed

OverlayHandle sArchiveOverride = 0;

bool archiveOverride(StepContext& ctx) {
    if (ctx.step.value("remove", false)) {
        if (sArchiveOverride != 0 && svc_overlay != nullptr) {
            svc_overlay->remove(mod_ctx, sArchiveOverride);
        }
        sArchiveOverride = 0;
        return true;
    }
    const auto arc = arcOf(ctx.step);
    const std::string bmd = ctx.step.value("bmd", std::string{});
    auto bytes = arc ? discArchive(*arc) : std::nullopt;
    const auto files = bytes ? looks::rarcFiles(*bytes) : std::nullopt;
    if (svc_overlay == nullptr || !files) {
        ctx.fail("looksArchiveOverride: no OverlayService or the archive is unreadable");
        return true;
    }
    const auto file = std::ranges::find_if(*files, [&](const looks::RarcFile& f) { return looks::iequals(f.name, bmd); });
    if (file == files->end() || (file->flags & 0x84) != 0) {
        ctx.fail(fmt::format("looksArchiveOverride: {} missing or compressed", bmd));
        return true;
    }
    const std::span<const uint8_t> model(bytes->data() + file->offset, file->size);
    const auto textures = looks::bmdTextures(model);
    const looks::Tex1Entry* tex = textures ? firstPlainTexture(*textures) : nullptr;
    if (tex == nullptr) {
        ctx.fail(fmt::format("looksArchiveOverride: {} has no plain texture", bmd));
        return true;
    }
    (*bytes)[file->offset + tex->imageOffset] ^= 0xFF;
    const std::span<const uint8_t> changed(bytes->data() + file->offset, file->size);
    if (svc_overlay->add_buffer(mod_ctx, looks::arcDefaultPath(*arc), bytes->data(), bytes->size(), &sArchiveOverride) !=
        MOD_OK)
    {
        ctx.fail("looksArchiveOverride: add_buffer refused it");
        return true;
    }
    TwiliLog.info("[autotest] looks: {} replaced whole, {} texture {} changed, sha256 {}", looks::arcDefaultPath(*arc),
        bmd, tex->name, looks::hex(looks::sha256(changed)));
    return true;
}

// --- our looks

std::string typeList(uint8_t bits) {
    std::string out;
    for (size_t t = 0; t < looks::kLookTypeCount; t++) {
        if ((bits >> t) & 1) {
            out += fmt::format("{}{}", out.empty() ? "" : ",", looks::lookTypeId(static_cast<LookType>(t)));
        }
    }
    return out;
}

std::string localText() {
    const looks::LocalLooks& l = looks::localLooks();
    std::string s = fmt::format("known={} dirty={:#x} whole={:#x}", l.known, l.dirtyArcs, l.wholeArcs);
    const looks::ShareScope scope = looks::shareScope();
    for (size_t t = 0; t < looks::kLookTypeCount; t++) {
        const looks::OwnSlot slot = looks::ownSlot(static_cast<LookType>(t), scope);
        s += fmt::format(" [{} {}{}:", looks::lookTypeId(static_cast<LookType>(t)), looks::slotStateId(slot.state),
            slot.why.empty() ? "" : " (" + slot.why + ")");
        for (const looks::LookFile& f : l.files[t]) {
            s += fmt::format(" {}/{}", looks::arcName(f.arc), f.name);
        }
        s += "]";
    }
    for (const looks::LookMod& m : l.mods) {
        s += fmt::format(" mod {} {} {}B{}", m.id, typeList(m.types), m.bytes, m.shareable ? "" : " unshareable");
    }
    return s + " sent " + looks::manifestLooksSummary(looks::sentLooks());
}

std::string localMismatch(const json& step) {
    const looks::LocalLooks& l = looks::localLooks();
    if (!l.known) {
        return "no scan finished";
    }
    const looks::ShareScope scope = looks::shareScope();
    for (const auto& [typeId, spec] : step.value("types", json::object()).items()) {
        const auto type = looks::parseLookType(typeId);
        if (!type) {
            return "unknown type " + typeId;
        }
        const looks::OwnSlot slot = looks::ownSlot(*type, scope);
        if (spec.contains("state") && spec.value("state", std::string{}) != looks::slotStateId(slot.state)) {
            return fmt::format("{} is {}", typeId, looks::slotStateId(slot.state));
        }
        if (spec.contains("files")) {
            std::set<std::string> want, have;
            for (const auto& f : spec["files"]) {
                want.insert(looks::arcByName(f.at(0).get<std::string>()) ?
                                fmt::format("{}/{}", looks::arcName(*looks::arcByName(f.at(0).get<std::string>())),
                                    f.at(1).get<std::string>()) :
                                "?" + f.at(0).get<std::string>());
            }
            for (const looks::LookFile& f : l.files[static_cast<size_t>(*type)]) {
                have.insert(fmt::format("{}/{}", looks::arcName(f.arc), f.name));
            }
            if (want != have) {
                return fmt::format("{} has {} files, not the ones expected", typeId, have.size());
            }
        }
    }
    for (const auto& [id, spec] : step.value("mods", json::object()).items()) {
        const auto mod = std::ranges::find_if(l.mods, [&](const looks::LookMod& m) { return m.id == id; });
        if (mod == l.mods.end()) {
            return id + " is not a look mod";
        }
        if (spec.contains("types")) {
            uint8_t bits = 0;
            for (const auto& t : spec["types"]) {
                if (const auto type = looks::parseLookType(t.get<std::string>())) {
                    bits |= static_cast<uint8_t>(1u << static_cast<uint32_t>(*type));
                }
            }
            if (bits != mod->types) {
                return fmt::format("{} changes {}", id, typeList(mod->types));
            }
        }
        if (spec.contains("shareable") && spec.value("shareable", true) != mod->shareable) {
            return fmt::format("{} shareable={}", id, mod->shareable);
        }
    }
    for (const auto& id : step.value("notMods", json::array())) {
        if (std::ranges::any_of(l.mods, [&](const looks::LookMod& m) { return m.id == id.get<std::string>(); })) {
            return id.get<std::string>() + " counts as a look mod";
        }
    }
    if (step.contains("dirty")) {
        uint32_t want = 0;
        for (const auto& a : step["dirty"]) {
            if (const auto arc = looks::arcByName(a.get<std::string>())) {
                want |= 1u << static_cast<uint32_t>(*arc);
            }
        }
        if (want != l.dirtyArcs) {
            return fmt::format("ours in archives {:#x}, want {:#x}", l.dirtyArcs, want);
        }
    }
    if (step.contains("sent")) {
        const json& sent = looks::sentLooks();
        const auto& want = step["sent"];
        if (want.is_null() || (want.is_array() && want.empty())) {
            if (!sent.empty()) {
                return "sent " + looks::manifestLooksSummary(sent);
            }
        } else {
            for (const auto& [typeId, state] : want.items()) {
                const auto it = std::find_if(sent.begin(), sent.end(),
                    [&](const json& e) { return e.value("slot", std::string{}) == typeId; });
                if (it == sent.end() || it->value("state", std::string{}) != state.get<std::string>()) {
                    return "sent " + looks::manifestLooksSummary(sent);
                }
            }
        }
    }
    return {};
}

// --- theirs

std::string peerText(uint32_t id) {
    const Client& c = Session::instance().clients().at(id);
    std::string s = fmt::format("installId='{}' mods={}", c.installId, c.modList.size());
    if (const looks::PeerLooks* peer = looks::peerLooks(id)) {
        for (size_t t = 0; t < looks::kLookTypeCount; t++) {
            if (const auto& slot = peer->slots[t]) {
                s += fmt::format(" [{} {} {} files {} B]", looks::lookTypeId(static_cast<LookType>(t)),
                    looks::slotStateId(slot->state), slot->files.size(), slot->bytes);
            }
        }
    } else {
        s += " no looks";
    }
    return s + " tag='" + looks::looksPlayerTag(id) + "'";
}

std::string peerMismatch(const json& step, uint32_t id) {
    const Client& c = Session::instance().clients().at(id);
    const looks::PeerLooks* peer = looks::peerLooks(id);
    if (step.value("none", false) && peer != nullptr) {
        return "lists looks";
    }
    for (const auto& [typeId, spec] : step.value("types", json::object()).items()) {
        const auto type = looks::parseLookType(typeId);
        const auto* slot = peer != nullptr && type ? &peer->slots[static_cast<size_t>(*type)] : nullptr;
        if (slot == nullptr || !*slot) {
            return typeId + " not listed";
        }
        if (spec.contains("state") && spec.value("state", std::string{}) != looks::slotStateId((*slot)->state)) {
            return fmt::format("{} is {}", typeId, looks::slotStateId((*slot)->state));
        }
        if (spec.contains("files") && spec.value("files", size_t{0}) != (*slot)->files.size()) {
            return fmt::format("{} has {} files", typeId, (*slot)->files.size());
        }
        for (const auto& mod : spec.value("mods", json::array())) {
            if (std::ranges::none_of((*slot)->mods, [&](const looks::PeerMod& m) { return m.id == mod.get<std::string>(); })) {
                return fmt::format("{} lacks mod {}", typeId, mod.get<std::string>());
            }
        }
    }
    if (step.value("installId", false) && !looks::validInstallId(c.installId)) {
        return "no install id";
    }
    if (step.contains("modList")) {
        const auto& want = step["modList"];
        if (want.empty() != c.modList.empty()) {
            return fmt::format("lists {} mods", c.modList.size());
        }
        for (const auto& m : want) {
            if (std::ranges::none_of(c.modList, [&](const ListedMod& l) { return l.id == m.get<std::string>(); })) {
                return "mod list lacks " + m.get<std::string>();
            }
        }
    }
    const std::string tag = looks::looksPlayerTag(id);
    if (step.contains("tag") && tag.find(step.value("tag", std::string{})) == std::string::npos) {
        return "tag lacks '" + step.value("tag", std::string{}) + "'";
    }
    if (step.contains("noTag") && tag.find(step.value("noTag", std::string{})) != std::string::npos) {
        return "tag has '" + step.value("noTag", std::string{}) + "'";
    }
    return {};
}

// --- puppets

bool puppetPaths(const json& step, std::vector<std::string>& paths, std::string& why) {
    const uint32_t id = clientIdByName(step.value("name", std::string{}));
    if (id == 0) {
        why = "no such peer";
        return false;
    }
    if (step.value("horse", false)) {
        daDummyHorse_c* horse = FindDummyHorse(id);
        if (horse == nullptr || horse->arcPath() == nullptr) {
            why = "no horse puppet";
            return false;
        }
        paths.assign(static_cast<size_t>(LookArc::Horse) + 1, std::string{});
        paths.back() = horse->arcPath();
        return true;
    }
    fopAc_ac_c* dummy = Session::instance().dummyActorForClient(id);
    if (dummy == nullptr || !GetDummyPlayerArcPaths(dummy, paths)) {
        why = "no dummy";
        return false;
    }
    return true;
}

std::string pathsMismatch(const json& step, const std::vector<std::string>& paths) {
    std::set<std::string> base;
    for (const auto& a : step.value("base", json::array())) {
        const auto arc = looks::arcByName(a.get<std::string>());
        if (!arc) {
            return "unknown archive " + a.get<std::string>();
        }
        base.insert(looks::arcName(*arc));
    }
    for (size_t i = 0; i < paths.size(); i++) {
        const auto arc = static_cast<LookArc>(i);
        if (paths[i].empty()) {
            continue;
        }
        const bool wantBase = base.contains(looks::arcName(arc));
        if (paths[i] != (wantBase ? looks::arcAliasPath(arc) : looks::arcDefaultPath(arc))) {
            return fmt::format("{} from {}", looks::arcName(arc), paths[i]);
        }
    }
    return {};
}

std::string pathsText(const std::vector<std::string>& paths) {
    std::string s;
    for (const std::string& p : paths) {
        if (!p.empty()) {
            s += " " + p;
        }
    }
    return s;
}

// --- a content-keyed texture of ours on a texture puppets load

struct SourceTexture {
    std::string name;
    TextureReplacementHandle handle = 0;
};
SourceTexture sSource;

bool registerSourceTexture(StepContext& ctx) {
    const json& spec = ctx.step["register"];
    const auto arc = arcOf(spec);
    const auto bytes = arc ? discArchive(*arc) : std::nullopt;
    const auto bmd = bytes ? looks::rarcRead(*bytes, spec.value("bmd", std::string{}), 16u << 20) : std::nullopt;
    const auto textures = bmd ? looks::bmdTextures(*bmd) : std::nullopt;
    const looks::Tex1Entry* tex = textures ? firstPlainTexture(*textures) : nullptr;
    if (svc_texture == nullptr || !looks::available() || tex == nullptr) {
        ctx.fail("looksSourceTexture: no TextureService or ModsService, or no plain texture in that model");
        return true;
    }
    const uint32_t base = looks::gxImageBytes(tex->format, tex->width, tex->height, 1);
    std::vector<uint8_t> texels(bmd->begin() + tex->imageOffset, bmd->begin() + tex->imageOffset + base);
    TextureKey key = TEXTURE_KEY_INIT;
    key.kind = TEXTURE_KEY_SOURCE;
    key.texture_hash = xxh64(texels.data(), texels.size());
    key.width = tex->width;
    key.height = tex->height;
    key.gx_format = tex->format;
    std::vector<uint8_t> replacement(base, 0xFF);
    TextureData data = TEXTURE_DATA_INIT;
    data.data = replacement.data();
    data.size = replacement.size();
    data.width = tex->width;
    data.height = tex->height;
    data.gx_format = tex->format;
    if (svc_texture->register_data(mod_ctx, &key, &data, &sSource.handle) != MOD_OK) {
        ctx.fail("looksSourceTexture: register_data refused it");
        return true;
    }
    sSource.name = tex->name;
    // The key has to match the disc's bytes, or the check below proves nothing.
    std::vector<uint8_t> aligned(base + 32);
    uint8_t* copy = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(aligned.data()) + 31) & ~uintptr_t{31});
    std::memcpy(copy, texels.data(), base);
    GXTexObj obj;
    GXInitTexObj(&obj, copy, tex->width, tex->height, static_cast<GXTexFmt>(tex->format), GX_CLAMP, GX_CLAMP, GX_FALSE);
    int source = 0;
#if TWILI_HAS_MODS_SERVICE
    ModTextureSource found = MOD_TEXTURE_SOURCE_NONE;
    svc_mods->debug_texture_source(mod_ctx, &obj, nullptr, &found);
    source = static_cast<int>(found);
#endif
    TwiliLog.info("[autotest] looks: SOURCE key {:016x} {}x{} fmt {} for {} in {}/{}, a copy renders from {}",
        key.texture_hash, key.width, key.height, key.gx_format, sSource.name, looks::arcName(*arc),
        spec.value("bmd", std::string{}), source);
    // 2: MOD_TEXTURE_SOURCE_SOURCE
    if (source != 2) {
        ctx.fail("looksSourceTexture: a copy of the disc's texels does not render from the new key");
    }
    return true;
}

std::string sourceMismatch(const json& step, std::string& detail) {
    const uint32_t id = clientIdByName(step.value("name", std::string{}));
    if (sSource.name.empty()) {
        return "no texture registered";
    }
    CosmeticsProbe probe;
    if (step.value("horse", false)) {
        daDummyHorse_c* horse = id != 0 ? FindDummyHorse(id) : nullptr;
        if (horse == nullptr) {
            return "no horse puppet";
        }
        horse->getCosmeticsProbe(probe);
    } else {
        fopAc_ac_c* dummy = id != 0 ? Session::instance().dummyActorForClient(id) : nullptr;
        if (dummy == nullptr || !GetDummyPlayerCosmeticsProbe(dummy, probe)) {
            return "no dummy";
        }
    }
    // Another model may have a texture of the same name: "source" needs one of them to show our
    // replacement, "pointer" and "none" need all of them so.
    const std::string want = step.value("source", std::string{});
    size_t seen = 0, matching = 0, blocked = 0, sources = 0;
    std::string have;
    for (const CosmeticsTexProbe& t : probe.tex) {
        blocked += t.state == CosmeticsTexState::Block ? 1 : 0;
        sources += t.source == 2 ? 1 : 0;
        if (t.texture == nullptr || sSource.name != t.texture) {
            continue;
        }
        seen++;
        const char* source = t.source == 1 ? "pointer" : t.source == 2 ? "source" : t.source == 0 ? "none" : "unknown";
        matching += want == source ? 1 : 0;
        have += fmt::format("{}{}", have.empty() ? "" : "/", source);
    }
    detail = fmt::format("{} textures, {} blocked, {} from SOURCE, {} named {} ({})", probe.tex.size(), blocked,
        sources, seen, sSource.name, have);
    if (seen == 0) {
        return sSource.name + " not on the puppet";
    }
    if (want == "source" ? matching == 0 : matching != seen) {
        return fmt::format("{} renders from {}", sSource.name, have);
    }
    return {};
}

// --- looks.json

std::string fileMismatch(const json& step, std::string& detail) {
    looks::flushChoices();
    const std::string path = looks::choicesPath();
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    const json file = json::parse(text.str(), nullptr, false);
    detail = path;
    if (!file.is_object() || !looks::validInstallId(file.value("installId", std::string{})) ||
        file.value("installId", std::string{}) != looks::installId())
    {
        return "no install id saved";
    }
    const uint32_t id = clientIdByName(step.value("player", std::string{}));
    const auto client = Session::instance().clients().find(id);
    if (client == Session::instance().clients().end()) {
        return "no such peer";
    }
    const std::string key = step.value("byId", true) ? "id:" + client->second.installId : "name:" + client->second.name;
    if (step.contains("blocked")) {
        const size_t blocked = file.value("blocked", json::array()).size();
        if (blocked != step.value("blocked", size_t{0})) {
            return fmt::format("{} blocked files", blocked);
        }
    }
    const json players = file.value("players", json::object());
    const auto entry = players.find(key);
    if (step.value("none", false)) {
        return entry == players.end() ? std::string{} : "saved under " + key;
    }
    if (entry == players.end()) {
        return "nothing saved under " + key;
    }
    detail += " " + entry->dump();
    if (step.contains("look") && entry->value("look", std::string{}) != step.value("look", std::string{})) {
        return "look " + entry->value("look", std::string{});
    }
    if (step.contains("types") && entry->value("types", json::object()) != step["types"]) {
        return "types " + entry->value("types", json::object()).dump();
    }
    return {};
}

std::optional<bool> lookShareSteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;
    if (op == "looksSelfTest") {
        if (const std::string failure = selfTest(); !failure.empty()) {
            ctx.fail("looksSelfTest: " + failure);
        } else {
            TwiliLog.info("[autotest] look hashes, allowlist and manifest checks OK");
        }
        return true;
    }
    if (op == "lookOption") {
        return lookOption(ctx);
    }
    if (op == "looksArchiveOverride") {
        return archiveOverride(ctx);
    }
    if (op == "expectLocalLooks") {
        looks::refreshLocalLooks();
        return holdMatch(ctx, op, localMismatch(step), localText());
    }
    if (op == "expectPeerLooks") {
        const uint32_t id = clientIdByName(step.value("name", std::string{}));
        return holdMatch(ctx, op, id == 0 ? "no such peer" : peerMismatch(step, id), id == 0 ? "" : peerText(id));
    }
    if (op == "expectDummyArcPath") {
        std::vector<std::string> paths;
        std::string why;
        if (puppetPaths(step, paths, why)) {
            why = pathsMismatch(step, paths);
        }
        return holdMatch(ctx, op, why, pathsText(paths));
    }
    if (op == "looksSourceTexture") {
        if (step.contains("register")) {
            return registerSourceTexture(ctx);
        }
        std::string detail;
        const std::string why = sourceMismatch(step, detail);
        return holdMatch(ctx, op, why, detail);
    }
    if (op == "expectLooksFile") {
        std::string detail;
        const std::string why = fileMismatch(step, detail);
        return holdMatch(ctx, op, why, detail);
    }
    return std::nullopt;
}

const bool sRegistered = registerSteps(&lookShareSteps);

}  // namespace
}  // namespace twili::autotest
