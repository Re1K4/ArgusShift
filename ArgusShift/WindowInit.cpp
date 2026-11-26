#include "WindowInit.h"

// char* Å® wstring
static std::wstring ToWide(const char* s) {
    if (!s) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (len <= 0) {
        len = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
        if (len <= 0) return L"";
        std::wstring w(len - 1, L'\0');
        MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), len);
        return w;
    }
    std::wstring w(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), len);
    return w;
}

// wstring -> char* (ANSI) for log messages
static std::string ToNarrow(const wchar_t* s) {
    if (!s) return "(null)";
    int len = WideCharToMultiByte(CP_ACP, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, s, -1, out.data(), len, nullptr, nullptr);
    return out;
}

static bool IsMainWindowCandidate(HWND hwnd) {
    return IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER) && GetWindowTextLengthW(hwnd) > 0;
}

// Search for the top-level window by process name.
// A visible, unowned window with a title (the app's main window) is preferred.
HWND FindTargetWindow(const wchar_t* targetExe) {
    struct Ctx { const wchar_t* target; HWND hit; HWND fallback; } ctx{ targetExe, nullptr, nullptr };
    if (!targetExe || !*targetExe) return nullptr;

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lParam);

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return TRUE;

        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) return TRUE;

        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        BOOL ok = QueryFullProcessImageNameW(hProc, 0, path, &size);
        CloseHandle(hProc);
        if (!ok) return TRUE;

        // Extract the filename portion
        const wchar_t* last = wcsrchr(path, L'\\');
        const wchar_t* name = last ? (last + 1) : path;

        if (_wcsicmp(name, c->target) == 0) {
            if (IsMainWindowCandidate(hwnd)) {
                c->hit = hwnd;
                return FALSE; // Found, so listing complete.
            }
            if (!c->fallback) c->fallback = hwnd;
        }
        return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.hit ? ctx.hit : ctx.fallback;
}

static void UpdateSharedWindowSize(HWND app_window) {
    RECT rect;
    GetWindowRect(app_window, &rect);
    SharedVariable::window_size_x = rect.right - rect.left;
    SharedVariable::window_size_y = rect.bottom - rect.top;
    SharedVariable::window_center_x = SharedVariable::window_size_x / 2;
    SharedVariable::window_center_y = SharedVariable::window_size_y / 2;
}

void WindowInit(const char* app_name) {
    if (!TryWindowInit(app_name)) {
        std::exit(EXIT_FAILURE);
    }
}

bool TryWindowInit(const char* app_name) {
    std::wstring targetExe = ToWide(app_name);
    HWND app_window = FindTargetWindow(targetExe.c_str());

    if (!app_window) {
        Logger::error(std::string("Process window not found: ") + (app_name ? app_name : "(null)"));
        return false;
    }

    // Set window in foreground
    SetForegroundWindow(app_window);
    UpdateSharedWindowSize(app_window);

    // Update window position and size
    MoveWindow(app_window, 0, 0, SharedVariable::window_size_x, SharedVariable::window_size_y, TRUE);
    return true;
}

bool TryWindowInitRect(const wchar_t* targetExe, int x, int y, int w, int h) {
    HWND app_window = FindTargetWindow(targetExe);
    if (!app_window) {
        Logger::error("Process window not found: " + std::string(ToNarrow(targetExe)));
        return false;
    }

    // A maximized / minimized window ignores MoveWindow, so restore it first
    if (IsIconic(app_window) || IsZoomed(app_window)) ShowWindow(app_window, SW_RESTORE);
    SetForegroundWindow(app_window);
    MoveWindow(app_window, x, y, w, h, TRUE);
    UpdateSharedWindowSize(app_window);

    RECT r;
    GetWindowRect(app_window, &r);
    if (r.left != x || r.top != y || r.right - r.left != w || r.bottom - r.top != h) {
        Logger::warning("Target window placed at (" + std::to_string(r.left) + ", " + std::to_string(r.top) + ") " +
            std::to_string(r.right - r.left) + "x" + std::to_string(r.bottom - r.top) +
            " instead of the requested placement (the app may limit its size)");
    }
    else {
        Logger::info("Target window fixed at (" + std::to_string(x) + ", " + std::to_string(y) + ") " +
            std::to_string(w) + "x" + std::to_string(h));
    }
    return true;
}

bool GetTargetWindowRect(const wchar_t* targetExe, RECT& rect) {
    HWND app_window = FindTargetWindow(targetExe);
    if (!app_window) return false;
    return GetWindowRect(app_window, &rect) != FALSE;
}

bool GetClientOrigin(HWND hwnd, POINT& origin) {
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return false;
    POINT p{ 0, 0 };
    if (!ClientToScreen(hwnd, &p)) return false;
    origin = p;
    return true;
}

bool GetTargetClientOrigin(const wchar_t* targetExe, POINT& origin) {
    return GetClientOrigin(FindTargetWindow(targetExe), origin);
}

bool TryActivateWindow(const wchar_t* targetExe) {
    HWND app_window = FindTargetWindow(targetExe);
    if (!app_window) {
        Logger::error("Process window not found: " + std::string(ToNarrow(targetExe)));
        return false;
    }
    if (IsIconic(app_window)) ShowWindow(app_window, SW_RESTORE);
    SetForegroundWindow(app_window);
    UpdateSharedWindowSize(app_window);
    return true;
}
