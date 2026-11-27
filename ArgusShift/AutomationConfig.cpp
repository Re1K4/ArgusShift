#include "AutomationConfig.h"
#include "Lang.h"
#include "Event.h"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cwctype>

namespace Automation {

    const wchar_t* const COLOR_TYPES[COLOR_TYPE_COUNT] = {
        L"RED", L"ORANGE", L"YELLOW", L"GREEN", L"BLUE", L"PURPLE"
    };

    const wchar_t* conditionName(ConditionType type) {
        switch (type) {
        case ConditionType::None:           return Lang::tr(L"Always", L"常に");
        case ConditionType::EventColor:     return Lang::tr(L"Point color (RGB match)", L"認識点の色 (RGB一致)");
        case ConditionType::EventRGB:       return Lang::tr(L"Point color family", L"認識点の色系統");
        case ConditionType::TimerElapsed:   return Lang::tr(L"Timer elapsed", L"タイマー経過");
        case ConditionType::Template:       return Lang::tr(L"Template match", L"テンプレート一致");
        case ConditionType::TemplateBinary: return Lang::tr(L"Template (binary)", L"テンプレート(二値)");
        default:                            return L"?";
        }
    }

    const wchar_t* actionName(ActionType type) {
        switch (type) {
        case ActionType::None:         return Lang::tr(L"None", L"なし");
        case ActionType::KeyTap:       return Lang::tr(L"Key tap", L"キー入力");
        case ActionType::TypeText:     return Lang::tr(L"Type text", L"文字列入力");
        case ActionType::MouseMove:    return Lang::tr(L"Mouse move", L"マウス移動");
        case ActionType::MouseMoveRel: return Lang::tr(L"Mouse move (rel)", L"マウス移動(相対)");
        case ActionType::LeftClick:    return Lang::tr(L"Left click", L"左クリック");
        case ActionType::RightClick:   return Lang::tr(L"Right click", L"右クリック");
        case ActionType::ClickMatch:   return Lang::tr(L"Click match", L"一致位置をクリック");
        case ActionType::Wait:         return Lang::tr(L"Wait", L"待機");
        case ActionType::TimerStart:   return Lang::tr(L"Timer start", L"タイマー開始");
        case ActionType::TimerReset:   return Lang::tr(L"Timer reset", L"タイマーリセット");
        default:                       return L"?";
        }
    }

    // ---------------------------------------------------------------- Project

    int Project::addEvent() {
        if ((int)events.size() >= MAX_EVENTS) return -1;
        EventConfig e;
        e.name = L"Point" + std::to_wstring(events.size());
        events.push_back(e);
        return (int)events.size() - 1;
    }

    int Project::addTimer() {
        if ((int)timers.size() >= MAX_TIMERS) return -1;
        TimerConfig t;
        t.name = L"Timer" + std::to_wstring(timers.size());
        timers.push_back(t);
        return (int)timers.size() - 1;
    }

    int Project::addStep() {
        if ((int)steps.size() >= MAX_STEPS) return -1;
        StepConfig s;
        s.name = L"Step" + std::to_wstring(steps.size());
        steps.push_back(s);
        return (int)steps.size() - 1;
    }

    // Fix an index reference after the element at `removed` was erased
    static int shiftRef(int ref, int removed, int fallback) {
        if (ref == removed) return fallback;
        if (ref > removed) return ref - 1;
        return ref;
    }

    static int swapRef(int ref, int a, int b) {
        if (ref == a) return b;
        if (ref == b) return a;
        return ref;
    }

    void Project::removeEvent(int index) {
        if (index < 0 || index >= (int)events.size()) return;
        events.erase(events.begin() + index);
        for (auto& s : steps) {
            std::vector<int> ids;
            for (int id : s.eventIds) {
                if (id == index) continue;
                ids.push_back(id > index ? id - 1 : id);
            }
            s.eventIds = ids;
        }
    }

    void Project::removeTimer(int index) {
        if (index < 0 || index >= (int)timers.size()) return;
        timers.erase(timers.begin() + index);
        for (auto& s : steps) {
            s.conditionTimer = (std::max)(0, shiftRef(s.conditionTimer, index, 0));
            s.actionTimer = (std::max)(0, shiftRef(s.actionTimer, index, 0));
        }
    }

