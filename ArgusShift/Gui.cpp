#include "Gui.h"
#include "AutomationConfig.h"
#include "AutomationRunner.h"
#include "Event.h"
#include "Lang.h"
#include "Logger.h"
#include "ScreenPicker.h"
#include "TemplateMatching.h"
#include "WindowInit.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace Automation;

namespace {

    constexpr UINT WM_APP_LOG = WM_APP + 1;
    constexpr UINT_PTR TIMER_STATUS = 1;
    constexpr int HOTKEY_CAPTURE = 1;      // F1: freeze the screen and pick on the capture
    constexpr int HOTKEY_CANCEL = 3;       // Esc: cancel the pick

    enum Page { PAGE_EVENTS = 0, PAGE_TIMERS = 1, PAGE_STEPS = 2, PAGE_COUNT = 3 };
    enum class PickTarget { None, Event, StepAction, TemplateRegion, TemplateImage };

    enum : int {
        ID_TARGET = 100, ID_POLL, ID_NEW, ID_OPEN, ID_SAVE, ID_SAVEAS, ID_RUN, ID_STOP, ID_STATUS, ID_TAB, ID_LOG, ID_LANG,
        ID_WFIX, ID_WX, ID_WY, ID_WW, ID_WH, ID_WGET, ID_WAPPLY, ID_COORD,

        ID_LIST = 200,     // + page
        ID_ADD = 210,      // + page
        ID_REMOVE = 220,   // + page
        ID_UP = 230,       // + page
        ID_DOWN = 240,     // + page
        ID_COUNT = 250,    // + page

        // Event form (300-399)
        EV_NAME = 300, EV_X, EV_Y, EV_R, EV_G, EV_B, EV_FUZZY, EV_TYPE,
        EV_PREVIEW = 380, EV_PICK, EV_TEST,

        // Timer form (400-499)
        TM_NAME = 400, TM_SEC,

        // Step form (500-599)
        ST_NAME = 500, ST_COND, ST_NEGATE, ST_DEBUG, ST_EVENTS, ST_MATCH, ST_CTIMER, ST_TPATH,
        ST_RX, ST_RY, ST_RW, ST_RH, ST_THRESH, ST_ACTION, ST_TEXT, ST_AX, ST_AY, ST_MS, ST_ATIMER,
        ST_NTRUE, ST_NFALSE, ST_TIMEOUT, ST_NTIMEOUT,
        ST_TBROWSE = 580, ST_APICK, ST_TEXTHINT, ST_MSHINT, ST_RPICK, ST_TCAPTURE,
    };

    // ------------------------------------------------------------ localized text

    // An English / Japanese pair. Controls keep their pair so the language can be switched live.
    struct Text {
        const wchar_t* en;
        const wchar_t* ja;
        const wchar_t* get() const { return Lang::tr(en, ja); }
    };

    const Text TAB_NAMES[PAGE_COUNT] = {
        { L"Points", L"認識点" }, { L"Timers", L"タイマー" }, { L"Steps", L"ステップ" },
    };

    const wchar_t* projectFilter() {
        return Lang::tr(L"ArgusShift project (*.argus;*.cppauto)\0*.argus;*.cppauto\0All files\0*.*\0",
            L"ArgusShift プロジェクト (*.argus;*.cppauto)\0*.argus;*.cppauto\0すべてのファイル\0*.*\0");
    }

    // ------------------------------------------------------------ small helpers

    // Executable names (e.g. "notepad.exe") of processes that own a visible top-level window,
    // i.e. the processes WindowInit can actually target. Sorted case-insensitively, unique.
    std::vector<std::wstring> listWindowProcesses() {
        std::vector<std::wstring> names;
        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* out = reinterpret_cast<std::vector<std::wstring>*>(lParam);
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
            if (GetWindowTextLengthW(hwnd) == 0) return TRUE;

            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (!pid || pid == GetCurrentProcessId()) return TRUE;

            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!hProc) return TRUE;
            wchar_t path[MAX_PATH] = {};
            DWORD size = MAX_PATH;
            BOOL ok = QueryFullProcessImageNameW(hProc, 0, path, &size);
            CloseHandle(hProc);
            if (!ok) return TRUE;

