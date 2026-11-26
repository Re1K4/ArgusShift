#include "AutomationRunner.h"
#include "Control.h"
#include "Emulate.h"
#include "Event.h"
#include "Lang.h"
#include "Logger.h"
#include "MultiEvent.h"
#include "TemplateMatching.h"
#include "Timer.h"
#include "WindowInit.h"
#include <memory>
#include <vector>

using namespace Automation;

namespace {
    // Objects created from the configuration of a single step
    struct StepRuntime {
        std::unique_ptr<MultiEvent> multi;
        std::unique_ptr<TemplateMatching> templ;
    };

    std::string stepLabel(int index, const StepConfig& s) {
        return "Step " + std::to_string(index) + " (" + toAnsi(s.name) + ")";
    }
}

AutomationRunner::~AutomationRunner() {
    stop();
}

bool AutomationRunner::start(const Project& project, std::wstring& error) {
    if (running.load()) {
        error = Lang::tr(L"Automation is already running.", L"自動化はすでに実行中です。");
        return false;
    }
    error = validateProject(project);
    if (!error.empty()) return false;

    if (worker.joinable()) worker.join();  // previous run already finished
    stopRequested = false;
    running = true;
    current = 0;
    worker = std::thread(&AutomationRunner::threadMain, this, project);
    return true;
}

void AutomationRunner::stop() {
    stopRequested = true;
    if (worker.joinable()) worker.join();
}

bool AutomationRunner::sleepInterruptible(int ms) {
    const int slice = 10;
    while (ms > 0) {
        if (stopRequested.load()) return false;
        int d = ms < slice ? ms : slice;
        Emulate::sleep(d);
        ms -= d;
    }
    return !stopRequested.load();
}

