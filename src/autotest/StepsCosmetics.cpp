// Steps for teammates' cosmetics on puppets; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "actors/DummyHorse.hpp"
#include "actors/DummyPlayer.hpp"
#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "fx/CosmeticsRecolor.hpp"
#include "looks/Cosmetics.hpp"
#include "looks/Inventory.hpp"
#include "ui/ColorMath.hpp"

#include <fmt/format.h>

#include <optional>
#include <string>

namespace twili::autotest {
namespace {

using nlohmann::json;

uint32_t clientIdByName(const std::string& name) {
    for (const auto& [id, c] : Session::instance().clients()) {
        if (!c.self && c.name == name) {
            return id;
        }
    }
    return 0;
}

int varIndex(const std::string& name) {
    for (size_t i = 0; i < looks::kCosmeticsVars.size(); i++) {
        if (name == looks::kCosmeticsVars[i].name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string valuesText(const looks::CosmeticsValues* v) {
    if (v == nullptr || v->mask == 0) {
        return "none";
    }
    std::string s = fmt::format("cosmetics {}:", v->version);
    for (size_t i = 0; i < looks::kCosmeticsVars.size(); i++) {
        if (v->has(static_cast<uint8_t>(i))) {
            s += looks::kCosmeticsVars[i].isInt ?
                     fmt::format(" {}={}", looks::kCosmeticsVars[i].name, v->value[i]) :
                     fmt::format(" {}={:06x}", looks::kCosmeticsVars[i].name, v->value[i]);
        }
    }
    return s;
}

// What of `want` ({vars, version, none}) the values miss, empty when none.
std::string valuesMismatch(const json& step, const looks::CosmeticsValues* v) {
    if (step.value("none", false)) {
        return v == nullptr || v->mask == 0 ? std::string{} : "shares " + valuesText(v);
    }
    if (v == nullptr || v->mask == 0) {
        return "shares none";
    }
    uint32_t mask = 0;
    for (const auto& [name, value] : step.value("vars", json::object()).items()) {
        const int i = varIndex(name);
        if (i < 0) {
            return "unknown var " + name;
        }
        mask |= 1u << i;
        const uint32_t want = value.is_string() ? static_cast<uint32_t>(std::stoul(
                                                      value.get<std::string>(), nullptr, 16)) :
                                                  value.get<uint32_t>();
        if (!v->has(static_cast<uint8_t>(i)) || v->value[i] != want) {
            return fmt::format("{} is not {}", name, value.dump());
        }
    }
    if (mask != v->mask) {
        return "other vars set: " + valuesText(v);
    }
    if (step.contains("version") && v->version != step.value("version", std::string{})) {
        return "version " + v->version;
    }
    return {};
}

const char* stateName(CosmeticsTexState s) {
    return s == CosmeticsTexState::Recolor ? "recolor" : s == CosmeticsTexState::Block ? "block" : "none";
}

const char* sourceName(int s) {
    return s == 1 ? "pointer" : s == 2 ? "source" : s == 0 ? "none" : "unknown";
}

std::string probeText(const CosmeticsProbe& p) {
    std::string s = fmt::format("keys={} recolored={} blocked={} hair={}/{}", p.keys, p.recolored,
        p.blocked, p.hairBase, p.hairTips);
    for (const CosmeticsTexProbe& t : p.tex) {
        s += fmt::format(" [{} {} {}{} src={}{}]", t.texture, looks::kCosmeticsVars[t.var].name,
            stateName(t.state),
            t.state == CosmeticsTexState::Recolor ? fmt::format(" #{:06X} h{:.0f} s{:.2f}", t.color,
                                                       t.mean.h, t.mean.s) :
                                                    std::string{},
            sourceName(t.source), t.vanilla == 1 ? "" : t.vanilla == 0 ? " custom" : t.vanilla == 2 ? " unchecked" : " pending");
    }
    return s;
}

// One texture against {state, rgb, hueTol, source, vanilla}.
std::string texMismatch(const json& spec, const CosmeticsTexProbe& t) {
    const std::string state = spec.value("state", std::string{});
    if (!state.empty() && state != stateName(t.state)) {
        return fmt::format("{} is {}", t.texture, stateName(t.state));
    }
    if (spec.contains("rgb")) {
        const auto rgb = ui::color::parseHex(spec.value("rgb", std::string{}));
        if (!rgb) {
            return "bad rgb";
        }
        const uint32_t want = static_cast<uint32_t>(rgb->r) << 16 | rgb->g << 8 | rgb->b;
        if (t.state != CosmeticsTexState::Recolor || t.color != want) {
            return fmt::format("{} holds #{:06X}", t.texture, t.color);
        }
        const Hsv hsv = toHsv({rgb->r / 255.0f, rgb->g / 255.0f, rgb->b / 255.0f});
        if (hsv.s >= 0.2f && hueDist(t.mean.h, hsv.h) > spec.value("hueTol", 15.0)) {
            return fmt::format("{} mean hue {:.0f}, want {:.0f}", t.texture, t.mean.h, hsv.h);
        }
    }
    if (spec.contains("source") && spec.value("source", std::string{}) != sourceName(t.source)) {
        return fmt::format("{} renders from {}", t.texture, sourceName(t.source));
    }
    if (spec.contains("vanilla") && spec.value("vanilla", true) != (t.vanilla == 1)) {
        return fmt::format("{} vanilla={}", t.texture, t.vanilla);
    }
    return {};
}

std::string probeMismatch(const json& step, const CosmeticsProbe& p) {
    if (!p.bound) {
        return "no cosmetics textures bound";
    }
    if (step.contains("keys")) {
        const std::string want = step.value("keys", std::string{});
        const char* have = p.keys == 1 ? "disc" : p.keys == 2 ? "unverified" : "pending";
        if (want != have) {
            return fmt::format("keys {}", have);
        }
    }
    for (const auto& [name, spec] : step.value("vars", json::object()).items()) {
        const int var = varIndex(name);
        size_t seen = 0;
        for (const CosmeticsTexProbe& t : p.tex) {
            if (t.var != var) {
                continue;
            }
            seen++;
            if (std::string why = texMismatch(spec, t); !why.empty()) {
                return name + ": " + why;
            }
        }
        if (seen == 0) {
            return name + ": no texture of it on this puppet";
        }
    }
    for (const auto& [name, spec] : step.value("textures", json::object()).items()) {
        size_t seen = 0;
        for (const CosmeticsTexProbe& t : p.tex) {
            if (name != t.texture) {
                continue;
            }
            seen++;
            if (std::string why = texMismatch(spec, t); !why.empty()) {
                return why;
            }
        }
        if (seen == 0) {
            return name + ": not on this puppet";
        }
    }
    if (step.contains("noSource") && step.value("noSource", true)) {
        for (const CosmeticsTexProbe& t : p.tex) {
            if (t.source == 2) {
                return fmt::format("{} renders from a content (SOURCE) replacement", t.texture);
            }
        }
    }
    if (step.contains("hair")) {
        const json& hair = step["hair"];
        if (p.hairBase != hair.at(0).get<int>() || p.hairTips != hair.at(1).get<int>()) {
            return fmt::format("hair palette {}/{}", p.hairBase, p.hairTips);
        }
    }
    return {};
}

bool probePuppet(const json& step, CosmeticsProbe& out, std::string& why) {
    const uint32_t id = clientIdByName(step.value("name", std::string{}));
    if (id == 0) {
        why = "no such peer";
        return false;
    }
    if (step.value("horse", false)) {
        daDummyHorse_c* horse = FindDummyHorse(id);
        if (horse == nullptr) {
            why = "no horse puppet";
            return false;
        }
        horse->getCosmeticsProbe(out);
        return true;
    }
    fopAc_ac_c* dummy = Session::instance().dummyActorForClient(id);
    if (dummy == nullptr || !GetDummyPlayerCosmeticsProbe(dummy, out)) {
        why = "no dummy";
        return false;
    }
    return true;
}

int sMatchedAt = -1;

// Holds `frames` ticks once it matches; fails on a break after that or at the timeout.
bool holdMatch(StepContext& ctx, const std::string& op, const std::string& why,
    const std::string& detail) {
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
        ctx.fail(fmt::format("{}: {} ticks after it matched, {} ({})", op, ctx.ticks - sMatchedAt,
            why, detail));
        return false;
    }
    if (ctx.seconds > ctx.timeout(30.0)) {
        ctx.fail(fmt::format("{}: {} ({})", op, why, detail));
    }
    return false;
}

std::optional<bool> cosmeticsSteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;

    if (op == "cosmeticsSelfTest") {
        if (const std::string failure = cosmeticsSelfTest(); !failure.empty()) {
            ctx.fail("cosmeticsSelfTest: " + failure);
            return false;
        }
        TwiliLog.info("[autotest] cosmetics recolour, palette and readers OK");
        return true;
    }

    if (op == "cosmeticsOption") {
        if (step.contains("share")) {
            config::setBool(config::Var::ShareCosmetics, step.value("share", true));
        }
        if (step.contains("show")) {
            config::setBool(config::Var::ShowCosmetics, step.value("show", true));
        }
        if (step.contains("player")) {
            const uint32_t id = clientIdByName(step.value("player", std::string{}));
            if (id == 0) {
                ctx.fail("cosmeticsOption: no such peer");
                return false;
            }
            looks::setCosmeticsHiddenFor(id, step.value("hidden", true));
        }
        return true;
    }

    if (op == "expectCosmetics") {
        std::string why, detail;
        if (step.value("self", false)) {
            const auto& sent = looks::sentCosmetics();
            const looks::CosmeticsValues* v = sent ? &*sent : nullptr;
            why = looks::cosmeticsSent() ? valuesMismatch(step, v) : "nothing sent yet";
            detail = "sent " + valuesText(v) + ", read " +
                     (looks::localCosmetics() ? valuesText(&*looks::localCosmetics()) : "inactive");
        } else {
            const uint32_t id = clientIdByName(step.value("name", std::string{}));
            const looks::CosmeticsValues* v = id != 0 ? looks::sharedCosmetics(id) : nullptr;
            why = id == 0 ? "no such peer" : valuesMismatch(step, v);
            detail = valuesText(v);
        }
        return holdMatch(ctx, op, why, detail);
    }

    if (op == "expectDummyCosmetics") {
        CosmeticsProbe probe;
        std::string why;
        if (probePuppet(step, probe, why)) {
            why = probeMismatch(step, probe);
        }
        return holdMatch(ctx, op, why, probe.bound ? probeText(probe) : "no probe");
    }

    if (op == "dumpCosmetics") {
        for (const auto& [id, c] : Session::instance().clients()) {
            if (c.self) {
                continue;
            }
            CosmeticsProbe probe;
            fopAc_ac_c* dummy = Session::instance().dummyActorForClient(id);
            TwiliLog.info("[autotest] cosmetics of {}: {}; dummy {}", c.name,
                valuesText(looks::sharedCosmetics(id)),
                dummy != nullptr && GetDummyPlayerCosmeticsProbe(dummy, probe) ? probeText(probe) :
                                                                               std::string("none"));
        }
        return true;
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&cosmeticsSteps);

}  // namespace
}  // namespace twili::autotest
