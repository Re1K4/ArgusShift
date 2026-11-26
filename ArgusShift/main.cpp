#include "Gui.h"
#include "Logger.h"

// Usage:
//   ArgusShift.exe                     -> open the automation editor (GUI)
//   ArgusShift.exe <file.argus>        -> open the editor with a project
int wmain(int argc, wchar_t** argv) {
    Logger::initialize("", false);  // no console in GUI mode: log goes to the log view
    return RunGui(argc > 1 ? argv[1] : nullptr);
}
