#pragma once
#include <string>
#include <vector>

// Data model for GUI-configured automation.
// A Project holds fixed-capacity pools of Events, Timers and Steps.
// Steps reference Events/Timers by index and are executed as a state machine
// driven by a Control object (Control::NOW == index of the running step).
namespace Automation {

    // Upper limits of each pool (steps can be added/removed up to these limits)
    constexpr int MAX_EVENTS = 32;
    constexpr int MAX_TIMERS = 16;
    constexpr int MAX_STEPS = 64;

    // Special transition targets
    constexpr int NEXT_END = -1;   // finish the automation
    constexpr int NEXT_STAY = -2;  // stay on the same step (poll again)
    constexpr int NEXT_NEXT = -3;  // go to the following step (end if it is the last one)

    enum class ConditionType : int {
        None = 0,        // always true
        EventColor,      // Event::checkColor (2+ events -> MultiEvent::checkColorMulti)
        EventRGB,        // Event::checkRGB   (2+ events -> MultiEvent::checkRGBMulti)
        TimerElapsed,    // Timer::distance() >= timer seconds
        Template,        // TemplateMatching::match
        TemplateBinary,  // TemplateMatching::matchBinary
        Count
    };

    enum class ActionType : int {
        None = 0,
        KeyTap,        // text = key combo (e.g. "CTRL+S", "ENTER", "A"), ms = hold time
        TypeText,      // text = string to type, ms = interval between characters
        MouseMove,     // absolute screen position (x, y)
        MouseMoveRel,  // relative move (x, y)
        LeftClick,     // move to (x, y) and click (x < 0 or y < 0: click at current position)
        RightClick,
        ClickMatch,    // left click the center of the last template match
        Wait,          // wait ms
        TimerStart,    // reset + start timer
        TimerReset,    // stop timer
        Count
    };

    // How every configured coordinate (points, template regions, mouse positions) is interpreted
    enum class CoordMode : int {
        Screen = 0,  // absolute screen pixels
        Client = 1,  // offset from the top-left of the target window's client area
    };

    const wchar_t* conditionName(ConditionType type);
    const wchar_t* actionName(ActionType type);

    constexpr int COLOR_TYPE_COUNT = 6;
    extern const wchar_t* const COLOR_TYPES[COLOR_TYPE_COUNT];

    struct EventConfig {
        std::wstring name = L"Event";
        int x = 0, y = 0;
        int r = 0, g = 0, b = 0;
        int fuzzy = 0;
        std::wstring colorType = L"RED";
    };

    // True if the point's RGB has no hue (gray / white / black): 'Point color family' never matches it
    bool isAchromatic(const EventConfig& e);

    struct TimerConfig {
        std::wstring name = L"Timer";
        double seconds = 1.0;
    };

    struct StepConfig {
        std::wstring name = L"Step";

        // Condition
        ConditionType condition = ConditionType::None;
        std::vector<int> eventIds;      // Event indices (1 -> Event, 2+ -> MultiEvent)
        bool matchAll = true;           // MultiEvent: ALL_MATCH / ANY_MATCH
        int conditionTimer = 0;         // Timer index for TimerElapsed
        std::wstring templatePath;      // relative to exe dir, or absolute
        int regionX = 0, regionY = 0, regionW = 1920, regionH = 1080;
        double threshold = 0.9;
        bool negate = false;            // invert the condition result
        bool debug = false;             // pass debugFlag to Event/MultiEvent checks

        // Action (executed when the condition is true)
        ActionType action = ActionType::None;
        std::wstring text;
        int x = 0, y = 0;
        int ms = 50;
        int actionTimer = 0;

        // Transitions
        int nextOnTrue = NEXT_NEXT;
        int nextOnFalse = NEXT_STAY;
        double timeoutSec = 0.0;        // 0 = no timeout
        int nextOnTimeout = NEXT_END;
    };

    struct Project {
        std::wstring targetExe;   // passed to WindowInit (empty = skip)
        int pollMs = 50;          // loop interval

        // Client: coordinates follow the target window wherever it is (requires targetExe).
        // Files saved before this setting existed load as Screen.
        CoordMode coordMode = CoordMode::Client;

        // Target window placement. When fixWindow is set the target window is moved/resized
        // to (winX, winY, winW, winH) before running, so configured coordinates match the screen.
        bool fixWindow = false;
        int winX = 0, winY = 0, winW = 1280, winH = 720;
        std::vector<EventConfig> events;
        std::vector<TimerConfig> timers;
        std::vector<StepConfig> steps;

        // Return the new index, or -1 when the limit is reached
        int addEvent();
        int addTimer();
        int addStep();

        // Remove/reorder while keeping the references in steps consistent
        void removeEvent(int index);
        void removeTimer(int index);
        void removeStep(int index);
        void swapEvents(int a, int b);
        void swapTimers(int a, int b);
        void swapSteps(int a, int b);

        // Add (dx, dy) to every stored position: points, template regions and absolute mouse
        // positions (relative moves and "click at current position" are left alone).
        void offsetCoordinates(int dx, int dy);
    };

    bool saveProject(const Project& project, const std::wstring& path, std::wstring& error);
    bool loadProject(Project& project, const std::wstring& path, std::wstring& error);

    // Returns an empty string when the project can be executed
    std::wstring validateProject(const Project& project);

    // Parse "CTRL+SHIFT+S" style key combos into virtual key codes (modifiers first)
    bool parseKeyCombo(const std::wstring& text, std::vector<unsigned short>& keys);

    // String conversion helpers
    std::string toUtf8(const std::wstring& s);
    std::wstring fromUtf8(const std::string& s);
    std::string toAnsi(const std::wstring& s);
    std::wstring fromAnsi(const std::string& s);
}