            const wchar_t* last = wcsrchr(path, L'\\');
            out->emplace_back(last ? last + 1 : path);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&names));

        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });
        names.erase(std::unique(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(a.c_str(), b.c_str()) == 0;
        }), names.end());
        return names;
    }

    std::wstring getText(HWND h) {
        int n = GetWindowTextLengthW(h);
        std::wstring s(n, L'\0');
        if (n > 0) GetWindowTextW(h, &s[0], n + 1);
        return s;
    }

    int getInt(HWND h) {
        return (int)wcstol(getText(h).c_str(), nullptr, 10);
    }

    double getDouble(HWND h) {
        return wcstod(getText(h).c_str(), nullptr);
    }

    std::wstring fmtDouble(double v) {
        wchar_t buf[64];
        swprintf_s(buf, L"%g", v);
        return buf;
    }

    std::wstring nextLabel(int next) {
        switch (next) {
        case NEXT_NEXT: return Lang::tr(L"next", L"次");
        case NEXT_END:  return Lang::tr(L"end", L"終了");
        case NEXT_STAY: return Lang::tr(L"stay", L"留まる");
        default:        return std::to_wstring(next);
        }
    }

    // Transition combo: 0 = next, 1 = end, 2 = stay, 3+ = step index
    int nextToCombo(int next) {
        if (next == NEXT_NEXT) return 0;
        if (next == NEXT_END) return 1;
        if (next == NEXT_STAY) return 2;
        return next + 3;
    }

    int comboToNext(int sel) {
        if (sel <= 0) return NEXT_NEXT;
        if (sel == 1) return NEXT_END;
        if (sel == 2) return NEXT_STAY;
        return sel - 3;
    }

    std::wstring exeDirectory() {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::wstring p(buf);
        size_t pos = p.find_last_of(L"\\/");
        return pos == std::wstring::npos ? L"" : p.substr(0, pos);
    }

    // ------------------------------------------------------------ main window

    class App {
    public:
        int run(const wchar_t* initialProject);

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
        LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

        int S(int v) const { return MulDiv(v, dpi, 96); }
        HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
            int id, int page, bool formInput = false, DWORD exStyle = 0);
        HWND label(Text text, int x, int y, int w, int page, int id = -1);
        HWND button(Text text, int id, int x, int y, int w, int h, int page);
        HWND edit(int id, int x, int y, int w, int page);
        HWND combo(int id, int x, int y, int w, int page);
        HWND checkbox(Text text, int id, int x, int y, int w, int page);
        HWND bindText(HWND h, Text text);

        void createControls();
        void captureLayout();
        void layoutControls(int clientW, int clientH);
        void createEventForm();
        void createTimerForm();
        void createStepForm();
        void addColumn(int page, Text title, int width);
        void fillTypeCombos();
        void applyLanguage();

        void showPage(int page);
        int selected(int page) const;
        std::vector<std::wstring> rowCells(int page, int index) const;
        void setRow(int page, int index, bool insert);
        void refreshList(int page, int select);
        void updateCountLabel(int page);
        void loadForm(int page);
        void saveForm(int page);
        void updatePreview();
        void updateStepFieldStates();
        void fillNextCombo(HWND combo, int value);
        void fillTimerCombo(HWND combo, int value);

        void onAdd(int page);
        void onRemove(int page);
        void onMove(int page, int dir);
        void onPickStart(PickTarget target);
        void endPick();
        void onPickHotkey(int id);
        void applyPickedPoint(PickTarget t, POINT pt, COLORREF c);
        void applyPickedRect(const RECT& rc);
        void applyPickedImage(const RECT& rc, const std::vector<uint32_t>& pixels);
        void onTestEvent();
        void onBrowseTemplate();
        void onRun();
        void onStop();
        void refreshTargetList();
        void readWindowFields();
        void onGetWindowRect();
        void onApplyWindowRect();
        void fillCoordCombo();
        void onCoordModeChange();
        bool coordOrigin(POINT& origin);

        void newProject();
        void openProject();
        bool saveProjectFile(bool saveAs);
        bool openProjectPath(const std::wstring& path);
        bool confirmDiscard();
        void loadProjectToUi();
        void markDirty();
        void updateTitle();
        void setStatus(Text text, const std::wstring& suffix = L"");
        void updateRunState();
        void appendLog(const std::wstring& line);

        HWND hwnd = nullptr;
        HFONT font = nullptr;
        int dpi = 96;

        // Resizable layout: every direct child is placed relative to its position in the
        // base (designed) client size, according to how it is anchored.
        enum Anchor : unsigned {
            MoveX = 1, MoveYTop = 2, StretchW = 4, StretchHTop = 8, StretchHBottom = 16,
        };
        struct LayoutItem { HWND h; RECT base; unsigned anchor; };
        std::vector<LayoutItem> layout;
        static constexpr int BASE_W = 1040, BASE_H = 770;   // designed client size (96 dpi)

        HWND target = nullptr, poll = nullptr, status = nullptr, tab = nullptr, logView = nullptr;
        HWND btnRun = nullptr, btnStop = nullptr, langCombo = nullptr, coordCombo = nullptr;
        HWND wFix = nullptr, wX = nullptr, wY = nullptr, wW = nullptr, wH = nullptr;
        std::vector<std::pair<HWND, Text>> texts;   // controls whose caption follows the language
        std::vector<Text> columns[PAGE_COUNT];
        Text statusText{ L"", L"" };
        std::wstring statusSuffix;
        HWND list[PAGE_COUNT] = {};
        HWND countLabel[PAGE_COUNT] = {};
        std::vector<HWND> pageCtrls[PAGE_COUNT];
        std::vector<HWND> formCtrls[PAGE_COUNT];

        // Event form
        HWND evName{}, evX{}, evY{}, evR{}, evG{}, evB{}, evFuzzy{}, evType{}, evPreview{};
        HBRUSH previewBrush = nullptr;
        // Timer form
        HWND tmName{}, tmSec{};
        // Step form
        HWND stName{}, stCond{}, stNegate{}, stDebug{}, stEvents{}, stMatch{}, stCTimer{}, stTPath{}, stTBrowse{}, stTCapture{};
        HWND stRX{}, stRY{}, stRW{}, stRH{}, stRPick{}, stThresh{}, stAction{}, stText{}, stTextHint{}, stAX{}, stAY{}, stAPick{};
        HWND stMs{}, stMsHint{}, stATimer{}, stNTrue{}, stNFalse{}, stTimeout{}, stNTimeout{};

        Project project;
        AutomationRunner runner;
        std::wstring projectPath;
        bool dirty = false;
        bool loadingForm = false;
        bool refreshing = false;
        int currentPage = PAGE_EVENTS;
        bool wasRunning = false;

        PickTarget pickTarget = PickTarget::None;
    };

    // ------------------------------------------------------------ control creation

    HWND App::add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
        int id, int page, bool formInput, DWORD exStyle) {
        HWND h_ = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
            S(x), S(y), S(w), S(h), hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(h_, WM_SETFONT, (WPARAM)font, TRUE);
        if (page >= 0) {
            pageCtrls[page].push_back(h_);
            if (formInput) formCtrls[page].push_back(h_);
        }
        return h_;
    }

    HWND App::bindText(HWND h, Text text) {
        texts.emplace_back(h, text);
        return h;
    }

    HWND App::label(Text text, int x, int y, int w, int page, int id) {
        HWND h = add(L"STATIC", text.get(), SS_LEFT, x, y + 4, w, 18, id, page);
        return *text.en ? bindText(h, text) : h;  // empty labels are hints set by code
    }

    HWND App::button(Text text, int id, int x, int y, int w, int h, int page) {
        return bindText(add(L"BUTTON", text.get(), WS_TABSTOP, x, y, w, h, id, page), text);
    }

    HWND App::edit(int id, int x, int y, int w, int page) {
        return add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, 23, id, page, true, WS_EX_CLIENTEDGE);
    }

    HWND App::combo(int id, int x, int y, int w, int page) {
        return add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, x, y, w, 300, id, page, true);
    }

    HWND App::checkbox(Text text, int id, int x, int y, int w, int page) {
        return bindText(add(L"BUTTON", text.get(), WS_TABSTOP | BS_AUTOCHECKBOX, x, y + 2, w, 20, id, page, true), text);
    }

    void App::addColumn(int page, Text title, int width) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<LPWSTR>(title.get());
        c.cx = S(width);
        ListView_InsertColumn(list[page], (int)columns[page].size(), &c);
        columns[page].push_back(title);
    }

    // Condition / action combos list localized names: rebuild them keeping the selection
    void App::fillTypeCombos() {
        bool wasLoading = loadingForm;
        loadingForm = true;
        int c = (int)SendMessageW(stCond, CB_GETCURSEL, 0, 0);
        SendMessageW(stCond, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i < (int)ConditionType::Count; ++i)
            SendMessageW(stCond, CB_ADDSTRING, 0, (LPARAM)conditionName((ConditionType)i));
        SendMessageW(stCond, CB_SETCURSEL, c, 0);

        int a = (int)SendMessageW(stAction, CB_GETCURSEL, 0, 0);
        SendMessageW(stAction, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i < (int)ActionType::Count; ++i)
            SendMessageW(stAction, CB_ADDSTRING, 0, (LPARAM)actionName((ActionType)i));
        SendMessageW(stAction, CB_SETCURSEL, a, 0);
        loadingForm = wasLoading;
    }

    void App::applyLanguage() {
        for (const auto& t : texts) SetWindowTextW(t.first, t.second.get());
        for (int i = 0; i < PAGE_COUNT; ++i) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<LPWSTR>(TAB_NAMES[i].get());
            TabCtrl_SetItem(tab, i, &item);
        }
        for (int p = 0; p < PAGE_COUNT; ++p) {
            for (int c = 0; c < (int)columns[p].size(); ++c) {
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT;
                col.pszText = const_cast<LPWSTR>(columns[p][c].get());
                ListView_SetColumn(list[p], c, &col);
            }
        }
        fillTypeCombos();
        fillCoordCombo();
        // Rows and forms contain localized names (conditions, actions, transitions, hints)
        for (int p = 0; p < PAGE_COUNT; ++p) refreshList(p, selected(p));
        updateTitle();
        setStatus(statusText, statusSuffix);
        InvalidateRect(hwnd, nullptr, TRUE);
    }

    void App::createControls() {
        // ---- Top bar
        label({ L"Target exe", L"対象exe" }, 10, 10, 68, -1);
        // Editable combo: type a name or pick from the running processes (refreshed on dropdown)
        target = add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_SORT,
            80, 10, 150, 300, ID_TARGET, -1);
        SendMessageW(target, CB_SETDROPPEDWIDTH, S(260), 0);
        SendMessageW(target, CB_SETMINVISIBLE, 20, 0);
        label({ L"Poll ms", L"周期ms" }, 240, 10, 48, -1);
        poll = add(L"EDIT", L"", WS_TABSTOP | ES_NUMBER, 290, 10, 55, 23, ID_POLL, -1, false, WS_EX_CLIENTEDGE);
        button({ L"New", L"新規" }, ID_NEW, 355, 9, 55, 26, -1);
        button({ L"Open...", L"開く..." }, ID_OPEN, 415, 9, 60, 26, -1);
        button({ L"Save", L"保存" }, ID_SAVE, 480, 9, 55, 26, -1);
        button({ L"Save As...", L"別名保存..." }, ID_SAVEAS, 540, 9, 70, 26, -1);
        btnRun = button({ L"Run", L"実行" }, ID_RUN, 625, 9, 70, 26, -1);
        btnStop = button({ L"Stop", L"停止" }, ID_STOP, 700, 9, 70, 26, -1);
        status = add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 780, 14, 250, 18, ID_STATUS, -1);

        // ---- Tabs (header strip only, pages are laid out on the window)
        tab = add(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, 10, 45, 250, 28, ID_TAB, -1);
        for (int i = 0; i < PAGE_COUNT; ++i) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<LPWSTR>(TAB_NAMES[i].get());
            TabCtrl_InsertItem(tab, i, &item);
        }

        // ---- Target window placement (fixed position / size, applied by Apply and on Run)
        wFix = checkbox({ L"Fix window", L"ウィンドウ固定" }, ID_WFIX, 270, 47, 105, -1);
        label({ L"X", L"X" }, 380, 47, 12, -1);
        wX = add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 393, 47, 45, 23, ID_WX, -1, false, WS_EX_CLIENTEDGE);
        label({ L"Y", L"Y" }, 443, 47, 12, -1);
        wY = add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 456, 47, 45, 23, ID_WY, -1, false, WS_EX_CLIENTEDGE);
        label({ L"W", L"幅" }, 506, 47, 18, -1);
        wW = add(L"EDIT", L"", WS_TABSTOP | ES_NUMBER, 525, 47, 50, 23, ID_WW, -1, false, WS_EX_CLIENTEDGE);
        label({ L"H", L"高" }, 580, 47, 18, -1);
        wH = add(L"EDIT", L"", WS_TABSTOP | ES_NUMBER, 599, 47, 50, 23, ID_WH, -1, false, WS_EX_CLIENTEDGE);
        button({ L"Get current", L"現在値取得" }, ID_WGET, 657, 45, 80, 26, -1);
        button({ L"Apply", L"適用" }, ID_WAPPLY, 742, 45, 60, 26, -1);

        // ---- Coordinate mode: client (follows the target window) or absolute screen
        label({ L"Coords", L"座標" }, 810, 47, 40, -1);
        coordCombo = add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 852, 47, 100, 200, ID_COORD, -1);
        SendMessageW(coordCombo, CB_SETDROPPEDWIDTH, S(130), 0);
        fillCoordCombo();

        // ---- UI language selector (right of the tab strip; the item names are self-explanatory)
        langCombo = add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 958, 47, 72, 200, ID_LANG, -1);
        for (int i = 0; i < (int)Lang::Language::Count; ++i)
            SendMessageW(langCombo, CB_ADDSTRING, 0, (LPARAM)Lang::displayName((Lang::Language)i));
        SendMessageW(langCombo, CB_SETCURSEL, (int)Lang::current(), 0);

        // ---- List + buttons for each page
        for (int p = 0; p < PAGE_COUNT; ++p) {
            list[p] = add(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                10, 82, 480, 400, ID_LIST + p, p, false, WS_EX_CLIENTEDGE);
            ListView_SetExtendedListViewStyle(list[p], LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            button({ L"Add", L"追加" }, ID_ADD + p, 10, 488, 70, 26, p);
            button({ L"Remove", L"削除" }, ID_REMOVE + p, 85, 488, 70, 26, p);
            button({ L"Up", L"上へ" }, ID_UP + p, 160, 488, 55, 26, p);
            button({ L"Down", L"下へ" }, ID_DOWN + p, 220, 488, 55, 26, p);
            countLabel[p] = add(L"STATIC", L"", SS_LEFT, 285, 493, 200, 18, ID_COUNT + p, p);
        }

        addColumn(PAGE_EVENTS, { L"#", L"#" }, 32);
        addColumn(PAGE_EVENTS, { L"Name", L"名前" }, 130);
        addColumn(PAGE_EVENTS, { L"X", L"X" }, 50);
        addColumn(PAGE_EVENTS, { L"Y", L"Y" }, 50);
        addColumn(PAGE_EVENTS, { L"RGB", L"RGB" }, 95);
        addColumn(PAGE_EVENTS, { L"Fuzzy", L"誤差" }, 45);
        addColumn(PAGE_EVENTS, { L"Type", L"種別" }, 65);

        addColumn(PAGE_TIMERS, { L"#", L"#" }, 32);
        addColumn(PAGE_TIMERS, { L"Name", L"名前" }, 250);
        addColumn(PAGE_TIMERS, { L"Seconds", L"秒数" }, 100);

        addColumn(PAGE_STEPS, { L"#", L"#" }, 32);
        addColumn(PAGE_STEPS, { L"Name", L"名前" }, 105);
        addColumn(PAGE_STEPS, { L"Condition", L"条件" }, 115);
        addColumn(PAGE_STEPS, { L"Action", L"アクション" }, 100);
        addColumn(PAGE_STEPS, { L"True >", L"真 >" }, 55);
        addColumn(PAGE_STEPS, { L"False >", L"偽 >" }, 55);

        createEventForm();
        createTimerForm();
        createStepForm();

        // ---- Log
        label({ L"Log  (ESC stops a running automation)", L"ログ  (ESCキーで実行中の自動化を停止)" }, 10, 518, 400, -1);
        logView = add(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            10, 540, 1020, 220, ID_LOG, -1, false, WS_EX_CLIENTEDGE);
        SendMessageW(logView, EM_SETLIMITTEXT, 0, 0);
    }

    void App::captureLayout() {
        layout.clear();
        // Direct children only (a combo box's inner edit moves with its parent)
        for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            RECT r;
            GetWindowRect(c, &r);
            MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&r), 2);

            unsigned a = 0;
            bool isList = false;
            for (int p = 0; p < PAGE_COUNT; ++p) isList |= c == list[p];
            if (isList)                                   a = StretchW | StretchHTop;        // item lists
            else if (c == logView)                        a = MoveYTop | StretchW | StretchHBottom;
            else if (c == status)                         a = StretchW;                      // top strip
            else if (r.top < S(80) && r.left >= S(885))   a = MoveX;                         // language selector
            else if (r.top >= S(80) && r.left >= S(500))  a = MoveX;                         // edit form (right side)
            else if (r.top >= S(480))                     a = MoveYTop;                      // list buttons, log label
            layout.push_back({ c, r, a });
        }
    }

    void App::layoutControls(int clientW, int clientH) {
        if (layout.empty()) return;
        int dx = (std::max)(0, clientW - S(BASE_W));
        int dy = (std::max)(0, clientH - S(BASE_H));
        int dyTop = dy / 2, dyBottom = dy - dyTop;   // extra height is shared by the list and the log

        HDWP dwp = BeginDeferWindowPos((int)layout.size());
        for (const auto& it : layout) {
            if (!it.anchor) continue;
            int x = it.base.left, y = it.base.top;
            int w = it.base.right - it.base.left, h = it.base.bottom - it.base.top;
            if (it.anchor & MoveX)          x += dx;
            if (it.anchor & MoveYTop)       y += dyTop;
            if (it.anchor & StretchW)       w += dx;
            if (it.anchor & StretchHTop)    h += dyTop;
            if (it.anchor & StretchHBottom) h += dyBottom;
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
            // Keep the dropped-down height of combo boxes: only move controls that do not stretch
            if (!(it.anchor & (StretchW | StretchHTop | StretchHBottom))) flags |= SWP_NOSIZE;
            if (dwp) dwp = DeferWindowPos(dwp, it.h, nullptr, x, y, w, h, flags);
            else SetWindowPos(it.h, nullptr, x, y, w, h, flags);
        }
        if (dwp) EndDeferWindowPos(dwp);
        InvalidateRect(hwnd, nullptr, TRUE);
    }

    void App::createEventForm() {
        const int P = PAGE_EVENTS, LX = 510, LW = 110, X = 625;
        label({ L"Name", L"名前" }, LX, 82, LW, P);          evName = edit(EV_NAME, X, 82, 300, P);
        label({ L"X", L"X" }, LX, 108, LW, P);   evX = edit(EV_X, X, 108, 80, P);
        label({ L"Y", L"Y" }, LX, 134, LW, P);   evY = edit(EV_Y, X, 134, 80, P);
        label({ L"R / G / B", L"R / G / B" }, LX, 160, LW, P);
        evR = edit(EV_R, X, 160, 55, P);
        evG = edit(EV_G, X + 60, 160, 55, P);
        evB = edit(EV_B, X + 120, 160, 55, P);
        label({ L"Fuzzy", L"許容誤差" }, LX, 186, LW, P);        evFuzzy = edit(EV_FUZZY, X, 186, 80, P);
        label({ L"tolerance per channel (0-255)", L"チャンネルごとの許容差 (0-255)" }, X + 90, 186, 250, P);
        label({ L"Color type", L"色の種別" }, LX, 212, LW, P);   evType = combo(EV_TYPE, X, 212, 120, P);
        label({ L"used by 'Point RGB type'", L"「認識点RGB種別」で使用" }, X + 130, 212, 250, P);
        for (int i = 0; i < COLOR_TYPE_COUNT; ++i) SendMessageW(evType, CB_ADDSTRING, 0, (LPARAM)COLOR_TYPES[i]);
        label({ L"Color", L"色" }, LX, 244, LW, P);
        evPreview = add(L"STATIC", L"", WS_BORDER, X, 244, 80, 36, EV_PREVIEW, P);
        formCtrls[P].push_back(button({ L"Pick from screen (F1: capture)", L"画面から取得 (F1: キャプチャ)" }, EV_PICK, X, 296, 200, 28, P));
        formCtrls[P].push_back(button({ L"Test now (writes to log)", L"今すぐテスト (ログに出力)" }, EV_TEST, X, 330, 200, 28, P));
        const Text help{
            L"A detection point checks the pixel at (X, Y). Coordinates follow the 'Coords' setting: "
            L"Client = from the top-left of the target window's content, Screen = absolute.\r\n"
            L"Use it from a step with the condition 'Point color' (color within Fuzzy) "
            L"or 'Point RGB type' (hue category). Selecting 2 or more points in a step "
            L"uses ALL / ANY matching.\r\n"
            L"Pick from screen: F1 freezes the screen and opens the capture picker "
            L"(magnifier, arrow keys move 1 px, Enter / click to confirm).",
            L"認識点は座標 (X, Y) のピクセルを判定します。座標は「座標」設定に従います "
            L"(クライアント = 対象ウィンドウの表示領域の左上から / 画面 = 絶対座標)。\r\n"
            L"ステップの条件「認識点の色」(許容誤差内の色) または「認識点RGB種別」(色相カテゴリ) で使用します。"
            L"1つのステップで2つ以上の認識点を選択すると、ALL / ANY 判定を行います。\r\n"
            L"画面から取得: F1キーで画面を静止キャプチャして選択できます "
            L"(ルーペ表示、矢印キーで1ピクセル移動、Enter / クリックで確定)。" };
        bindText(add(L"STATIC", help.get(), SS_LEFT, LX, 372, 500, 80, -1, P), help);
    }

    void App::createTimerForm() {
        const int P = PAGE_TIMERS, LX = 510, LW = 110, X = 625;
        label({ L"Name", L"名前" }, LX, 82, LW, P);      tmName = edit(TM_NAME, X, 82, 300, P);
        label({ L"Seconds", L"秒数" }, LX, 108, LW, P);  tmSec = edit(TM_SEC, X, 108, 80, P);
        const Text help{
            L"Start or reset a timer with the step actions 'Timer start' / 'Timer reset', "
            L"then wait for it with the condition 'Timer elapsed' (true when the timer has "
            L"been running for at least Seconds).",
            L"ステップのアクション「タイマー開始」/「タイマーリセット」でタイマーを開始・リセットし、"
            L"条件「タイマー経過」で待機します (タイマーの経過時間が秒数以上になると真)。" };
        bindText(add(L"STATIC", help.get(), SS_LEFT, LX, 145, 500, 80, -1, P), help);
    }

    void App::createStepForm() {
        const int P = PAGE_STEPS, LX = 510, LW = 110, X = 625;
        label({ L"Name", L"名前" }, LX, 82, LW, P);             stName = edit(ST_NAME, X, 82, 300, P);

        label({ L"Condition", L"条件" }, LX, 108, LW, P);       stCond = combo(ST_COND, X, 108, 200, P);
        stNegate = checkbox({ L"Negate", L"否定" }, ST_NEGATE, 835, 108, 80, P);
        stDebug = checkbox({ L"Debug log", L"デバッグログ" }, ST_DEBUG, 920, 108, 100, P);

        label({ L"Points", L"認識点" }, LX, 134, LW, P);
        stEvents = add(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_MULTIPLESEL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            X, 134, 200, 76, ST_EVENTS, P, true, WS_EX_CLIENTEDGE);
        label({ L"Match", L"一致" }, 835, 134, 45, P);
        stMatch = combo(ST_MATCH, 880, 134, 80, P);
        SendMessageW(stMatch, CB_ADDSTRING, 0, (LPARAM)L"ALL");
        SendMessageW(stMatch, CB_ADDSTRING, 0, (LPARAM)L"ANY");

        label({ L"Cond. timer", L"条件タイマー" }, LX, 214, LW, P);     stCTimer = combo(ST_CTIMER, X, 214, 200, P);

        label({ L"Template image", L"テンプレート画像" }, LX, 240, LW, P);  stTPath = edit(ST_TPATH, X, 240, 215, P);
        stTBrowse = button({ L"Browse...", L"参照..." }, ST_TBROWSE, 845, 239, 75, 25, P);
        formCtrls[P].push_back(stTBrowse);
        stTCapture = button({ L"Capture (F1)", L"画面から作成 (F1)" }, ST_TCAPTURE, 925, 239, 105, 25, P);
        formCtrls[P].push_back(stTCapture);

        label({ L"Region X/Y/W/H", L"領域 X/Y/W/H" }, LX, 266, LW, P);
        stRX = edit(ST_RX, X, 266, 50, P);
        stRY = edit(ST_RY, X + 53, 266, 50, P);
        stRW = edit(ST_RW, X + 106, 266, 50, P);
        stRH = edit(ST_RH, X + 159, 266, 50, P);
        stRPick = button({ L"Drag...", L"ドラッグ指定" }, ST_RPICK, X + 214, 265, 85, 25, P);
        formCtrls[P].push_back(stRPick);
        label({ L"Threshold", L"しきい値" }, 930, 266, 52, P);
        stThresh = edit(ST_THRESH, 983, 266, 47, P);

        label({ L"Action", L"アクション" }, LX, 300, LW, P);          stAction = combo(ST_ACTION, X, 300, 200, P);
        fillTypeCombos();

        label({ L"Key / Text", L"キー / 文字列" }, LX, 326, LW, P);      stText = edit(ST_TEXT, X, 326, 200, P);
        stTextHint = label({ L"", L"" }, 835, 326, 190, P, ST_TEXTHINT);

        label({ L"X, Y", L"X, Y" }, LX, 352, LW, P);
        stAX = edit(ST_AX, X, 352, 60, P);
        stAY = edit(ST_AY, X + 65, 352, 60, P);
        stAPick = button({ L"Pick (F1)", L"取得 (F1)" }, ST_APICK, X + 135, 351, 90, 25, P);
        formCtrls[P].push_back(stAPick);

        label({ L"ms", L"ms" }, LX, 378, LW, P);              stMs = edit(ST_MS, X, 378, 60, P);
        stMsHint = label({ L"", L"" }, X + 70, 378, 300, P, ST_MSHINT);

        label({ L"Action timer", L"アクションタイマー" }, LX, 404, LW, P);    stATimer = combo(ST_ATIMER, X, 404, 200, P);

        label({ L"Next if true", L"真の場合の次" }, LX, 434, LW, P);    stNTrue = combo(ST_NTRUE, X, 434, 250, P);
        label({ L"Next if false", L"偽の場合の次" }, LX, 460, LW, P);   stNFalse = combo(ST_NFALSE, X, 460, 250, P);
        label({ L"Timeout (s)", L"タイムアウト (秒)" }, LX, 486, LW, P);     stTimeout = edit(ST_TIMEOUT, X, 486, 60, P);
        label({ L"then", L"→" }, X + 70, 486, 35, P);
        stNTimeout = combo(ST_NTIMEOUT, X + 105, 486, 250, P);
    }

    // ------------------------------------------------------------ pages / lists

    void App::showPage(int page) {
        currentPage = page;
        for (int p = 0; p < PAGE_COUNT; ++p) {
            for (HWND h : pageCtrls[p]) ShowWindow(h, p == page ? SW_SHOW : SW_HIDE);
        }
        // Steps reference events/timers by name: refresh the form when it is shown
        loadForm(page);
    }

    int App::selected(int page) const {
        return ListView_GetNextItem(list[page], -1, LVNI_SELECTED);
    }

    std::vector<std::wstring> App::rowCells(int page, int i) const {
        if (page == PAGE_EVENTS) {
            const auto& e = project.events[i];
            return { std::to_wstring(i), e.name, std::to_wstring(e.x), std::to_wstring(e.y),
                std::to_wstring(e.r) + L"," + std::to_wstring(e.g) + L"," + std::to_wstring(e.b),
                std::to_wstring(e.fuzzy), e.colorType };
        }
        if (page == PAGE_TIMERS) {
            const auto& t = project.timers[i];
            return { std::to_wstring(i), t.name, fmtDouble(t.seconds) };
        }
        const auto& s = project.steps[i];
        std::wstring cond = conditionName(s.condition);
        if (s.negate) cond = L"NOT " + cond;
        return { std::to_wstring(i), s.name, cond, actionName(s.action),
            nextLabel(s.nextOnTrue), nextLabel(s.nextOnFalse) };
    }

    void App::setRow(int page, int index, bool insert) {
        auto cells = rowCells(page, index);
        if (insert) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = index;
            item.pszText = const_cast<LPWSTR>(cells[0].c_str());
            ListView_InsertItem(list[page], &item);
        }
        for (int c = 0; c < (int)cells.size(); ++c) {
            ListView_SetItemText(list[page], index, c, const_cast<LPWSTR>(cells[c].c_str()));
        }
    }

    void App::refreshList(int page, int select) {
        int count = page == PAGE_EVENTS ? (int)project.events.size()
            : page == PAGE_TIMERS ? (int)project.timers.size() : (int)project.steps.size();
        refreshing = true;
        ListView_DeleteAllItems(list[page]);
        for (int i = 0; i < count; ++i) setRow(page, i, true);
        if (select >= count) select = count - 1;
        if (select >= 0) {
            ListView_SetItemState(list[page], select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list[page], select, FALSE);
        }
        refreshing = false;
        updateCountLabel(page);
        loadForm(page);
    }

    void App::updateCountLabel(int page) {
        int count = page == PAGE_EVENTS ? (int)project.events.size()
            : page == PAGE_TIMERS ? (int)project.timers.size() : (int)project.steps.size();
        int max = page == PAGE_EVENTS ? MAX_EVENTS : page == PAGE_TIMERS ? MAX_TIMERS : MAX_STEPS;
        SetWindowTextW(countLabel[page], (std::to_wstring(count) + L" / " + std::to_wstring(max)).c_str());
    }

    void App::fillNextCombo(HWND c, int value) {
        SendMessageW(c, CB_RESETCONTENT, 0, 0);
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)Lang::tr(L"(next step)", L"(次のステップ)"));
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)Lang::tr(L"(end)", L"(終了)"));
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)Lang::tr(L"(stay / retry)", L"(留まる / 再試行)"));
        for (int i = 0; i < (int)project.steps.size(); ++i) {
            std::wstring t = Lang::tr(L"Step ", L"ステップ ") + std::to_wstring(i) + L": " + project.steps[i].name;
            SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)t.c_str());
        }
        SendMessageW(c, CB_SETCURSEL, nextToCombo(value), 0);
    }

    void App::fillTimerCombo(HWND c, int value) {
        SendMessageW(c, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i < (int)project.timers.size(); ++i) {
            std::wstring t = std::to_wstring(i) + L": " + project.timers[i].name;
            SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)t.c_str());
        }
        SendMessageW(c, CB_SETCURSEL, value < (int)project.timers.size() ? value : -1, 0);
    }

    void App::loadForm(int page) {
        int idx = selected(page);
        loadingForm = true;
        for (HWND h : formCtrls[page]) EnableWindow(h, idx >= 0);

        if (page == PAGE_EVENTS) {
            EventConfig e = idx >= 0 ? project.events[idx] : EventConfig{};
            if (idx < 0) e.name.clear();
            SetWindowTextW(evName, e.name.c_str());
            SetWindowTextW(evX, std::to_wstring(e.x).c_str());
            SetWindowTextW(evY, std::to_wstring(e.y).c_str());
            SetWindowTextW(evR, std::to_wstring(e.r).c_str());
            SetWindowTextW(evG, std::to_wstring(e.g).c_str());
            SetWindowTextW(evB, std::to_wstring(e.b).c_str());
            SetWindowTextW(evFuzzy, std::to_wstring(e.fuzzy).c_str());
            int typeSel = 0;
            for (int i = 0; i < COLOR_TYPE_COUNT; ++i) if (e.colorType == COLOR_TYPES[i]) typeSel = i;
            SendMessageW(evType, CB_SETCURSEL, typeSel, 0);
            updatePreview();
        }
        else if (page == PAGE_TIMERS) {
            TimerConfig t = idx >= 0 ? project.timers[idx] : TimerConfig{};
            if (idx < 0) t.name.clear();
            SetWindowTextW(tmName, t.name.c_str());
            SetWindowTextW(tmSec, fmtDouble(t.seconds).c_str());
        }
        else {
            StepConfig s = idx >= 0 ? project.steps[idx] : StepConfig{};
            if (idx < 0) s.name.clear();
            SetWindowTextW(stName, s.name.c_str());
            SendMessageW(stCond, CB_SETCURSEL, (int)s.condition, 0);
            SendMessageW(stNegate, BM_SETCHECK, s.negate ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(stDebug, BM_SETCHECK, s.debug ? BST_CHECKED : BST_UNCHECKED, 0);

            SendMessageW(stEvents, LB_RESETCONTENT, 0, 0);
            for (int i = 0; i < (int)project.events.size(); ++i) {
                std::wstring t = std::to_wstring(i) + L": " + project.events[i].name;
                SendMessageW(stEvents, LB_ADDSTRING, 0, (LPARAM)t.c_str());
            }
            for (int id : s.eventIds) SendMessageW(stEvents, LB_SETSEL, TRUE, id);
            SendMessageW(stMatch, CB_SETCURSEL, s.matchAll ? 0 : 1, 0);

            fillTimerCombo(stCTimer, s.conditionTimer);
            SetWindowTextW(stTPath, s.templatePath.c_str());
            SetWindowTextW(stRX, std::to_wstring(s.regionX).c_str());
            SetWindowTextW(stRY, std::to_wstring(s.regionY).c_str());
            SetWindowTextW(stRW, std::to_wstring(s.regionW).c_str());
            SetWindowTextW(stRH, std::to_wstring(s.regionH).c_str());
            SetWindowTextW(stThresh, fmtDouble(s.threshold).c_str());

            SendMessageW(stAction, CB_SETCURSEL, (int)s.action, 0);
            SetWindowTextW(stText, s.text.c_str());
            SetWindowTextW(stAX, std::to_wstring(s.x).c_str());
            SetWindowTextW(stAY, std::to_wstring(s.y).c_str());
            SetWindowTextW(stMs, std::to_wstring(s.ms).c_str());
            fillTimerCombo(stATimer, s.actionTimer);

            fillNextCombo(stNTrue, s.nextOnTrue);
            fillNextCombo(stNFalse, s.nextOnFalse);
            SetWindowTextW(stTimeout, fmtDouble(s.timeoutSec).c_str());
            fillNextCombo(stNTimeout, s.nextOnTimeout);
            if (idx >= 0) updateStepFieldStates();
        }
        loadingForm = false;
    }

    void App::saveForm(int page) {
        int idx = selected(page);
        if (idx < 0 || loadingForm) return;

        if (page == PAGE_EVENTS) {
            auto& e = project.events[idx];
            e.name = getText(evName);
            e.x = getInt(evX);
            e.y = getInt(evY);
            e.r = getInt(evR);
            e.g = getInt(evG);
            e.b = getInt(evB);
            e.fuzzy = getInt(evFuzzy);
            int sel = (int)SendMessageW(evType, CB_GETCURSEL, 0, 0);
            e.colorType = COLOR_TYPES[sel >= 0 ? sel : 0];
            updatePreview();
        }
        else if (page == PAGE_TIMERS) {
            auto& t = project.timers[idx];
            t.name = getText(tmName);
            t.seconds = getDouble(tmSec);
        }
        else {
            auto& s = project.steps[idx];
            s.name = getText(stName);
            int c = (int)SendMessageW(stCond, CB_GETCURSEL, 0, 0);
            s.condition = (ConditionType)(c >= 0 ? c : 0);
            s.negate = SendMessageW(stNegate, BM_GETCHECK, 0, 0) == BST_CHECKED;
            s.debug = SendMessageW(stDebug, BM_GETCHECK, 0, 0) == BST_CHECKED;

            s.eventIds.clear();
            int selCount = (int)SendMessageW(stEvents, LB_GETSELCOUNT, 0, 0);
            if (selCount > 0) {
                std::vector<int> items(selCount);
                SendMessageW(stEvents, LB_GETSELITEMS, selCount, (LPARAM)items.data());
                s.eventIds = items;
            }
            s.matchAll = SendMessageW(stMatch, CB_GETCURSEL, 0, 0) != 1;

            int ct = (int)SendMessageW(stCTimer, CB_GETCURSEL, 0, 0);
            s.conditionTimer = ct >= 0 ? ct : 0;
            s.templatePath = getText(stTPath);
            s.regionX = getInt(stRX);
            s.regionY = getInt(stRY);
            s.regionW = getInt(stRW);
            s.regionH = getInt(stRH);
            s.threshold = getDouble(stThresh);

            int a = (int)SendMessageW(stAction, CB_GETCURSEL, 0, 0);
            s.action = (ActionType)(a >= 0 ? a : 0);
            s.text = getText(stText);
            s.x = getInt(stAX);
            s.y = getInt(stAY);
            s.ms = getInt(stMs);
            int at = (int)SendMessageW(stATimer, CB_GETCURSEL, 0, 0);
            s.actionTimer = at >= 0 ? at : 0;

            s.nextOnTrue = comboToNext((int)SendMessageW(stNTrue, CB_GETCURSEL, 0, 0));
            s.nextOnFalse = comboToNext((int)SendMessageW(stNFalse, CB_GETCURSEL, 0, 0));
            s.timeoutSec = getDouble(stTimeout);
            s.nextOnTimeout = comboToNext((int)SendMessageW(stNTimeout, CB_GETCURSEL, 0, 0));
            updateStepFieldStates();
        }
        setRow(page, idx, false);
        markDirty();
    }

    void App::updatePreview() {
        if (previewBrush) DeleteObject(previewBrush);
        previewBrush = CreateSolidBrush(RGB(getInt(evR) & 0xFF, getInt(evG) & 0xFF, getInt(evB) & 0xFF));
        InvalidateRect(evPreview, nullptr, TRUE);
    }

    // Enable only the fields that matter for the chosen condition / action
    void App::updateStepFieldStates() {
        auto c = (ConditionType)SendMessageW(stCond, CB_GETCURSEL, 0, 0);
        auto a = (ActionType)SendMessageW(stAction, CB_GETCURSEL, 0, 0);

        bool ev = c == ConditionType::EventColor || c == ConditionType::EventRGB;
        bool tp = c == ConditionType::Template || c == ConditionType::TemplateBinary;
        EnableWindow(stEvents, ev);
        EnableWindow(stMatch, ev);
        EnableWindow(stDebug, ev);
        EnableWindow(stCTimer, c == ConditionType::TimerElapsed);
        for (HWND h : { stTPath, stTBrowse, stTCapture, stRX, stRY, stRW, stRH, stRPick, stThresh }) EnableWindow(h, tp);

        bool text = a == ActionType::KeyTap || a == ActionType::TypeText;
        bool xy = a == ActionType::MouseMove || a == ActionType::MouseMoveRel ||
            a == ActionType::LeftClick || a == ActionType::RightClick;
        bool ms = a == ActionType::KeyTap || a == ActionType::TypeText || a == ActionType::LeftClick ||
            a == ActionType::RightClick || a == ActionType::ClickMatch || a == ActionType::Wait;
        EnableWindow(stText, text);
        EnableWindow(stAX, xy);
        EnableWindow(stAY, xy);
        EnableWindow(stAPick, xy && a != ActionType::MouseMoveRel);
        EnableWindow(stMs, ms);
        EnableWindow(stATimer, a == ActionType::TimerStart || a == ActionType::TimerReset);

        const wchar_t* textHint = a == ActionType::KeyTap ? Lang::tr(L"e.g. ENTER, CTRL+S, A, F5", L"例: ENTER, CTRL+S, A, F5")
            : a == ActionType::TypeText ? Lang::tr(L"text to type", L"入力する文字列") : L"";
        const wchar_t* msHint = L"";
        switch (a) {
        case ActionType::KeyTap:     msHint = Lang::tr(L"key hold time", L"キーを押す時間"); break;
        case ActionType::TypeText:   msHint = Lang::tr(L"interval between characters", L"文字ごとの間隔"); break;
        case ActionType::LeftClick:
        case ActionType::RightClick: msHint = Lang::tr(L"button hold time (X or Y < 0: no move)", L"ボタンを押す時間 (X/Y < 0: 移動しない)"); break;
        case ActionType::ClickMatch: msHint = Lang::tr(L"button hold time", L"ボタンを押す時間"); break;
        case ActionType::Wait:       msHint = Lang::tr(L"wait time", L"待機時間"); break;
        default: break;
        }
        SetWindowTextW(stTextHint, textHint);
        SetWindowTextW(stMsHint, msHint);
    }

    // ------------------------------------------------------------ list operations

    void App::onAdd(int page) {
        int idx = page == PAGE_EVENTS ? project.addEvent()
            : page == PAGE_TIMERS ? project.addTimer() : project.addStep();
        if (idx < 0) {
            int max = page == PAGE_EVENTS ? MAX_EVENTS : page == PAGE_TIMERS ? MAX_TIMERS : MAX_STEPS;
            std::wstring msg = Lang::current() == Lang::Language::Japanese
                ? L"上限の " + std::to_wstring(max) + L" 件に達しました。"
                : L"The limit of " + std::to_wstring(max) + L" items has been reached.";
            MessageBoxW(hwnd, msg.c_str(), L"ArgusShift", MB_ICONINFORMATION);
            return;
        }
        markDirty();
        refreshList(page, idx);
    }

    void App::onRemove(int page) {
        int idx = selected(page);
        if (idx < 0) return;
        if (page == PAGE_EVENTS) project.removeEvent(idx);
        else if (page == PAGE_TIMERS) project.removeTimer(idx);
        else project.removeStep(idx);
        markDirty();
        refreshList(page, idx);
        if (page != PAGE_STEPS) refreshList(PAGE_STEPS, selected(PAGE_STEPS)); // references changed
    }

    void App::onMove(int page, int dir) {
        int idx = selected(page);
        int other = idx + dir;
        int count = page == PAGE_EVENTS ? (int)project.events.size()
            : page == PAGE_TIMERS ? (int)project.timers.size() : (int)project.steps.size();
        if (idx < 0 || other < 0 || other >= count) return;
        if (page == PAGE_EVENTS) project.swapEvents(idx, other);
        else if (page == PAGE_TIMERS) project.swapTimers(idx, other);
        else project.swapSteps(idx, other);
        markDirty();
        refreshList(page, other);
        if (page != PAGE_STEPS) refreshList(PAGE_STEPS, selected(PAGE_STEPS));
    }

    // ------------------------------------------------------------ tools

    void App::refreshTargetList() {
        // Rebuild the dropdown items while keeping the typed/selected text
        std::wstring current = getText(target);
        loadingForm = true;
        SendMessageW(target, CB_RESETCONTENT, 0, 0);
        for (const auto& name : listWindowProcesses()) {
            SendMessageW(target, CB_ADDSTRING, 0, (LPARAM)name.c_str());
        }
        SetWindowTextW(target, current.c_str());
        int found = (int)SendMessageW(target, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)current.c_str());
        if (found >= 0) SendMessageW(target, CB_SETCURSEL, found, 0);
        loadingForm = false;
    }

    // Picking from the screen.
    // While a pick is pending global hot keys are registered:
    //   F1  : freeze the whole screen and pick on the capture (magnifier, arrow keys move 1 px).
    //         The colors are those of the moment F1 was pressed, so animated screens work too.
    //   Esc : cancel
    // Clicking the pick button again also cancels.
    void App::onPickStart(PickTarget t) {
        if (pickTarget != PickTarget::None) {
            bool same = pickTarget == t;
            endPick();
            setStatus({ L"Pick canceled", L"取得をキャンセルしました" });
            if (same) return;
        }
        if (!RegisterHotKey(hwnd, HOTKEY_CAPTURE, MOD_NOREPEAT, VK_F1)) {
            MessageBoxW(hwnd, Lang::tr(L"The F1 key could not be registered as a hot key (another application may be using it).",
                L"F1キーをホットキーとして登録できませんでした (他のアプリケーションが使用している可能性があります)。"),
                L"ArgusShift", MB_ICONWARNING);
            return;
        }
        RegisterHotKey(hwnd, HOTKEY_CANCEL, MOD_NOREPEAT, VK_ESCAPE);
        pickTarget = t;

        if (t == PickTarget::TemplateRegion) {
            setStatus({ L"Show the target, then F1: freeze & drag a region / Esc: cancel",
                L"対象を表示して F1: 静止キャプチャしてドラッグで領域指定 / Esc: キャンセル" });
        }
        else if (t == PickTarget::TemplateImage) {
            setStatus({ L"Show the target, then F1: freeze & drag the template image / Esc: cancel",
                L"対象を表示して F1: 静止キャプチャしてドラッグでテンプレート画像を切り出し / Esc: キャンセル" });
        }
        else {
            setStatus({ L"Show the target, then F1: freeze & pick on capture / Esc: cancel",
                L"対象を表示して F1: 静止キャプチャから取得 / Esc: キャンセル" });
        }
    }

    void App::endPick() {
        UnregisterHotKey(hwnd, HOTKEY_CAPTURE);
        UnregisterHotKey(hwnd, HOTKEY_CANCEL);
        pickTarget = PickTarget::None;
    }

    void App::onPickHotkey(int id) {
        PickTarget t = pickTarget;
        if (t == PickTarget::None) return;

        if (id == HOTKEY_CANCEL) {
            endPick();
            setStatus({ L"Pick canceled", L"取得をキャンセルしました" });
            return;
        }
        if (id != HOTKEY_CAPTURE) return;

        // F1: freeze the screen now, then pick on it.
        // The origin is read first so the picker shows the same coordinates that will be stored.
        endPick();
        POINT o{};
        if (!coordOrigin(o)) {
            setStatus({ L"Pick canceled", L"取得をキャンセルしました" });
            return;
        }
        ScreenPicker::Display display;
        if (project.coordMode == CoordMode::Client) display.clientOrigin = &o;

        bool ok = false;
        if (t == PickTarget::TemplateRegion) {
            RECT rc{};
            ok = ScreenPicker::pickRect(hwnd, rc, display);
            if (ok) {
                OffsetRect(&rc, -o.x, -o.y);
                applyPickedRect(rc);
            }
        }
        else if (t == PickTarget::TemplateImage) {
            RECT rc{};
            std::vector<uint32_t> pixels;
            ok = ScreenPicker::pickImage(hwnd, rc, pixels, display);
            if (ok) {
                OffsetRect(&rc, -o.x, -o.y);
                applyPickedImage(rc, pixels);
                SetForegroundWindow(hwnd);
                return;
            }
        }
        else {
            POINT pt{};
            COLORREF c = 0;
            ok = ScreenPicker::pickPoint(hwnd, pt, c, display);
            if (ok) {
                pt.x -= o.x;
                pt.y -= o.y;
                applyPickedPoint(t, pt, c);
            }
        }
        if (!ok) setStatus({ L"Pick canceled", L"取得をキャンセルしました" });
        SetForegroundWindow(hwnd);
    }

    void App::applyPickedPoint(PickTarget t, POINT pt, COLORREF c) {
        if (t == PickTarget::Event && selected(PAGE_EVENTS) >= 0) {
            SetWindowTextW(evX, std::to_wstring(pt.x).c_str());
            SetWindowTextW(evY, std::to_wstring(pt.y).c_str());
            SetWindowTextW(evR, std::to_wstring(GetRValue(c)).c_str());
            SetWindowTextW(evG, std::to_wstring(GetGValue(c)).c_str());
            SetWindowTextW(evB, std::to_wstring(GetBValue(c)).c_str());
        }
        else if (t == PickTarget::StepAction && selected(PAGE_STEPS) >= 0) {
            SetWindowTextW(stAX, std::to_wstring(pt.x).c_str());
            SetWindowTextW(stAY, std::to_wstring(pt.y).c_str());
        }
        Logger::info("Picked xy(" + std::to_string(pt.x) + "," + std::to_string(pt.y) + ") RGB(" +
            std::to_string(GetRValue(c)) + "," + std::to_string(GetGValue(c)) + "," + std::to_string(GetBValue(c)) + ")");
        setStatus({ L"Picked", L"取得しました" });
    }

    void App::applyPickedRect(const RECT& rc) {
        if (selected(PAGE_STEPS) < 0) return;
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        SetWindowTextW(stRX, std::to_wstring(rc.left).c_str());
        SetWindowTextW(stRY, std::to_wstring(rc.top).c_str());
        SetWindowTextW(stRW, std::to_wstring(w).c_str());
        SetWindowTextW(stRH, std::to_wstring(h).c_str());
        Logger::info("Picked region x" + std::to_string(rc.left) + " y" + std::to_string(rc.top) +
            " w" + std::to_string(w) + " h" + std::to_string(h));
        setStatus({ L"Region set", L"領域を設定しました" });
    }

    // Save the pixels cut out of the frozen capture as a template image and set it to the step
    void App::applyPickedImage(const RECT& rc, const std::vector<uint32_t>& pixels) {
        int w = rc.right - rc.left, h = rc.bottom - rc.top;

        wchar_t file[MAX_PATH];
        time_t now = time(nullptr);
        tm lt{};
        localtime_s(&lt, &now);
        wcsftime(file, MAX_PATH, L"template_%Y%m%d_%H%M%S.png", &lt);

        // Start in the folder of the current template, else next to the exe
        std::wstring dir = exeDirectory();
        std::wstring cur = getText(stTPath);
        if (!cur.empty()) {
            bool abs = (cur.size() >= 2 && cur[1] == L':') || cur[0] == L'\\' || cur[0] == L'/';
            std::wstring full = abs ? cur : dir + L"\\" + cur;
            size_t pos = full.find_last_of(L"\\/");
            if (pos != std::wstring::npos) {
                DWORD attr = GetFileAttributesW(full.substr(0, pos).c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) dir = full.substr(0, pos);
            }
        }

        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = Lang::tr(L"PNG image (*.png)\0*.png\0Bitmap (*.bmp)\0*.bmp\0",
            L"PNG 画像 (*.png)\0*.png\0ビットマップ (*.bmp)\0*.bmp\0");
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = dir.c_str();
        ofn.lpstrDefExt = L"png";
        ofn.lpstrTitle = Lang::tr(L"Save template image", L"テンプレート画像の保存");
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&ofn)) {
            setStatus({ L"Template capture canceled", L"テンプレート画像の作成をキャンセルしました" });
            return;
        }

        std::wstring path = file;
        if (!TemplateMatching::saveImage(path, w, h, pixels.data())) {
            MessageBoxW(hwnd, (std::wstring(Lang::tr(L"Could not save the image:\n", L"画像を保存できませんでした:\n")) + path).c_str(),
                L"ArgusShift", MB_ICONERROR);
            return;
        }
        Logger::info("Saved template " + toAnsi(path) + " (x" + std::to_string(rc.left) + " y" + std::to_string(rc.top) +
            " w" + std::to_string(w) + " h" + std::to_string(h) + ")");

        if (selected(PAGE_STEPS) >= 0) {
            std::wstring base = exeDirectory() + L"\\";
            if (path.size() > base.size() && _wcsnicmp(path.c_str(), base.c_str(), base.size()) == 0) {
                path = path.substr(base.size());  // store relative to exe directory
            }
            SetWindowTextW(stTPath, path.c_str());
        }
        setStatus({ L"Template image saved", L"テンプレート画像を保存しました" });
    }

    void App::onTestEvent() {
        int idx = selected(PAGE_EVENTS);
        if (idx < 0) return;
        const auto& e = project.events[idx];
        POINT o{};
        if (!coordOrigin(o)) return;
        Event ev(e.x + o.x, e.y + o.y, RGB(e.r, e.g, e.b), e.fuzzy, toAnsi(e.colorType));
        Logger::info("Test point " + std::to_string(idx) + " (" + toAnsi(e.name) + ")");
        ev.checkColor(true);
        ev.checkRGB(true);
    }

    void App::onBrowseTemplate() {
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = Lang::tr(L"Images (*.png;*.bmp;*.jpg)\0*.png;*.bmp;*.jpg;*.jpeg\0All files\0*.*\0",
            L"画像 (*.png;*.bmp;*.jpg)\0*.png;*.bmp;*.jpg;*.jpeg\0すべてのファイル\0*.*\0");
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) return;

        std::wstring path = file;
        std::wstring dir = exeDirectory() + L"\\";
        if (path.size() > dir.size() && _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) == 0) {
            path = path.substr(dir.size());  // store relative to exe directory
        }
        SetWindowTextW(stTPath, path.c_str());
    }

    void App::onRun() {
        std::wstring err;
        if (!runner.start(project, err)) {
            MessageBoxW(hwnd, err.c_str(), Lang::tr(L"Cannot start automation", L"自動化を開始できません"), MB_ICONWARNING);
            return;
        }
        updateRunState();
    }

    void App::onStop() {
        runner.stop();
        updateRunState();
    }

    // ------------------------------------------------------------ file handling

    bool App::confirmDiscard() {
        if (!dirty) return true;
        int r = MessageBoxW(hwnd, Lang::tr(L"Save changes to the current project?", L"現在のプロジェクトの変更を保存しますか?"), L"ArgusShift",
            MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return false;
        if (r == IDYES) return saveProjectFile(false);
        return true;
    }

    void App::newProject() {
        if (!confirmDiscard()) return;
        project = Project{};
        projectPath.clear();
        loadProjectToUi();
    }

    void App::openProject() {
        if (!confirmDiscard()) return;
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = projectFilter();
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) openProjectPath(file);
    }

    bool App::openProjectPath(const std::wstring& path) {
        Project p;
        std::wstring err;
        if (!loadProject(p, path, err)) {
            MessageBoxW(hwnd, err.c_str(), Lang::tr(L"Open failed", L"読み込みに失敗しました"), MB_ICONERROR);
            return false;
        }
        project = p;
        projectPath = path;
        loadProjectToUi();
        Logger::info("Opened " + toAnsi(path));
        return true;
    }

    bool App::saveProjectFile(bool saveAs) {
        std::wstring path = projectPath;
        if (saveAs || path.empty()) {
            wchar_t file[MAX_PATH] = L"automation.argus";
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = projectFilter();
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"argus";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
            if (!GetSaveFileNameW(&ofn)) return false;
            path = file;
        }
        std::wstring err;
        if (!saveProject(project, path, err)) {
            MessageBoxW(hwnd, err.c_str(), Lang::tr(L"Save failed", L"保存に失敗しました"), MB_ICONERROR);
            return false;
        }
        projectPath = path;
        dirty = false;
        updateTitle();
        Logger::info("Saved " + toAnsi(path));
        return true;
    }

    void App::loadProjectToUi() {
        loadingForm = true;
        SetWindowTextW(target, project.targetExe.c_str());
        SetWindowTextW(poll, std::to_wstring(project.pollMs).c_str());
        fillCoordCombo();
        SendMessageW(wFix, BM_SETCHECK, project.fixWindow ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(wX, std::to_wstring(project.winX).c_str());
        SetWindowTextW(wY, std::to_wstring(project.winY).c_str());
        SetWindowTextW(wW, std::to_wstring(project.winW).c_str());
        SetWindowTextW(wH, std::to_wstring(project.winH).c_str());
        loadingForm = false;
        for (int p = 0; p < PAGE_COUNT; ++p) refreshList(p, 0);
        dirty = false;
        updateTitle();
    }

    void App::markDirty() {
        if (!dirty) {
            dirty = true;
            updateTitle();
        }
    }

    void App::updateTitle() {
        std::wstring name = projectPath.empty() ? std::wstring(Lang::tr(L"Untitled", L"無題")) : projectPath.substr(projectPath.find_last_of(L"\\/") + 1);
        SetWindowTextW(hwnd, (L"ArgusShift - " + name + (dirty ? L" *" : L"")).c_str());
    }

    void App::setStatus(Text text, const std::wstring& suffix) {
        statusText = text;
        statusSuffix = suffix;
        SetWindowTextW(status, (std::wstring(text.get()) + suffix).c_str());
    }

    void App::updateRunState() {
        bool running = runner.isRunning();
        EnableWindow(btnRun, !running);
        EnableWindow(btnStop, running);
        if (running) {
            int step = runner.currentStep();
            std::wstring name = (step >= 0 && step < (int)project.steps.size()) ? project.steps[step].name : L"";
            setStatus({ L"Running - step ", L"実行中 - ステップ " }, std::to_wstring(step) + L": " + name);
        }
        else if (wasRunning) {
            setStatus({ L"Stopped", L"停止しました" });
        }
        wasRunning = running;
    }

    void App::appendLog(const std::wstring& line) {
        int len = GetWindowTextLengthW(logView);
        if (len > 500000) {  // keep the log view bounded
            SendMessageW(logView, EM_SETSEL, 0, len / 2);
            SendMessageW(logView, EM_REPLACESEL, FALSE, (LPARAM)L"");
            len = GetWindowTextLengthW(logView);
        }
        std::wstring text = line + L"\r\n";
        SendMessageW(logView, EM_SETSEL, len, len);
        SendMessageW(logView, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    }

    // ------------------------------------------------------------ target window placement

    void App::readWindowFields() {
        project.fixWindow = SendMessageW(wFix, BM_GETCHECK, 0, 0) == BST_CHECKED;
        project.winX = getInt(wX);
        project.winY = getInt(wY);
        project.winW = getInt(wW);
        project.winH = getInt(wH);
    }

    // Copy the target window's current position / size into the fields
    void App::onGetWindowRect() {
        project.targetExe = getText(target);
        RECT r;
        if (!GetTargetWindowRect(project.targetExe.c_str(), r)) {
            MessageBoxW(hwnd, (std::wstring(Lang::tr(L"Target window not found: ", L"対象ウィンドウが見つかりません: ")) + project.targetExe).c_str(),
                L"ArgusShift", MB_ICONWARNING);
            return;
        }
        loadingForm = true;
        SetWindowTextW(wX, std::to_wstring(r.left).c_str());
        SetWindowTextW(wY, std::to_wstring(r.top).c_str());
        SetWindowTextW(wW, std::to_wstring(r.right - r.left).c_str());
        SetWindowTextW(wH, std::to_wstring(r.bottom - r.top).c_str());
        loadingForm = false;
        readWindowFields();
        markDirty();
        setStatus({ L"Got target window placement", L"対象ウィンドウの位置とサイズを取得しました" });
    }

    // Move / resize the target window to the configured placement now
    void App::onApplyWindowRect() {
        project.targetExe = getText(target);
        readWindowFields();
        if (project.winW < 1 || project.winH < 1) {
            MessageBoxW(hwnd, Lang::tr(L"Width and height must be >= 1.", L"幅と高さは1以上にしてください。"),
                L"ArgusShift", MB_ICONWARNING);
            return;
        }
        if (!TryWindowInitRect(project.targetExe.c_str(), project.winX, project.winY, project.winW, project.winH)) {
            MessageBoxW(hwnd, (std::wstring(Lang::tr(L"Target window not found: ", L"対象ウィンドウが見つかりません: ")) + project.targetExe).c_str(),
                L"ArgusShift", MB_ICONWARNING);
            return;
        }
        setStatus({ L"Target window placed", L"対象ウィンドウを配置しました" });
    }

    // ------------------------------------------------------------ coordinate mode

    // Combo order: 0 = client, 1 = screen. Rebuilt on language change, selection follows the project.
    void App::fillCoordCombo() {
        bool wasLoading = loadingForm;
        loadingForm = true;
        SendMessageW(coordCombo, CB_RESETCONTENT, 0, 0);
        SendMessageW(coordCombo, CB_ADDSTRING, 0, (LPARAM)Lang::tr(L"Client", L"クライアント"));
        SendMessageW(coordCombo, CB_ADDSTRING, 0, (LPARAM)Lang::tr(L"Screen", L"画面"));
        SendMessageW(coordCombo, CB_SETCURSEL, project.coordMode == CoordMode::Client ? 0 : 1, 0);
        loadingForm = wasLoading;
    }

    // Origin to add to stored coordinates to get screen coordinates: (0, 0) in screen mode,
    // the target window's client top-left in client mode. Shows a message and returns false
    // when the target window is not available.
    bool App::coordOrigin(POINT& origin) {
        origin = { 0, 0 };
        if (project.coordMode != CoordMode::Client) return true;
        project.targetExe = getText(target);
        if (GetTargetClientOrigin(project.targetExe.c_str(), origin)) return true;
        MessageBoxW(hwnd, (std::wstring(Lang::tr(L"Target window not found or minimized: ",
            L"対象ウィンドウが見つからないか最小化されています: ")) + project.targetExe).c_str(),
            L"ArgusShift", MB_ICONWARNING);
        return false;
    }

    void App::onCoordModeChange() {
        int sel = (int)SendMessageW(coordCombo, CB_GETCURSEL, 0, 0);
        CoordMode mode = sel == 1 ? CoordMode::Screen : CoordMode::Client;
        if (mode == project.coordMode) return;

        if (!project.events.empty() || !project.steps.empty()) {
            int r = MessageBoxW(hwnd, Lang::tr(
                L"Convert the existing coordinates using the target window's current position?\n\n"
                L"Yes: convert (the same screen positions are kept)\nNo: keep the numbers as they are",
                L"対象ウィンドウの現在位置を使って、既存の座標を変換しますか?\n\n"
                L"はい: 変換する (画面上の同じ位置を指したまま)\nいいえ: 数値はそのまま"),
                L"ArgusShift", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) { fillCoordCombo(); return; }
            if (r == IDYES) {
                project.targetExe = getText(target);
                POINT o{};
                if (!GetTargetClientOrigin(project.targetExe.c_str(), o)) {
                    MessageBoxW(hwnd, (std::wstring(Lang::tr(L"Target window not found or minimized: ",
                        L"対象ウィンドウが見つからないか最小化されています: ")) + project.targetExe).c_str(),
                        L"ArgusShift", MB_ICONWARNING);
                    fillCoordCombo();
                    return;
                }
                // screen -> client subtracts the origin, client -> screen adds it
                int sign = mode == CoordMode::Client ? -1 : 1;
                project.offsetCoordinates(sign * o.x, sign * o.y);
                Logger::info("Converted coordinates by (" + std::to_string(sign * o.x) + ", " + std::to_string(sign * o.y) + ")");
            }
        }
        project.coordMode = mode;
        markDirty();
        for (int p = 0; p < PAGE_COUNT; ++p) refreshList(p, selected(p));
        setStatus(mode == CoordMode::Client
            ? Text{ L"Coordinates: client", L"座標: クライアント" }
            : Text{ L"Coordinates: screen", L"座標: 画面" });
    }

    // ------------------------------------------------------------ window procedure

    LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            app = reinterpret_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            app->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app ? app->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            createControls();
            captureLayout();
            return 0;

        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) layoutControls(LOWORD(lp), HIWORD(lp));
            return 0;

        case WM_GETMINMAXINFO: {
            // Never smaller than the designed size
            RECT rc{ 0, 0, S(BASE_W), S(BASE_H) };
            AdjustWindowRect(&rc, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE);
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = rc.right - rc.left;
            mmi->ptMinTrackSize.y = rc.bottom - rc.top;
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            switch (id) {
            case ID_NEW:    newProject(); return 0;
            case ID_OPEN:   openProject(); return 0;
            case ID_SAVE:   saveProjectFile(false); return 0;
            case ID_SAVEAS: saveProjectFile(true); return 0;
            case ID_RUN:    onRun(); return 0;
            case ID_STOP:   onStop(); return 0;
            case EV_PICK:   onPickStart(PickTarget::Event); return 0;
            case ST_APICK:  onPickStart(PickTarget::StepAction); return 0;
            case ST_RPICK:  onPickStart(PickTarget::TemplateRegion); return 0;
            case ST_TCAPTURE: onPickStart(PickTarget::TemplateImage); return 0;
            case EV_TEST:   onTestEvent(); return 0;
            case ST_TBROWSE: onBrowseTemplate(); return 0;
            case ID_LANG:
                if (code == CBN_SELCHANGE) {
                    int sel = (int)SendMessageW(langCombo, CB_GETCURSEL, 0, 0);
                    if (sel >= 0 && sel != (int)Lang::current()) {
                        Lang::set((Lang::Language)sel);
                        Lang::save((Lang::Language)sel);
                        applyLanguage();
                    }
                }
                return 0;
            case ID_TARGET:
                if (code == CBN_DROPDOWN) {
                    refreshTargetList();
                }
                else if (code == CBN_SELCHANGE && !loadingForm) {
                    // The edit text is not updated yet during CBN_SELCHANGE, so read the item
                    int sel = (int)SendMessageW(target, CB_GETCURSEL, 0, 0);
                    if (sel >= 0) {
                        int len = (int)SendMessageW(target, CB_GETLBTEXTLEN, sel, 0);
                        std::wstring item(len > 0 ? len : 0, L'\0');
                        if (len > 0) SendMessageW(target, CB_GETLBTEXT, sel, (LPARAM)&item[0]);
                        project.targetExe = item;
                        markDirty();
                    }
                }
                else if (code == CBN_EDITCHANGE && !loadingForm) {
                    project.targetExe = getText(target);
                    markDirty();
                }
                return 0;
            case ID_WFIX:
                if (code == BN_CLICKED && !loadingForm) { readWindowFields(); markDirty(); }
                return 0;
            case ID_WX: case ID_WY: case ID_WW: case ID_WH:
                if (code == EN_CHANGE && !loadingForm) { readWindowFields(); markDirty(); }
                return 0;
            case ID_COORD:
                if (code == CBN_SELCHANGE && !loadingForm) onCoordModeChange();
                return 0;
            case ID_WGET:   onGetWindowRect(); return 0;
            case ID_WAPPLY: onApplyWindowRect(); return 0;
            case ID_POLL:
                if (code == EN_CHANGE && !loadingForm) {
                    project.targetExe = getText(target);
                    project.pollMs = getInt(poll);
                    markDirty();
                }
                return 0;
            default: break;
            }
            for (int p = 0; p < PAGE_COUNT; ++p) {
                if (id == ID_ADD + p)    { onAdd(p); return 0; }
                if (id == ID_REMOVE + p) { onRemove(p); return 0; }
                if (id == ID_UP + p)     { onMove(p, -1); return 0; }
                if (id == ID_DOWN + p)   { onMove(p, +1); return 0; }
            }
            bool change = code == EN_CHANGE || code == CBN_SELCHANGE || code == BN_CLICKED || code == LBN_SELCHANGE;
            if (change && !loadingForm) {
                if (id >= EV_NAME && id < EV_PREVIEW) saveForm(PAGE_EVENTS);
                else if (id >= TM_NAME && id < 500) saveForm(PAGE_TIMERS);
                else if (id >= ST_NAME && id < ST_TBROWSE) saveForm(PAGE_STEPS);
            }
            return 0;
        }

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->idFrom == ID_TAB && hdr->code == TCN_SELCHANGE) {
                showPage(TabCtrl_GetCurSel(tab));
                return 0;
            }
            for (int p = 0; p < PAGE_COUNT; ++p) {
                if (hdr->hwndFrom == list[p] && hdr->code == LVN_ITEMCHANGED && !refreshing) {
                    auto* nm = reinterpret_cast<NMLISTVIEW*>(lp);
                    if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_SELECTED)) {
                        loadForm(p);
                    }
                    return 0;
                }
            }
            break;
        }

        case WM_CTLCOLORSTATIC:
            if ((HWND)lp == evPreview && previewBrush) return (LRESULT)previewBrush;
            break;

        case WM_TIMER:
            if (wp == TIMER_STATUS) updateRunState();
            return 0;

        case WM_HOTKEY:
            onPickHotkey((int)wp);
            return 0;

        case WM_APP_LOG: {
            auto* line = reinterpret_cast<std::wstring*>(lp);
            appendLog(*line);
            delete line;
            return 0;
        }

        case WM_CLOSE:
            if (runner.isRunning()) runner.stop();
            if (confirmDiscard()) DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_STATUS);
            endPick();
            runner.stop();
            Logger::setSink(nullptr);
            if (previewBrush) DeleteObject(previewBrush);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    int App::run(const wchar_t* initialProject) {
        // Use physical pixels so picked coordinates match Event / Emulate coordinates
        SetProcessDPIAware();
        Lang::set(Lang::loadSaved());

        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);

        HDC screen = GetDC(nullptr);
        dpi = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(nullptr, screen);

        NONCLIENTMETRICSW ncm{ sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        font = CreateFontIndirectW(&ncm.lfMessageFont);

        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ArgusShiftMainWindow";
        RegisterClassExW(&wc);

        DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        RECT rc{ 0, 0, S(BASE_W), S(BASE_H) };
        AdjustWindowRect(&rc, style, FALSE);
        CreateWindowExW(0, wc.lpszClassName, L"ArgusShift", style, CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, this);
        if (!hwnd) return 1;

        // Forward Logger output (from any thread) to the log view
        HWND target_ = hwnd;
        Logger::setSink([target_](const std::string& line) {
            auto* w = new std::wstring(fromAnsi(line));
            if (!PostMessageW(target_, WM_APP_LOG, 0, (LPARAM)w)) delete w;
        });

        if (!(initialProject && *initialProject && openProjectPath(initialProject))) {
            project.targetExe = L"notepad.exe";
            loadProjectToUi();
        }
        showPage(PAGE_EVENTS);
        updateRunState();
        setStatus({ L"Idle", L"待機中" });
        SetTimer(hwnd, TIMER_STATUS, 200, nullptr);

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        int exitCode = (int)m.wParam;
        // Drop log lines that were posted after the window closed
        while (PeekMessageW(&m, nullptr, WM_APP_LOG, WM_APP_LOG, PM_REMOVE)) {
            delete reinterpret_cast<std::wstring*>(m.lParam);
        }
        DeleteObject(font);
        return exitCode;
    }
}

int RunGui(const wchar_t* initialProject) {
    App app;
    return app.run(initialProject);
}