    void Project::removeStep(int index) {
        if (index < 0 || index >= (int)steps.size()) return;
        steps.erase(steps.begin() + index);
        for (auto& s : steps) {
            if (s.nextOnTrue >= 0)    s.nextOnTrue = shiftRef(s.nextOnTrue, index, NEXT_END);
            if (s.nextOnFalse >= 0)   s.nextOnFalse = shiftRef(s.nextOnFalse, index, NEXT_END);
            if (s.nextOnTimeout >= 0) s.nextOnTimeout = shiftRef(s.nextOnTimeout, index, NEXT_END);
        }
    }

    void Project::swapEvents(int a, int b) {
        if (a < 0 || b < 0 || a >= (int)events.size() || b >= (int)events.size()) return;
        std::swap(events[a], events[b]);
        for (auto& s : steps)
            for (int& id : s.eventIds) id = swapRef(id, a, b);
    }

    void Project::swapTimers(int a, int b) {
        if (a < 0 || b < 0 || a >= (int)timers.size() || b >= (int)timers.size()) return;
        std::swap(timers[a], timers[b]);
        for (auto& s : steps) {
            s.conditionTimer = swapRef(s.conditionTimer, a, b);
            s.actionTimer = swapRef(s.actionTimer, a, b);
        }
    }

    void Project::swapSteps(int a, int b) {
        if (a < 0 || b < 0 || a >= (int)steps.size() || b >= (int)steps.size()) return;
        std::swap(steps[a], steps[b]);
        for (auto& s : steps) {
            s.nextOnTrue = swapRef(s.nextOnTrue, a, b);
            s.nextOnFalse = swapRef(s.nextOnFalse, a, b);
            s.nextOnTimeout = swapRef(s.nextOnTimeout, a, b);
        }
    }

    void Project::offsetCoordinates(int dx, int dy) {
        for (auto& e : events) { e.x += dx; e.y += dy; }
        for (auto& s : steps) {
            s.regionX += dx;
            s.regionY += dy;
            bool click = s.action == ActionType::LeftClick || s.action == ActionType::RightClick;
            bool absMove = s.action == ActionType::MouseMove || (click && s.x >= 0 && s.y >= 0);
            if (absMove) { s.x += dx; s.y += dy; }
        }
    }

    // ---------------------------------------------------------------- Strings

    std::string toUtf8(const std::wstring& s) {
        if (s.empty()) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string out(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len, nullptr, nullptr);
        return out;
    }

