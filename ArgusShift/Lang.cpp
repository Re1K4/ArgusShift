#include "Lang.h"

#include <windows.h>
#include <string>
#include <atomic>

namespace Lang {

    namespace {
        std::atomic<int> g_language{ (int)Language::English };

        std::wstring iniPath() {
            wchar_t buf[MAX_PATH];
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            std::wstring p(buf);
            size_t pos = p.find_last_of(L"\\/");
            return (pos == std::wstring::npos ? L"" : p.substr(0, pos + 1)) + L"ArgusShift.ini";
        }
    }

    Language current() {
        return (Language)g_language.load();
    }

    void set(Language language) {
        g_language = (int)language;
    }

    Language loadSaved() {
        wchar_t value[16] = L"";
        GetPrivateProfileStringW(L"Settings", L"Language", L"", value, 16, iniPath().c_str());
        if (_wcsicmp(value, L"ja") == 0) return Language::Japanese;
        if (_wcsicmp(value, L"en") == 0) return Language::English;
        return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE ? Language::Japanese : Language::English;
    }

    void save(Language language) {
        WritePrivateProfileStringW(L"Settings", L"Language",
            language == Language::Japanese ? L"ja" : L"en", iniPath().c_str());
    }

    const wchar_t* displayName(Language language) {
        return language == Language::Japanese ? L"日本語" : L"English";
    }
}
