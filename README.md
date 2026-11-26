# Name 
ArgusShift

## Overview
C++ template for GUI automation with OpenCV library. 
It is very fast and can automate most operations.

## Usage

### GUI (automation editor)
Run `ArgusShift.exe` to open the editor. Automations are configured without writing code:

| Tab | Content | Limit |
|---|---|---|
| Points (認識点) | Detection points = pixel checks `(x, y, color, fuzzy, colorType)` (the `Event` class). "Pick from screen" reads a position and color (see below); "Test now" logs the current result. | 32 |
| Timers | Named timers with a duration in seconds. | 16 |
| Steps | The automation itself. Each step = condition + action + transitions. | 64 |

#### Picking from the screen
"Pick from screen" (points), "Pick" (action X, Y) and "Drag..." (template region) start a pick.
While it is pending, these keys work globally:

| Key | Effect |
|---|---|
| **F1** | Freezes the whole screen at that moment and opens a full-screen picker on the capture. The capture is drawn 1:1 at its real screen position, so the picked coordinates are the real ones. Because the colors are those of the moment F1 was pressed, animated screens work too. |
| **Esc** | Cancels (clicking the pick button again also cancels). |

In the frozen picker: a magnifier shows the pixels around the cursor with the selected pixel outlined,
its coordinates and RGB; **arrow keys** move 1 px (**Shift**: 10 px); **Enter** / left click confirms;
**Esc** / right click cancels. For a template region, drag a rectangle (or press **Enter** on two corners);
X / Y / W / H are filled in.

Steps are executed as a state machine driven by a `Control` object (`Control::NOW` is the index of the running step):

1. The step's **condition** is checked every *Poll ms*:
   `Always`, `Point color` / `Point RGB type` (1 point -> `Event`, 2+ points -> `MultiEvent` with ALL/ANY),
   `Timer elapsed`, `Template match` / `Template (binary)`. `Negate` inverts the result.
2. When it is true, the **action** runs (`Key tap` e.g. `CTRL+S`, `Type text`, `Mouse move`, `Left/Right click`,
   `Click match` (center of the last template match), `Wait`, `Timer start`, `Timer reset`)
   and `Control::changeCtrl()` moves to **Next if true**.
3. When it is false, **Next if false** is used (`(stay / retry)` keeps polling the same step).
   With **Timeout (s)** > 0 the step moves to the timeout target when the condition stays false for that long.

Transition targets: `(next step)`, `(end)`, `(stay / retry)` or a specific step.
Add / Remove / Up / Down keep references between steps, points and timers consistent.
Projects are saved as UTF-8 text files (`*.argus`; older `*.cppauto` files can still be opened). `ArgusShift.exe <file.argus>` opens a project directly.
Press **ESC** (or the Stop button) to stop a running automation.

The UI language can be switched between English and Japanese (日本語) with the language selector
at the right end of the tab row. The choice is saved in `ArgusShift.ini` next to the exe; without it,
the Windows display language is used.

Coordinates are physical pixels. The **Coords** selector chooses how every coordinate (points,
template regions, mouse positions) is interpreted:

| Mode | Meaning |
|---|---|
| **Client** (default) | Offset from the top-left of the *Target exe* window's client area. The origin is re-read every poll, so the automation keeps working when the window is moved. Picks from the screen are stored relative to the window. If the window disappears or is minimized, the run waits for it. |
| **Screen** | Absolute screen pixels. Before the run the window is brought to the front and moved to (0, 0) (unless *Fix window* is set). |

When the mode is switched, the existing coordinates can be converted using the window's current position.
Project files saved before this setting existed open in **Screen** mode.

### Writing code (template)
The classes below can still be used directly from C++. The original console sample lives in `Sample.cpp`
and runs with `ArgusShift.exe --sample`.
(!) Please launch notepad.exe before running the sample.

### Definition and method calls

#### Logger
```cpp
Logger::initialize("", true);
Logger::info("message");
Logger::warning("message");
Logger::error("message");
```
- `initialize(filename, consoleOutput)` initializes log output destination and console output settings.
- `info / warning / error` writes logs at each level.

#### Timer
```cpp
Timer tm;
tm.set();
double elapsed = tm.distance();
tm.reset();
```
- Create an instance and call `set()` to start measurement.
- `distance()` returns elapsed time.
- `reset()` resets the internal state.

#### Control
```cpp
Control control;
int current = control.NOW;
int previous = control.PREV;
control.changeCtrl(10);
```
- `NOW` and `PREV` store current/previous control numbers.
- Use `changeCtrl()` to update control state.

#### Event
```cpp
Event ev(28, 19, RGB(106, 167, 182), 0, "BLUE");
bool colorMatched = ev.checkColor();
bool rgbMatched = ev.checkRGB();
```
- Define with `(x, y, color, fazzy, colorType)`.
- `checkColor()` checks exact-like color matching with `fazzy` tolerance.
- `checkRGB()` checks by RGB category (`"RED"`, `"GREEN"`, `"BLUE"`).

#### MultiEvent
```cpp
Event ev1(28, 19, RGB(106, 167, 182), 0, "BLUE");
Event ev2(29, 13, RGB(104, 166, 181), 0, "BLUE");
Event ev3(28, 20, RGB(118, 184, 199), 0, "BLUE");

MultiEvent mev(ev1, ev2, ev3);
bool allColor = mev.checkColorMulti(ALL_MATCH, true);
bool anyRgb = mev.checkRGBMulti(ANY_MATCH, false);
```
- Pass two or more `Event` objects to the constructor.
- `checkColorMulti(mode, debugFlag)` and `checkRGBMulti(mode, debugFlag)` support:
  - `ALL_MATCH`: all events must match
  - `ANY_MATCH`: at least one event must match

#### TemplateMatching
```cpp
TemplateMatching tm("asset/images/notepad_icon.png");
bool found = tm.match(cv::Rect(0, 0, 1920, 1080), 0.9);
bool foundBinary = tm.matchBinary(cv::Rect(0, 0, 1920, 1080), 0.9);
```
- Create with template image path.
- `match(region, fuzzy)` performs normal template matching.
- `matchBinary(region, fuzzy)` performs binary-image-based matching.
- Last result rectangle is stored in `lastMatchResult`.

### How to call `WindowInit`
```cpp
WindowInit("notepad.exe");
```
- Pass the target application executable name as `const char*`.
- Run this before event checks or mouse/keyboard automation so shared window information is initialized.