    std::wstring fromUtf8(const std::string& s) {
        if (s.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring out(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
        return out;
    }

    std::string toAnsi(const std::wstring& s) {
        if (s.empty()) return {};
        int len = WideCharToMultiByte(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string out(len, '\0');
        WideCharToMultiByte(CP_ACP, 0, s.c_str(), (int)s.size(), out.data(), len, nullptr, nullptr);
        return out;
    }

    std::wstring fromAnsi(const std::string& s) {
        if (s.empty()) return {};
        int len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring out(len, L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), out.data(), len);
        return out;
    }

    // ---------------------------------------------------------------- Keys

    static std::wstring upper(std::wstring s) {
        for (auto& c : s) c = (wchar_t)std::towupper(c);
        return s;
    }

    static bool keyNameToVk(const std::wstring& rawName, unsigned short& vk) {
        std::wstring name = upper(rawName);
        struct Entry { const wchar_t* name; unsigned short vk; };
        static const Entry table[] = {
            {L"CTRL", VK_CONTROL}, {L"CONTROL", VK_CONTROL}, {L"SHIFT", VK_SHIFT},
            {L"ALT", VK_MENU}, {L"WIN", VK_LWIN},
            {L"ENTER", VK_RETURN}, {L"RETURN", VK_RETURN}, {L"TAB", VK_TAB},
            {L"ESC", VK_ESCAPE}, {L"ESCAPE", VK_ESCAPE}, {L"SPACE", VK_SPACE},
            {L"BACKSPACE", VK_BACK}, {L"BS", VK_BACK}, {L"DELETE", VK_DELETE}, {L"DEL", VK_DELETE},
            {L"INSERT", VK_INSERT}, {L"INS", VK_INSERT}, {L"HOME", VK_HOME}, {L"END", VK_END},
            {L"PAGEUP", VK_PRIOR}, {L"PGUP", VK_PRIOR}, {L"PAGEDOWN", VK_NEXT}, {L"PGDN", VK_NEXT},
            {L"UP", VK_UP}, {L"DOWN", VK_DOWN}, {L"LEFT", VK_LEFT}, {L"RIGHT", VK_RIGHT},
        };
        for (const auto& e : table) {
            if (name == e.name) { vk = e.vk; return true; }
        }
        if (name.size() >= 2 && name[0] == L'F') {
            int n = _wtoi(name.c_str() + 1);
            if (n >= 1 && n <= 24 && name.find_first_not_of(L"0123456789", 1) == std::wstring::npos) {
                vk = (unsigned short)(VK_F1 + n - 1);
                return true;
            }
        }
        if (name.size() == 1) {
            wchar_t c = name[0];
            if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) { vk = c; return true; }
            SHORT r = VkKeyScanW(rawName[0]);
            if (r != -1) { vk = (unsigned short)(r & 0xFF); return true; }
        }
        return false;
    }

    bool parseKeyCombo(const std::wstring& text, std::vector<unsigned short>& keys) {
        keys.clear();
        std::wstring token;
        std::wstring src = text;
        // A lone "+" key: "CTRL++" -> handled as last token "+"
        size_t start = 0;
        while (start <= src.size()) {
            size_t pos = src.find(L'+', start);
            if (pos == start && pos + 1 == src.size()) pos = std::wstring::npos; // trailing '+' key
            token = src.substr(start, pos == std::wstring::npos ? std::wstring::npos : pos - start);
            // trim
            size_t b = token.find_first_not_of(L" \t");
            size_t e = token.find_last_not_of(L" \t");
            token = (b == std::wstring::npos) ? L"" : token.substr(b, e - b + 1);
            if (token.empty()) return false;
            unsigned short vk = 0;
            if (!keyNameToVk(token, vk)) return false;
            keys.push_back(vk);
            if (pos == std::wstring::npos) break;
            start = pos + 1;
        }
        return !keys.empty();
    }

    // ---------------------------------------------------------------- Validation

    bool isAchromatic(const EventConfig& e) {
        return Event::classifyColor(e.r & 0xFF, e.g & 0xFF, e.b & 0xFF) == nullptr;
    }

    std::wstring validateProject(const Project& p) {
        std::wostringstream err;
        int nSteps = (int)p.steps.size();
        int nEvents = (int)p.events.size();
        int nTimers = (int)p.timers.size();
        if (nSteps == 0) return Lang::tr(L"No steps are defined.", L"ステップが定義されていません。");
        if (p.pollMs < 1) err << Lang::tr(L"Poll interval must be >= 1 ms.\n", L"ポーリング間隔は1ms以上にしてください。\n");
        if (p.coordMode == CoordMode::Client && p.targetExe.empty())
            err << Lang::tr(L"Client coordinates require a target exe.\n", L"クライアント座標には対象exeの指定が必要です。\n");
        if (p.fixWindow) {
            if (p.targetExe.empty())
                err << Lang::tr(L"Fix window requires a target exe.\n", L"ウィンドウ固定には対象exeの指定が必要です。\n");
            if (p.winW < 1 || p.winH < 1)
                err << Lang::tr(L"Fixed window width/height must be >= 1.\n", L"固定ウィンドウの幅・高さは1以上にしてください。\n");
        }

        auto stepPrefix = [&](int i) -> std::wostream& {
            return err << Lang::tr(L"Step ", L"ステップ ") << i << L": ";
        };
        auto checkNext = [&](int i, int next, const wchar_t* what) {
            if (next == NEXT_END || next == NEXT_STAY || next == NEXT_NEXT) return;
            if (next < 0 || next >= nSteps)
                stepPrefix(i) << Lang::tr(L"invalid '", L"「") << what
                    << Lang::tr(L"' target (", L"」の遷移先が不正です (") << next << L").\n";
        };

        for (int i = 0; i < nSteps; ++i) {
            const StepConfig& s = p.steps[i];
            switch (s.condition) {
            case ConditionType::EventColor:
            case ConditionType::EventRGB:
                if (s.eventIds.empty()) stepPrefix(i) << Lang::tr(L"select at least one point.\n", L"認識点を1つ以上選択してください。\n");
                for (int id : s.eventIds) {
                    if (id < 0 || id >= nEvents) stepPrefix(i) << Lang::tr(L"invalid point index ", L"不正な認識点番号 ") << id << L".\n";
                    else if (s.condition == ConditionType::EventRGB && isAchromatic(p.events[id]))
                        stepPrefix(i) << Lang::tr(L"point ", L"認識点 ") << id << L" (" << p.events[id].name
                            << Lang::tr(L") is achromatic and never matches 'Point color family'.\n",
                                        L") は無彩色のため「認識点の色系統」では判定できません。\n");
                }
                break;
            case ConditionType::TimerElapsed:
                if (s.conditionTimer < 0 || s.conditionTimer >= nTimers)
                    stepPrefix(i) << Lang::tr(L"invalid timer for condition.\n", L"条件のタイマーが不正です。\n");
                break;
            case ConditionType::Template:
            case ConditionType::TemplateBinary:
                if (s.templatePath.empty()) stepPrefix(i) << Lang::tr(L"template image path is empty.\n", L"テンプレート画像のパスが空です。\n");
                if (s.regionW <= 0 || s.regionH <= 0) stepPrefix(i) << Lang::tr(L"region size must be positive.\n", L"領域のサイズは正の値にしてください。\n");
                break;
            default:
                break;
            }
            switch (s.action) {
            case ActionType::KeyTap: {
                std::vector<unsigned short> keys;
                if (!parseKeyCombo(s.text, keys)) stepPrefix(i) << Lang::tr(L"invalid key '", L"不正なキー「") << s.text << Lang::tr(L"'.\n", L"」\n");
                break;
            }
            case ActionType::TypeText:
                if (s.text.empty()) stepPrefix(i) << Lang::tr(L"text to type is empty.\n", L"入力する文字列が空です。\n");
                break;
            case ActionType::TimerStart:
            case ActionType::TimerReset:
                if (s.actionTimer < 0 || s.actionTimer >= nTimers)
                    stepPrefix(i) << Lang::tr(L"invalid timer for action.\n", L"アクションのタイマーが不正です。\n");
                break;
            default:
                break;
            }
            checkNext(i, s.nextOnTrue, Lang::tr(L"next if true", L"真の場合の次"));
            checkNext(i, s.nextOnFalse, Lang::tr(L"next if false", L"偽の場合の次"));
            if (s.timeoutSec > 0) checkNext(i, s.nextOnTimeout, Lang::tr(L"next on timeout", L"タイムアウト時の次"));
        }
        return err.str();
    }

    // ---------------------------------------------------------------- Save / Load
    // Text format (UTF-8), one record per line, fields separated by TAB.

    static const char* FILE_MAGIC = "ARGUSSHIFT\t1";
    static const char* LEGACY_FILE_MAGIC = "CPPAUTOMATION\t1"; // files saved before the rename

    static std::string esc(const std::wstring& ws) {
        std::string s = toUtf8(ws), out;
        for (char c : s) {
            switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
            }
        }
        return out;
    }