void AutomationRunner::threadMain(Project p) {
    try {
        Logger::info("Automation start");

        const bool clientCoords = p.coordMode == CoordMode::Client;
        if (!p.targetExe.empty()) {
            // Client coordinates follow the window, so it only needs to be activated, not moved
            bool found = p.fixWindow
                ? TryWindowInitRect(p.targetExe.c_str(), p.winX, p.winY, p.winW, p.winH)
                : clientCoords ? TryActivateWindow(p.targetExe.c_str())
                : TryWindowInit(toAnsi(p.targetExe).c_str());
            if (!found) {
                Logger::error("Automation aborted: target window not found");
                current = -1;
                running = false;
                return;
            }
            if (p.fixWindow) sleepInterruptible(200);  // let the moved window repaint before checking pixels
        }

        // ---- Build runtime objects from configuration
        std::vector<std::unique_ptr<Event>> events;
        for (const auto& e : p.events) {
            events.push_back(std::make_unique<Event>(e.x, e.y, RGB(e.r, e.g, e.b), e.fuzzy, toAnsi(e.colorType)));
        }

        std::vector<Timer> timers(p.timers.size());

        const int stepCount = (int)p.steps.size();
        std::vector<StepRuntime> rt(stepCount);
        for (int i = 0; i < stepCount; ++i) {
            const StepConfig& s = p.steps[i];
            if ((s.condition == ConditionType::EventColor || s.condition == ConditionType::EventRGB) &&
                s.eventIds.size() >= 2) {
                std::vector<Event*> list;
                for (int id : s.eventIds) list.push_back(events[id].get());
                rt[i].multi = std::make_unique<MultiEvent>(list);
            }
            if (s.condition == ConditionType::Template || s.condition == ConditionType::TemplateBinary) {
                rt[i].templ = std::make_unique<TemplateMatching>(toAnsi(s.templatePath));
            }
        }

        // ---- Coordinate origin: (0, 0) for screen coordinates, otherwise the screen position of
        // the target window's client area, re-read every poll so a moved window is followed.
        POINT origin{ 0, 0 };
        bool originApplied = false;
        bool windowLost = false;
        HWND targetWindow = clientCoords ? FindTargetWindow(p.targetExe.c_str()) : nullptr;

        auto updateOrigin = [&]() -> bool {
            if (!clientCoords) return true;
            POINT o;
            if (!GetClientOrigin(targetWindow, o)) {
                targetWindow = FindTargetWindow(p.targetExe.c_str());
                if (!GetClientOrigin(targetWindow, o)) {
                    if (!windowLost) Logger::warning("Target window not found or minimized: waiting for it");
                    windowLost = true;
                    return false;
                }
            }
            if (windowLost) {
                Logger::info("Target window found again");
                windowLost = false;
            }
            if (!originApplied || o.x != origin.x || o.y != origin.y) {
                origin = o;
                originApplied = true;
                for (size_t i = 0; i < events.size(); ++i) {
                    events[i]->x = p.events[i].x + origin.x;
                    events[i]->y = p.events[i].y + origin.y;
                }
                for (auto& r : rt) if (r.multi) r.multi->calculateBoundingRect();
            }
            return true;
        };

        bool hasMatch = false;
        POINT matchCenter{ 0, 0 };

        auto evaluate = [&](int i) -> bool {
            const StepConfig& s = p.steps[i];
            switch (s.condition) {
            case ConditionType::None:
                return true;
            case ConditionType::EventColor:
                if (rt[i].multi) return rt[i].multi->checkColorMulti(s.matchAll ? ALL_MATCH : ANY_MATCH, s.debug);
                return events[s.eventIds[0]]->checkColor(s.debug);
            case ConditionType::EventRGB:
                if (rt[i].multi) return rt[i].multi->checkRGBMulti(s.matchAll ? ALL_MATCH : ANY_MATCH, s.debug);
                return events[s.eventIds[0]]->checkRGB(s.debug);
            case ConditionType::TimerElapsed:
                return timers[s.conditionTimer].distance() >= p.timers[s.conditionTimer].seconds;
            case ConditionType::Template:
            case ConditionType::TemplateBinary: {
                cv::Rect region(s.regionX + origin.x, s.regionY + origin.y, s.regionW, s.regionH);
                bool found = (s.condition == ConditionType::Template)
                    ? rt[i].templ->match(region, s.threshold)
                    : rt[i].templ->matchBinary(region, s.threshold);
                if (found) {
                    // Screen position: the click is made where the match was seen
                    const cv::Rect& r = rt[i].templ->lastMatchResult;
                    matchCenter.x = region.x + r.x + r.width / 2;
                    matchCenter.y = region.y + r.y + r.height / 2;
                    hasMatch = true;
                }
                return found;
            }
            default:
                return false;
            }
        };

        auto click = [&](bool left, int holdMs) {
            if (left) Emulate::pressMouseLeft(); else Emulate::pressMouseRight();
            Emulate::sleep(holdMs > 0 ? holdMs : 1);
            if (left) Emulate::releaseMouseLeft(); else Emulate::releaseMouseRight();
        };

        auto perform = [&](const StepConfig& s) {
            switch (s.action) {
            case ActionType::None:
                break;
            case ActionType::KeyTap: {
                std::vector<unsigned short> keys;
                parseKeyCombo(s.text, keys);
                for (auto k : keys) Emulate::pressKey(k);
                Emulate::sleep(s.ms > 0 ? s.ms : 1);
                for (auto it = keys.rbegin(); it != keys.rend(); ++it) Emulate::releaseKey(*it);
                break;
            }
            case ActionType::TypeText:
                for (wchar_t ch : s.text) {
                    if (stopRequested.load()) break;
                    Emulate::typeChar(ch);
                    if (s.ms > 0) Emulate::sleep(s.ms);
                }
                break;
            case ActionType::MouseMove:
                Emulate::moveMouseAbs(s.x + origin.x, s.y + origin.y);
                break;
            case ActionType::MouseMoveRel:
                Emulate::moveMouseRel(s.x, s.y);
                break;
            case ActionType::LeftClick:
            case ActionType::RightClick:
                if (s.x >= 0 && s.y >= 0) {
                    Emulate::moveMouseAbs(s.x + origin.x, s.y + origin.y);
                    Emulate::sleep(20);
                }
                click(s.action == ActionType::LeftClick, s.ms);
                break;
            case ActionType::ClickMatch:
                if (!hasMatch) {
                    Logger::warning("Click match: no template match result yet");
                    break;
                }
                Emulate::moveMouseAbs(matchCenter.x, matchCenter.y);
                Emulate::sleep(20);
                click(true, s.ms);
                break;
            case ActionType::Wait:
                sleepInterruptible(s.ms);
                break;
            case ActionType::TimerStart:
                timers[s.actionTimer].reset();
                timers[s.actionTimer].set();
                break;
            case ActionType::TimerReset:
                timers[s.actionTimer].reset();
                break;
            default:
                break;
            }
        };

        // Resolve a transition target to a step index (NEXT_END when finished)
        auto resolve = [&](int next, int index) -> int {
            if (next == NEXT_STAY) return index;
            if (next == NEXT_NEXT) return (index + 1 < stepCount) ? index + 1 : NEXT_END;
            return next;
        };

        // ---- Main loop: Control::NOW selects the running step
        Control control;
        Timer stepTimer;
        stepTimer.set();
        Logger::info("Enter " + stepLabel(0, p.steps[0]));

        while (!stopRequested.load()) {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                Logger::info("ESC key pressed, stop automation");
                break;
            }

            const int index = control.NOW;
            current = index;
            const StepConfig& s = p.steps[index];

            // Without the window there is nothing to check: wait for it to come back
            if (!updateOrigin()) {
                if (!sleepInterruptible(p.pollMs)) break;
                continue;
            }

            bool ok = evaluate(index);
            if (s.negate) ok = !ok;

            bool transition = false;
            int next = NEXT_STAY;
            if (ok) {
                perform(s);
                next = s.nextOnTrue;
                transition = true;
            }
            else if (s.timeoutSec > 0 && stepTimer.distance() >= s.timeoutSec) {
                Logger::warning(stepLabel(index, s) + " timed out");
                next = s.nextOnTimeout;
                transition = true;
            }
            else if (s.nextOnFalse != NEXT_STAY) {
                next = s.nextOnFalse;
                transition = true;
            }

            if (transition) {
                int target = resolve(next, index);
                if (target == NEXT_END) {
                    Logger::info("Reached end of automation");
                    break;
                }
                if (target != index) {
                    control.changeCtrl(target);
                    Logger::info("Enter " + stepLabel(target, p.steps[target]));
                }
                stepTimer.reset();
                stepTimer.set();
            }

            if (!sleepInterruptible(p.pollMs)) break;
        }

        Logger::info("Automation stopped");
    }
    catch (const std::exception& ex) {
        Logger::error(std::string("Automation error: ") + ex.what());
    }

    current = -1;
    running = false;
}
