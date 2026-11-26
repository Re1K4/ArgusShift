#include "ScreenPicker.h"
#include "Lang.h"

#include <windowsx.h>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace ScreenPicker {
    namespace {

        constexpr int LOUPE_RADIUS = 7;                       // 15 x 15 source pixels
        constexpr int LOUPE_CELLS = LOUPE_RADIUS * 2 + 1;

        int systemDpi() {
            HDC dc = GetDC(nullptr);
            int dpi = GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(nullptr, dc);
            return dpi;
        }

        int S(int v) {
            static const int dpi = systemDpi();
            return MulDiv(v, dpi, 96);
        }

        HFONT uiFont() {
            static const HFONT font = [] {
                NONCLIENTMETRICSW ncm{ sizeof(ncm) };
                SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
                return CreateFontIndirectW(&ncm.lfMessageFont);
            }();
            return font;
        }

        RECT virtualScreen() {
            int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
            return { x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
        }

        RECT monitorRect(POINT pt) {
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
            return mi.rcMonitor;
        }

        void fillRect(HDC dc, const RECT& r, COLORREF color) {
            SetDCBrushColor(dc, color);
            FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
        }

        void frameRect(HDC dc, RECT r, COLORREF color, int inflate = 0) {
            InflateRect(&r, inflate, inflate);
            SetDCBrushColor(dc, color);
            FrameRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
        }

        // ------------------------------------------------------------ 32bpp copy of a screen area

        // Pixel (sx, sy) of the screen is stored at bits[(sy - y0) * w + (sx - x0)], copied 1:1 with
        // BitBlt from the screen DC (the same source GetPixel(GetDC(NULL), x, y) reads).
        struct Capture {
            int x0 = 0, y0 = 0, w = 0, h = 0;
            HDC dc = nullptr;
            HBITMAP bmp = nullptr;
            HGDIOBJ old = nullptr;
            const uint32_t* bits = nullptr;

            Capture() = default;
            Capture(const Capture&) = delete;
            Capture& operator=(const Capture&) = delete;
            ~Capture() { release(); }

            void release() {
                if (dc) {
                    SelectObject(dc, old);
                    DeleteDC(dc);
                }
                if (bmp) DeleteObject(bmp);
                dc = nullptr;
                bmp = nullptr;
                bits = nullptr;
                w = h = 0;
            }

            bool allocate(int width, int height) {
                if (dc && width == w && height == h) return true;
                release();
                if (width <= 0 || height <= 0) return false;
                HDC screen = GetDC(nullptr);
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = width;
                bi.bmiHeader.biHeight = -height;   // top-down
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                void* p = nullptr;
                bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
                dc = CreateCompatibleDC(screen);
                ReleaseDC(nullptr, screen);
                if (!bmp || !dc || !p) {
                    release();
                    return false;
                }
                old = SelectObject(dc, bmp);
                bits = static_cast<const uint32_t*>(p);
                w = width;
                h = height;
                return true;
            }

            bool grab(int x, int y, int width, int height) {
                if (!allocate(width, height)) return false;
                x0 = x;
                y0 = y;
                HDC screen = GetDC(nullptr);
                BOOL ok = BitBlt(dc, 0, 0, width, height, screen, x, y, SRCCOPY);
                ReleaseDC(nullptr, screen);
                GdiFlush();
                return ok != FALSE;
            }

            bool contains(int sx, int sy) const {
                return bits && sx >= x0 && sy >= y0 && sx < x0 + w && sy < y0 + h;
            }

            COLORREF at(int sx, int sy) const {
                uint32_t p = bits[(size_t)(sy - y0) * w + (sx - x0)];
                return RGB((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
            }
        };

        // ------------------------------------------------------------ magnifier drawing

        int loupeCell() { return S(10); }
        int loupeInfoHeight() { return S(58); }

        SIZE loupeSize() {
            int zoom = loupeCell() * LOUPE_CELLS;
            return { zoom + 2, zoom + 2 + loupeInfoHeight() };
        }

        // Top-left of the loupe for a cursor at `anchor`: bottom-right of the cursor,
        // flipped to the other side when it would leave `bounds`.
        POINT placeLoupe(POINT anchor, const RECT& bounds) {
            SIZE sz = loupeSize();
            int gap = S(24);
            POINT p{ anchor.x + gap, anchor.y + gap };
            if (p.x + sz.cx > bounds.right) p.x = anchor.x - gap - sz.cx;
            if (p.y + sz.cy > bounds.bottom) p.y = anchor.y - gap - sz.cy;
            p.x = std::max<int>(bounds.left, std::min<int>(p.x, bounds.right - sz.cx));
            p.y = std::max<int>(bounds.top, std::min<int>(p.y, bounds.bottom - sz.cy));
            return p;
        }

        // Draws a magnified view of the pixels around screen point `c` with its top-left at (left, top).
        // Pixels outside `valid` (the virtual screen) or the capture are drawn as a gray checkerboard.
        // `shown` is the coordinate printed for `c` (screen or client coordinates).
        void drawLoupe(HDC dc, int left, int top, const Capture& cap, const RECT& valid, POINT c, POINT shown) {
            const int cell = loupeCell();
            const int zoom = cell * LOUPE_CELLS;
            const SIZE sz = loupeSize();
            RECT frame{ left, top, left + sz.cx, top + sz.cy };
            fillRect(dc, frame, RGB(30, 30, 30));

            const int zx = left + 1, zy = top + 1;
            for (int dy = -LOUPE_RADIUS; dy <= LOUPE_RADIUS; ++dy) {
                for (int dx = -LOUPE_RADIUS; dx <= LOUPE_RADIUS; ++dx) {
                    int sx = c.x + dx, sy = c.y + dy;
                    bool inside = sx >= valid.left && sy >= valid.top && sx < valid.right && sy < valid.bottom &&
                        cap.contains(sx, sy);
                    COLORREF col = inside ? cap.at(sx, sy) : (((dx + dy) & 1) ? RGB(90, 90, 90) : RGB(60, 60, 60));
                    int cx = zx + (dx + LOUPE_RADIUS) * cell, cy = zy + (dy + LOUPE_RADIUS) * cell;
                    fillRect(dc, { cx, cy, cx + cell, cy + cell }, col);
                }
            }

            // Highlight the picked pixel (black + white outline so it is visible on any color)
            int cx = zx + LOUPE_RADIUS * cell, cy = zy + LOUPE_RADIUS * cell;
            RECT center{ cx, cy, cx + cell, cy + cell };
            frameRect(dc, center, RGB(0, 0, 0), 1);
            frameRect(dc, center, RGB(255, 255, 255));

            // Info: coordinates and color of the center pixel
            bool inside = c.x >= valid.left && c.y >= valid.top && c.x < valid.right && c.y < valid.bottom &&
                cap.contains(c.x, c.y);
            COLORREF col = inside ? cap.at(c.x, c.y) : RGB(0, 0, 0);
            wchar_t l1[64], l2[64], l3[64];
            swprintf_s(l1, L"X: %d   Y: %d", shown.x, shown.y);
            swprintf_s(l2, L"RGB(%d, %d, %d)", GetRValue(col), GetGValue(col), GetBValue(col));
            swprintf_s(l3, L"#%02X%02X%02X", GetRValue(col), GetGValue(col), GetBValue(col));

            HGDIOBJ oldFont = SelectObject(dc, uiFont());
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            int ty = zy + zoom + S(3);
            int lineH = S(17);
            TextOutW(dc, left + S(6), ty, l1, (int)wcslen(l1));
            TextOutW(dc, left + S(6), ty + lineH, l2, (int)wcslen(l2));
            TextOutW(dc, left + S(6), ty + lineH * 2, l3, (int)wcslen(l3));
            SelectObject(dc, oldFont);

            int sw = S(26);
            RECT swatch{ frame.right - S(6) - sw, ty + lineH, frame.right - S(6), ty + lineH + sw };
            fillRect(dc, swatch, col);
            frameRect(dc, swatch, RGB(255, 255, 255));

            frameRect(dc, frame, RGB(200, 200, 200));
        }

        void registerClass(const wchar_t* name, WNDPROC proc) {
            WNDCLASSEXW wc{ sizeof(wc) };
            if (GetClassInfoExW(GetModuleHandleW(nullptr), name, &wc)) return;
            wc.lpfnWndProc = proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
            wc.lpszClassName = name;
            RegisterClassExW(&wc);
        }

        // ------------------------------------------------------------ frozen capture picker

        struct Picker {
            bool rectMode = false;
            bool clientCoords = false;
            POINT origin{};          // subtracted from screen coordinates for display (client mode)
            RECT vs{};               // virtual screen (screen coordinates)
            Capture cap;             // frozen screen, cap pixel (0,0) == screen (vs.left, vs.top)
            Capture back;            // back buffer, same size as the window
            HWND wnd = nullptr;

            POINT cur{};             // current pixel (screen coordinates)
            bool dragging = false;
            bool keyboardDrag = false;
            POINT anchor{};
            RECT shown{};            // overlay area drawn last time (client coordinates)

            bool done = false, ok = false;
            POINT resultPt{};
            COLORREF resultColor = 0;
            RECT resultRect{};

            POINT toClient(POINT s) const { return { s.x - vs.left, s.y - vs.top }; }
            RECT toClient(RECT r) const { OffsetRect(&r, -vs.left, -vs.top); return r; }

            RECT selection() const {   // screen coordinates, right/bottom exclusive
                return { (std::min)(anchor.x, cur.x), (std::min)(anchor.y, cur.y),
                    (std::max)(anchor.x, cur.x) + 1, (std::max)(anchor.y, cur.y) + 1 };
            }

            POINT display(POINT s) const { return { s.x - origin.x, s.y - origin.y }; }

            std::wstring bannerText() const {
                std::wstring t = rectMode
                    ? Lang::tr(
                        L"Screen frozen - drag to select the region (Enter: set corner)   Arrows: 1 px (Shift: 10 px)   Esc / right click: cancel",
                        L"画面を静止中 - ドラッグで領域を選択 (Enter: 角を指定)   矢印キー: 1px移動 (Shift: 10px)   Esc / 右クリック: キャンセル")
                    : Lang::tr(
                        L"Screen frozen - Arrows: 1 px (Shift: 10 px)   Enter / click: pick   Esc / right click: cancel",
                        L"画面を静止中 - 矢印キー: 1px移動 (Shift: 10px)   Enter / クリック: 確定   Esc / 右クリック: キャンセル");
                t += clientCoords ? Lang::tr(L"   [Coords: client]", L"   [座標: クライアント]")
                    : Lang::tr(L"   [Coords: screen]", L"   [座標: 画面]");
                return t;
            }

            RECT bannerRect() const {
                RECT text{ 0, 0, 0, 0 };
                HGDIOBJ oldFont = SelectObject(back.dc, uiFont());
                DrawTextW(back.dc, bannerText().c_str(), -1, &text, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(back.dc, oldFont);
                int w = text.right + S(24), h = text.bottom + S(12);
                RECT mon = toClient(monitorRect(cur));
                POINT c = toClient(cur);
                int x = (mon.left + mon.right - w) / 2;
                int y = mon.top + S(12);
                if (c.y < y + h + S(40)) y = mon.bottom - h - S(12);   // keep it away from the cursor
                return { x, y, x + w, y + h };
            }

            RECT labelRect(const RECT& selClient, const wchar_t* text) const {
                RECT t{ 0, 0, 0, 0 };
                HGDIOBJ oldFont = SelectObject(back.dc, uiFont());
                DrawTextW(back.dc, text, -1, &t, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(back.dc, oldFont);
                int w = t.right + S(10), h = t.bottom + S(4);
                int x = selClient.left, y = selClient.bottom + S(4);
                if (y + h > back.h) y = selClient.top - h - S(4);
                if (y < 0) y = selClient.top + S(4);
                x = (std::max)(0, (std::min)(x, back.w - w));
                return { x, y, x + w, y + h };
            }

            void selectionLabel(wchar_t* buf, size_t n) const {
                RECT s = selection();
                POINT tl = display({ s.left, s.top });
                swprintf_s(buf, n, L"X %d  Y %d  W %d  H %d", tl.x, tl.y, s.right - s.left, s.bottom - s.top);
            }

            RECT loupeRect() const {
                RECT mon = toClient(monitorRect(cur));
                POINT p = placeLoupe(toClient(cur), mon);
                SIZE sz = loupeSize();
                return { p.x, p.y, p.x + sz.cx, p.y + sz.cy };
            }

            RECT overlayBounds() const {
                RECT u = loupeRect();
                RECT b = bannerRect();
                UnionRect(&u, &u, &b);
                if (rectMode && dragging) {
                    RECT s = toClient(selection());
                    wchar_t label[96];
                    selectionLabel(label, 96);
                    RECT l = labelRect(s, label);
                    InflateRect(&s, 3, 3);
                    UnionRect(&u, &u, &s);
                    UnionRect(&u, &u, &l);
                }
                return u;
            }

            // Invalidate what was drawn last time and what will be drawn now
            void refresh() {
                RECT now = overlayBounds();
                InvalidateRect(wnd, &shown, FALSE);
                InvalidateRect(wnd, &now, FALSE);
                shown = now;
            }

            POINT clamp(POINT p) const {
                p.x = std::max<LONG>(vs.left, std::min<LONG>(p.x, vs.right - 1));
                p.y = std::max<LONG>(vs.top, std::min<LONG>(p.y, vs.bottom - 1));
                return p;
            }

            void setCur(POINT p, bool moveCursor) {
                p = clamp(p);
                if (moveCursor) SetCursorPos(p.x, p.y);
                if (p.x == cur.x && p.y == cur.y) return;
                cur = p;
                refresh();
            }

            void finishPoint() {
                resultPt = cur;
                resultColor = cap.at(cur.x, cur.y);
                ok = done = true;
            }

            void finishRect(bool fromMouse) {
                RECT s = selection();
                if (fromMouse && (s.right - s.left < 2 || s.bottom - s.top < 2)) {
                    dragging = false;   // a plain click: not a region
                    refresh();
                    return;
                }
                resultRect = s;
                ok = done = true;
            }

            void cancel() { done = true; }

            void paint(HDC dc, const RECT& clip) {
                // Restore the frozen screen in the repainted area, draw the overlay, then present the area
                BitBlt(back.dc, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top,
                    cap.dc, clip.left, clip.top, SRCCOPY);

                HGDIOBJ oldFont = SelectObject(back.dc, uiFont());
                SetBkMode(back.dc, TRANSPARENT);

                if (rectMode && dragging) {
                    RECT s = toClient(selection());
                    // Outline drawn outside the selected pixels so they stay visible
                    frameRect(back.dc, s, RGB(255, 0, 0), 1);
                    frameRect(back.dc, s, RGB(255, 255, 255), 2);
                    wchar_t label[96];
                    selectionLabel(label, 96);
                    RECT l = labelRect(s, label);
                    fillRect(back.dc, l, RGB(30, 30, 30));
                    SetTextColor(back.dc, RGB(255, 255, 255));
                    DrawTextW(back.dc, label, -1, &l, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }

                RECT b = bannerRect();
                fillRect(back.dc, b, RGB(30, 30, 30));
                frameRect(back.dc, b, RGB(255, 200, 0));
                SetTextColor(back.dc, RGB(255, 255, 255));
                DrawTextW(back.dc, bannerText().c_str(), -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(back.dc, oldFont);

                RECT lr = loupeRect();
                drawLoupe(back.dc, lr.left, lr.top, cap, vs, cur, display(cur));

                BitBlt(dc, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top,
                    back.dc, clip.left, clip.top, SRCCOPY);
            }
        };

        const wchar_t PICKER_CLASS[] = L"ArgusShiftScreenPicker";

        POINT screenPoint(Picker* p, LPARAM lp) {
            return { GET_X_LPARAM(lp) + p->vs.left, GET_Y_LPARAM(lp) + p->vs.top };
        }

        LRESULT CALLBACK pickerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
            Picker* p = reinterpret_cast<Picker*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (msg == WM_NCCREATE) {
                p = reinterpret_cast<Picker*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                p->wnd = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
            }
            if (!p) return DefWindowProcW(hwnd, msg, wp, lp);

            switch (msg) {
            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                p->paint(dc, ps.rcPaint);
                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_MOUSEMOVE:
                p->setCur(screenPoint(p, lp), false);
                return 0;

            case WM_LBUTTONDOWN:
                p->setCur(screenPoint(p, lp), false);
                if (!p->rectMode) {
                    p->finishPoint();
                }
                else {
                    p->anchor = p->cur;
                    p->dragging = true;
                    p->keyboardDrag = false;
                    SetCapture(hwnd);
                    p->refresh();
                }
                return 0;

            case WM_LBUTTONUP:
                if (p->rectMode && p->dragging && !p->keyboardDrag) {
                    ReleaseCapture();
                    p->setCur(screenPoint(p, lp), false);
                    p->finishRect(true);
                }
                return 0;

            case WM_RBUTTONUP:
                p->cancel();
                return 0;

            case WM_KEYDOWN: {
                int step = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
                switch (wp) {
                case VK_LEFT:  p->setCur({ p->cur.x - step, p->cur.y }, true); return 0;
                case VK_RIGHT: p->setCur({ p->cur.x + step, p->cur.y }, true); return 0;
                case VK_UP:    p->setCur({ p->cur.x, p->cur.y - step }, true); return 0;
                case VK_DOWN:  p->setCur({ p->cur.x, p->cur.y + step }, true); return 0;
                case VK_RETURN:
                case VK_SPACE:
                    if (!p->rectMode) p->finishPoint();
                    else if (!p->dragging) {
                        p->anchor = p->cur;
                        p->dragging = true;
                        p->keyboardDrag = true;
                        p->refresh();
                    }
                    else p->finishRect(false);
                    return 0;
                case VK_ESCAPE:
                    if (p->rectMode && p->dragging) {
                        if (GetCapture() == hwnd) ReleaseCapture();
                        p->dragging = false;
                        p->refresh();
                    }
                    else p->cancel();
                    return 0;
                }
                break;
            }

            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE) p->cancel();   // switched to another window
                break;

            case WM_CLOSE:
                p->cancel();
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        bool runPicker(HWND owner, Picker& p) {
            registerClass(PICKER_CLASS, pickerProc);

            // Freeze the screen first: this is the moment the user asked for
            p.vs = virtualScreen();
            int w = p.vs.right - p.vs.left, h = p.vs.bottom - p.vs.top;
            if (!p.cap.grab(p.vs.left, p.vs.top, w, h)) return false;
            if (!p.back.allocate(w, h)) return false;
            GetCursorPos(&p.cur);
            p.cur = p.clamp(p.cur);

            // Borderless window exactly over the virtual screen: client (0,0) == screen (vs.left, vs.top),
            // and the capture is drawn 1:1, so every pixel shown is at its real screen position.
            HWND wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, PICKER_CLASS, L"ArgusShift picker",
                WS_POPUP, p.vs.left, p.vs.top, w, h, owner, nullptr, GetModuleHandleW(nullptr), &p);
            if (!wnd) return false;

            if (owner) EnableWindow(owner, FALSE);
            ShowWindow(wnd, SW_SHOW);
            SetForegroundWindow(wnd);
            SetFocus(wnd);
            p.shown = { 0, 0, w, h };
            InvalidateRect(wnd, nullptr, FALSE);

            MSG m;
            bool quit = false;
            int quitCode = 0;
            while (!p.done) {
                BOOL r = GetMessageW(&m, nullptr, 0, 0);
                if (r <= 0) {
                    quit = true;
                    quitCode = (int)m.wParam;
                    break;
                }
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            p.done = true;   // ignore the WM_ACTIVATE sent while closing

            if (owner) EnableWindow(owner, TRUE);
            DestroyWindow(wnd);
            if (owner) SetForegroundWindow(owner);
            if (quit) PostQuitMessage(quitCode);
            return p.ok;
        }
    }

    // ------------------------------------------------------------ pickers

    namespace {
        void applyDisplay(Picker& p, const Display& d) {
            p.clientCoords = d.clientOrigin != nullptr;
            if (d.clientOrigin) p.origin = *d.clientOrigin;
        }
    }

    bool pickPoint(HWND owner, POINT& pt, COLORREF& color, Display display) {
        Picker p;
        p.rectMode = false;
        applyDisplay(p, display);
        if (!runPicker(owner, p)) return false;
        pt = p.resultPt;
        color = p.resultColor;
        return true;
    }

    bool pickRect(HWND owner, RECT& rc, Display display) {
        Picker p;
        p.rectMode = true;
        applyDisplay(p, display);
        if (!runPicker(owner, p)) return false;
        rc = p.resultRect;
        return true;
    }

    bool pickImage(HWND owner, RECT& rc, std::vector<uint32_t>& pixels, Display display) {
        Picker p;
        p.rectMode = true;
        applyDisplay(p, display);
        if (!runPicker(owner, p)) return false;
        rc = p.resultRect;
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        pixels.resize((size_t)w * h);
        for (int y = 0; y < h; ++y) {
            const uint32_t* src = p.cap.bits + (size_t)(rc.top + y - p.cap.y0) * p.cap.w + (rc.left - p.cap.x0);
            std::copy(src, src + w, pixels.begin() + (size_t)y * w);
        }
        return true;
    }
}
