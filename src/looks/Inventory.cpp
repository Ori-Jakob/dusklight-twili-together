#include "looks/Inventory.hpp"

#include "core/Log.hpp"

#include <mods/service.hpp>
#include <mods/svc/overlay.h>

#include <fmt/format.h>

#include <utility>

#if TWILI_HAS_MODS_SERVICE
// Without it nothing of the player's mods is visible: look sharing stays off.
IMPORT_OPTIONAL_SERVICE(ModsService, svc_mods);
#endif
// Look staging and tests; add_base_alias (minor 1) is checked with SERVICE_HAS.
IMPORT_OPTIONAL_SERVICE_VERSION(OverlayService, svc_overlay, 0);
// Look tests, and cosmetics on puppets.
IMPORT_OPTIONAL_SERVICE_VERSION(TextureService, svc_texture, 0);

namespace twili::looks {

const std::array<CosmeticsVar, 24> kCosmeticsVars = {{
    {"herosTunicCapColor", false},
    {"herosTunicTorsoColor", false},
    {"herosTunicSkirtColor", false},
    {"zoraArmorCapColor", false},
    {"zoraArmorHelmetColor", false},
    {"zoraArmorTorsoColor", false},
    {"zoraArmorScalesColor", false},
    {"zoraArmorFlippersColor", false},
    {"lanternGlowColor", false},
    {"woodenSwordColor", false},
    {"ordonSwordBladeColor", false},
    {"ordonSwordHandleColor", false},
    {"msBladeColor", false},
    {"msHandleColor", false},
    {"lightSwordGlowColor", false},
    {"boomerangColor", false},
    {"ironBootsColor", false},
    {"spinnerColor", false},
    {"midnaHairBaseColor", true},
    {"midnaHairTipsColor", true},
    {"midnaChargeRingColor", false},
    {"linkHairColor", false},
    {"wolfLinkColor", false},
    {"eponaColor", false},
}};

std::string hex(const Sha256& sha) {
    std::string out;
    out.reserve(sha.size() * 2);
    for (const uint8_t byte : sha) {
        out += fmt::format("{:02x}", byte);
    }
    return out;
}

#if TWILI_HAS_MODS_SERVICE

namespace {

std::string str(const char* text) {
    return text != nullptr ? text : "";
}

std::optional<std::vector<uint8_t>> takeBuffer(ModBuffer& buffer) {
    std::vector<uint8_t> bytes;
    if (buffer.data != nullptr) {
        const auto* data = static_cast<const uint8_t*>(buffer.data);
        bytes.assign(data, data + buffer.size);
    }
    svc_mods->free_buffer(mod_ctx, &buffer);
    return bytes;
}

std::string describe(const OverrideEntry& o) {
    switch (o.kind) {
    case OverrideKind::DiscFile:
        return o.aliasOf.empty() ? fmt::format("file {} {} B", o.discPath, o.size) :
                                   fmt::format("file {} {} B, alias of {}", o.discPath, o.size,
                                       o.aliasOf);
    case OverrideKind::ArcEntry:
        return fmt::format("entry {} in {} {} B", o.discPath, o.archivePath, o.size);
    case OverrideKind::Texture:
        break;
    }
    const TextureKey& key = o.textureKey;
    std::string what = key.kind == TEXTURE_KEY_POINTER ?
                           fmt::format("texture ptr {}", key.pointer) :
                           fmt::format("texture tex1_{}x{}_{:016x}{}_{}", key.width, key.height,
                               key.texture_hash,
                               key.has_tlut ? fmt::format("_{:016x}", key.tlut_hash) : "",
                               key.gx_format);
    if (o.textureRaw) {
        what += fmt::format(" raw {}x{} fmt {}", o.textureWidth, o.textureHeight, o.textureGxFormat);
    } else if (!o.bundlePath.empty()) {
        what += " " + o.bundlePath;
    }
    return what + fmt::format(" {} B", o.size);
}

}  // namespace

bool available() {
    return svc_mods != nullptr;
}

std::vector<ModEntry> listMods() {
    std::vector<ModEntry> mods;
    if (svc_mods == nullptr) {
        return mods;
    }
    svc_mods->list_mods(
        mod_ctx,
        [](void* user, const ModInfo* info) {
            static_cast<std::vector<ModEntry>*>(user)->push_back({
                .id = str(info->id),
                .name = str(info->name),
                .version = str(info->version),
                .author = str(info->author),
                .manifest = std::string(info->manifest_json != nullptr ? info->manifest_json : "",
                    info->manifest_json != nullptr ? info->manifest_len : 0),
                .loadOrder = info->load_order,
                .native = info->has_native,
                .script = info->has_script,
            });
            return true;
        },
        &mods);
    return mods;
}

std::vector<OverrideEntry> listOverrides(const std::string& modId, bool textures) {
    struct Listing {
        std::vector<OverrideEntry> overrides;
        bool textures;
    } listing{{}, textures};
    if (svc_mods == nullptr) {
        return {};
    }
    svc_mods->list_overrides(
        mod_ctx, modId.c_str(),
        [](void* user, const ModOverride* o) {
            auto* l = static_cast<Listing*>(user);
            if (!l->textures && o->kind == MOD_OVR_TEXTURE) {
                return true;
            }
            l->overrides.push_back({
                .id = o->id,
                .kind = o->kind == MOD_OVR_ARC_ENTRY ? OverrideKind::ArcEntry :
                        o->kind == MOD_OVR_TEXTURE   ? OverrideKind::Texture :
                                                       OverrideKind::DiscFile,
                .runtime = o->runtime,
                .wins = o->wins,
                .discPath = str(o->disc_path),
                .archivePath = str(o->archive_path),
                .aliasOf = str(o->alias_of),
                .bundlePath = str(o->bundle_path),
                .size = o->size,
                .textureKey = o->texture_key,
                .textureRaw = o->texture_raw,
                .textureWidth = o->texture_width,
                .textureHeight = o->texture_height,
                .textureGxFormat = o->texture_gx_format,
            });
            return true;
        },
        &listing);
    return std::move(listing.overrides);
}

std::optional<Sha256> hashOverride(const std::string& modId, uint64_t id) {
    Sha256 sha{};
    if (svc_mods == nullptr ||
        svc_mods->hash_override(mod_ctx, modId.c_str(), id, sha.data()) != MOD_OK)
    {
        return std::nullopt;
    }
    return sha;
}

std::optional<std::vector<uint8_t>> readOverride(const std::string& modId, uint64_t id) {
    ModBuffer buffer = MOD_BUFFER_INIT;
    if (svc_mods == nullptr ||
        svc_mods->read_override(mod_ctx, modId.c_str(), id, &buffer) != MOD_OK)
    {
        return std::nullopt;
    }
    return takeBuffer(buffer);
}

std::optional<std::vector<uint8_t>> readBaseDiscFile(const char* discPath) {
    ModBuffer buffer = MOD_BUFFER_INIT;
    if (svc_mods == nullptr ||
        svc_mods->read_base_disc_file(mod_ctx, discPath, &buffer) != MOD_OK)
    {
        return std::nullopt;
    }
    return takeBuffer(buffer);
}

void dump(bool hashes) {
    if (svc_mods == nullptr) {
        TwiliLog.info("[looks] tt.looks.dump: the host has no ModsService");
        return;
    }
    const auto mods = listMods();
    TwiliLog.info("[looks] tt.looks.dump: disc {} rev {}, generation {}, {} active mod(s)",
        str(svc_mods->game_id), svc_mods->disc_revision, svc_mods->generation(mod_ctx),
        mods.size());
    for (const auto& mod : mods) {
        TwiliLog.info("[looks]   mod {} '{}' {} by {} (order {}{}{}), mod.json {} B", mod.id,
            mod.name, mod.version, mod.author, mod.loadOrder, mod.native ? ", native" : "",
            mod.script ? ", script" : "", mod.manifest.size());
        for (const auto& o : listOverrides(mod.id)) {
            std::string sha;
            if (hashes && o.wins) {
                const auto hash = hashOverride(mod.id, o.id);
                sha = hash ? " sha256 " + hex(*hash) : " sha256 unavailable";
            }
            TwiliLog.info("[looks]     #{} {}{}{}{}", o.id, describe(o), o.runtime ? " runtime" : "",
                o.wins ? " wins" : "", sha);
        }
    }

    std::string values;
    for (const auto& var : kCosmeticsVars) {
        if (var.isInt) {
            int64_t value = 0;
            if (svc_mods->get_mod_var_int(mod_ctx, kCosmeticsModId, var.name, &value) == MOD_OK) {
                values += fmt::format(" {}={}", var.name, value);
            }
        } else {
            char value[64] = {};
            if (svc_mods->get_mod_var_string(
                    mod_ctx, kCosmeticsModId, var.name, value, sizeof(value), nullptr) == MOD_OK)
            {
                values += fmt::format(" {}='{}'", var.name, value);
            }
        }
    }
    TwiliLog.info("[looks]   cosmetics:{}", values.empty() ? " not active" : values);
}

#else

bool available() {
    return false;
}

std::vector<ModEntry> listMods() {
    return {};
}

std::vector<OverrideEntry> listOverrides(const std::string&, bool) {
    return {};
}

std::optional<Sha256> hashOverride(const std::string&, uint64_t) {
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> readOverride(const std::string&, uint64_t) {
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> readBaseDiscFile(const char*) {
    return std::nullopt;
}

void dump(bool) {
    TwiliLog.info("[looks] tt.looks.dump: built without ModsService");
}

#endif

}  // namespace twili::looks
