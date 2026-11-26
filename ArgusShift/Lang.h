#pragma once

// UI language selection (English / Japanese).
// Strings are written at the call site as pairs: Lang::tr(L"Save", L"<Japanese>").
// Files that contain Japanese literals must be saved as UTF-8 with BOM.
namespace Lang {

    enum class Language : int { English = 0, Japanese = 1, Count };

    Language current();
    void set(Language language);

    // Language stored in ArgusShift.ini next to the exe.
    // Falls back to the Windows UI language when nothing is stored.
    Language loadSaved();
    void save(Language language);

    // Name of a language in its own language (for the selector)
    const wchar_t* displayName(Language language);

    inline const wchar_t* tr(const wchar_t* en, const wchar_t* ja) {
        return current() == Language::Japanese ? ja : en;
    }
}