    static std::wstring unesc(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char n = s[++i];
                out += (n == 't') ? '\t' : (n == 'n') ? '\n' : (n == 'r') ? '\r' : n;
            }
            else {
                out += s[i];
            }
        }
        return fromUtf8(out);
    }

    static std::string joinIds(const std::vector<int>& ids) {
        std::string out;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i) out += ',';
            out += std::to_string(ids[i]);
        }
        return out;
    }

    static std::vector<int> splitIds(const std::string& s) {
        std::vector<int> ids;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (!item.empty()) ids.push_back(std::atoi(item.c_str()));
        }
        return ids;
    }

    bool saveProject(const Project& p, const std::wstring& path, std::wstring& error) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) { error = Lang::tr(L"Cannot open file for writing: ", L"書き込み用にファイルを開けません: ") + path; return false; }
        f.imbue(std::locale::classic());
        const char T = '\t';
        f << FILE_MAGIC << "\n";
        f << "TARGET" << T << esc(p.targetExe) << "\n";
        f << "POLL" << T << p.pollMs << "\n";
        f << "COORD" << T << (int)p.coordMode << "\n";
        f << "WINDOW" << T << (p.fixWindow ? 1 : 0) << T << p.winX << T << p.winY << T << p.winW << T << p.winH << "\n";
        for (const auto& e : p.events) {
            f << "EVENT" << T << esc(e.name) << T << e.x << T << e.y << T << e.r << T << e.g << T << e.b
                << T << e.fuzzy << T << esc(e.colorType) << "\n";
        }
        for (const auto& t : p.timers) {
            f << "TIMER" << T << esc(t.name) << T << t.seconds << "\n";
        }
        for (const auto& s : p.steps) {
            f << "STEP" << T << esc(s.name)
                << T << (int)s.condition << T << joinIds(s.eventIds) << T << (s.matchAll ? 1 : 0)
                << T << s.conditionTimer << T << esc(s.templatePath)
                << T << s.regionX << T << s.regionY << T << s.regionW << T << s.regionH
                << T << s.threshold << T << (s.negate ? 1 : 0) << T << (s.debug ? 1 : 0)
                << T << (int)s.action << T << esc(s.text) << T << s.x << T << s.y << T << s.ms << T << s.actionTimer
                << T << s.nextOnTrue << T << s.nextOnFalse << T << s.timeoutSec << T << s.nextOnTimeout << "\n";
        }
        if (!f) { error = Lang::tr(L"Failed to write file: ", L"ファイルの書き込みに失敗しました: ") + path; return false; }
        return true;
    }

    bool loadProject(Project& out, const std::wstring& path, std::wstring& error) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { error = Lang::tr(L"Cannot open file: ", L"ファイルを開けません: ") + path; return false; }

        std::string line;
        if (!std::getline(f, line)) { error = Lang::tr(L"Empty file.", L"ファイルが空です。"); return false; }
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = line.substr(3); // BOM
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line != FILE_MAGIC && line != LEGACY_FILE_MAGIC) { error = Lang::tr(L"Not an ArgusShift project file.", L"ArgusShiftのプロジェクトファイルではありません。"); return false; }

        Project p;
        p.coordMode = CoordMode::Screen;  // files without a COORD record used screen coordinates
        int lineNo = 1;
        while (std::getline(f, line)) {
            ++lineNo;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            std::vector<std::string> v;
            size_t start = 0;
            while (true) {
                size_t pos = line.find('\t', start);
                v.push_back(line.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
                if (pos == std::string::npos) break;
                start = pos + 1;
            }
            auto I = [&](size_t i) { return i < v.size() ? std::atoi(v[i].c_str()) : 0; };
            auto D = [&](size_t i) { return i < v.size() ? std::atof(v[i].c_str()) : 0.0; };
            auto S = [&](size_t i) { return i < v.size() ? unesc(v[i]) : std::wstring(); };
            const std::string& key = v[0];

            if (key == "TARGET") {
                p.targetExe = S(1);
            }
            else if (key == "POLL") {
                p.pollMs = I(1);
            }
            else if (key == "COORD") {
                p.coordMode = I(1) == (int)CoordMode::Client ? CoordMode::Client : CoordMode::Screen;
            }
            else if (key == "WINDOW" && v.size() >= 6) {
                p.fixWindow = I(1) != 0;
                p.winX = I(2); p.winY = I(3); p.winW = I(4); p.winH = I(5);
            }
            else if (key == "EVENT" && v.size() >= 9) {
                if ((int)p.events.size() >= MAX_EVENTS) continue;
                EventConfig e;
                e.name = S(1); e.x = I(2); e.y = I(3); e.r = I(4); e.g = I(5); e.b = I(6);
                e.fuzzy = I(7); e.colorType = S(8);
                p.events.push_back(e);
            }
            else if (key == "TIMER" && v.size() >= 3) {
                if ((int)p.timers.size() >= MAX_TIMERS) continue;
                TimerConfig t;
                t.name = S(1); t.seconds = D(2);
                p.timers.push_back(t);
            }
            else if (key == "STEP" && v.size() >= 24) {
                if ((int)p.steps.size() >= MAX_STEPS) continue;
                StepConfig s;
                s.name = S(1);
                int c = I(2);
                s.condition = (c >= 0 && c < (int)ConditionType::Count) ? (ConditionType)c : ConditionType::None;
                s.eventIds = splitIds(v[3]);
                s.matchAll = I(4) != 0;
                s.conditionTimer = I(5);
                s.templatePath = S(6);
                s.regionX = I(7); s.regionY = I(8); s.regionW = I(9); s.regionH = I(10);
                s.threshold = D(11);
                s.negate = I(12) != 0;
                s.debug = I(13) != 0;
                int a = I(14);
                s.action = (a >= 0 && a < (int)ActionType::Count) ? (ActionType)a : ActionType::None;
                s.text = S(15);
                s.x = I(16); s.y = I(17); s.ms = I(18); s.actionTimer = I(19);
                s.nextOnTrue = I(20); s.nextOnFalse = I(21); s.timeoutSec = D(22); s.nextOnTimeout = I(23);
                p.steps.push_back(s);
            }
            else {
                error = Lang::tr(L"Unknown or malformed record at line ", L"不明または不正なレコードがあります: 行 ") + std::to_wstring(lineNo);
                return false;
            }
        }
        out = p;
        return true;
    }
}
