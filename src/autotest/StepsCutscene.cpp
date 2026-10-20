// Steps for the generated cutscene scenarios (scenarios/cutscenes.js); reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "core/Log.hpp"
#include "story/StoryState.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_event.h"
#include "d/d_event_manager.h"

#include <fmt/format.h>

#include <string>

namespace twili::autotest {
namespace {

namespace sd = story::detail;

// The running event as the story tracker named it (map event name, else event data name).
std::string runningEventName() {
    if (!dComIfGp_event_runCheck()) {
        return {};
    }
    const story::Instance* open = story::tracker().openInstance();
    if (open != nullptr && open->running()) {
        return open->name;
    }
    const s16 id = dComIfGp_getEvent()->mEventId;
    dEvDtEvent_c* data = id != -1 ? dComIfGp_getEventManager().getEventData(id) : nullptr;
    return data != nullptr ? std::string(data->getName()) : std::string{};
}

// soft: a failed check is logged as a CUTSCENE line and the script goes on (batch scenarios).
bool finish(StepContext& ctx, bool pass, const std::string& detail) {
    const std::string tag = ctx.step.value("tag", std::string{});
    if (pass) {
        TwiliLog.info("[autotest] CUTSCENE PASS {} {}", tag, detail);
        return true;
    }
    if (ctx.step.value("soft", false)) {
        TwiliLog.warn("[autotest] CUTSCENE FAIL {} {}", tag, detail);
        return true;
    }
    ctx.fail(detail);
    return false;
}

std::optional<bool> cutsceneSteps(const std::string& op, StepContext& ctx) {
    const nlohmann::json& step = ctx.step;

    // Our own game runs the event of that name.
    if (op == "expectEventName") {
        const std::string want = step.value("name", std::string{});
        const std::string have = runningEventName();
        if (!want.empty() && have == want) {
            return finish(ctx, true, fmt::format("running '{}'", have));
        }
        if (ctx.seconds > ctx.timeout(30.0)) {
            return finish(ctx, false, fmt::format("expectEventName: want '{}', running '{}'", want, have));
        }
        return false;
    }

    // Our pull-in copy of a teammate's event of that name runs or ran.
    if (op == "expectCutsceneJoin") {
        const std::string want = step.value("name", std::string{});
        const sd::JoinInfo& j = sd::state().join;
        const bool ours = j.eventName == want;
        if (ours && (j.state == story::JoinState::Running || j.state == story::JoinState::Ended ||
                        j.state == story::JoinState::Shared))
        {
            return finish(ctx, true, fmt::format("joined '{}' ({})", want, story::joinStateName(j.state)));
        }
        if (ours && j.state == story::JoinState::Missed) {
            return finish(ctx, false, fmt::format("expectCutsceneJoin: missed '{}' ({})", want, j.reason));
        }
        if (ctx.seconds > ctx.timeout(60.0)) {
            return finish(ctx, false,
                fmt::format("expectCutsceneJoin: no join of '{}' (join {} '{}' {}, running '{}')", want,
                    story::joinStateName(j.state), j.eventName, j.reason, runningEventName()));
        }
        return false;
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&cutsceneSteps);

}  // namespace
}  // namespace twili::autotest
