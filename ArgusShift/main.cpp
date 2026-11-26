#include "Gui.h"
#include "Logger.h"
#include "Sample.h"
#include <cstdio>
#include <cwchar>
#include <windows.h>

// Usage:
//   ArgusShift.exe                     -> open the automation editor (GUI)
//   ArgusShift.exe <file.argus>        -> open the editor with a project
//   ArgusShift.exe --sample            -> run the original console sample (opens a console)
int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && wcscmp(argv[1], L"--sample") == 0) {
        // The exe is a GUI (Windows subsystem) app: give the console sample a console.
        // Reuse the launching console when started from cmd / PowerShell, else open a new one.
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONIN$", "r", stdin);
        return RunSample();
    }

    Logger::initialize("", false);  // no console in GUI mode: log goes to the log view
    return RunGui(argc > 1 ? argv[1] : nullptr);
}
