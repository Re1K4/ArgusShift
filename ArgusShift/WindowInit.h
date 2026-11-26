#pragma once
#include "Logger.h"
#include "SharedVariable.h"
#include <string>
#include <windows.h>
#include <cwchar> 

void WindowInit(const char* app_name);

// Same as WindowInit but returns false instead of exiting when the window is not found
bool TryWindowInit(const char* app_name);

// Top-level window of the process (e.g. L"notepad.exe"), preferring its visible main window.
// Returns nullptr when not found.
HWND FindTargetWindow(const wchar_t* targetExe);

// Bring the target window to the foreground and move/resize it to (x, y, w, h) in screen pixels.
// Returns false when the window is not found.
bool TryWindowInitRect(const wchar_t* targetExe, int x, int y, int w, int h);

// Current screen rectangle of the target window. Returns false when the window is not found.
bool GetTargetWindowRect(const wchar_t* targetExe, RECT& rect);

// Screen position of the top-left of the window's client area.
// Returns false when the window no longer exists or is minimized.
bool GetClientOrigin(HWND hwnd, POINT& origin);

// Same for the target process window. Returns false when it is not found or minimized.
bool GetTargetClientOrigin(const wchar_t* targetExe, POINT& origin);

// Restore (if minimized) and bring the target window to the foreground without moving it.
// Returns false when the window is not found.
bool TryActivateWindow(const wchar_t* targetExe);
