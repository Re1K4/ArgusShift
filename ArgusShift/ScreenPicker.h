#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

// Screen picking tools used by the editor GUI.
//
// All returned coordinates are screen coordinates in the same pixel space as GetPixel(GetDC(NULL), x, y),
// SetCursorPos and BitBlt from the screen DC, i.e. the space Event / TemplateMatching / Emulate use.
namespace ScreenPicker {

    // How coordinates are shown while picking. With `clientOrigin` set, the loupe and the region
    // label show (screen - origin), i.e. client coordinates of the target window, and the banner
    // says so. Only the display changes: the results are still screen coordinates.
    struct Display {
        const POINT* clientOrigin = nullptr;
    };

    // Freeze the whole (virtual) screen right now and let the user choose one pixel on the capture
    // with a magnifier. Arrow keys move 1 px (Shift: 10 px), Enter / left click confirms,
    // Esc / right click cancels. `color` is read from the frozen capture.
    bool pickPoint(HWND owner, POINT& pt, COLORREF& color, Display display = {});

    // Freeze the whole (virtual) screen right now and let the user drag a rectangle on the capture.
    // `rc` is { left, top, right (exclusive), bottom (exclusive) }.
    bool pickRect(HWND owner, RECT& rc, Display display = {});

    // Same as pickRect, and also returns the selected pixels as they were on the frozen capture:
    // `pixels` is top-down 32bpp BGRX, (rc.right - rc.left) * (rc.bottom - rc.top) values.
    bool pickImage(HWND owner, RECT& rc, std::vector<uint32_t>& pixels, Display display = {});
}
