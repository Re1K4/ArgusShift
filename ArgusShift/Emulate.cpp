#include "Emulate.h"

static bool isExtendedKey(WORD key) {
    switch (key) {
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_INSERT: case VK_DELETE: case VK_LWIN: case VK_RWIN:
    case VK_RCONTROL: case VK_RMENU: case VK_DIVIDE: case VK_NUMLOCK:
        return true;
    default:
        return false;
    }
}

void Emulate::pressKey(WORD key) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = MapVirtualKey(key, 0);
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (isExtendedKey(key) ? KEYEVENTF_EXTENDEDKEY : 0);

    // Key press
    SendInput(1, &input, sizeof(INPUT));
}

void Emulate::releaseKey(WORD key) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = MapVirtualKey(key, 0);
    input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | (isExtendedKey(key) ? KEYEVENTF_EXTENDEDKEY : 0);

    // Key release
    SendInput(1, &input, sizeof(INPUT));
}

void Emulate::moveMouse(int x, int y) {
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dx = x - SharedVariable::window_center_x;
    input.mi.dy = y - SharedVariable::window_center_y;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;

    // Move mouse relative coordinate
    SendInput(1, &input, sizeof(INPUT));
}

void Emulate::moveMouseRel(int dx, int dy) {
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;

    // Move mouse relative coordinate
    SendInput(1, &input, sizeof(INPUT));
}

void Emulate::moveMouseAbs(int screenX, int screenY) {
    // Normalize to the virtual desktop (0..65535)
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dx = (vw > 1) ? MulDiv(screenX - vx, 65535, vw - 1) : 0;
    input.mi.dy = (vh > 1) ? MulDiv(screenY - vy, 65535, vh - 1) : 0;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    SendInput(1, &input, sizeof(INPUT));
}

void Emulate::typeChar(wchar_t ch) {
    INPUT input[2] = {};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wScan = ch;
    input[0].ki.dwFlags = KEYEVENTF_UNICODE;
    input[1] = input[0];
    input[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

    SendInput(2, input, sizeof(INPUT));
}

void Emulate::pressMouseLeft() {
    INPUT input[1] = {};

    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;

    SendInput(1, input, sizeof(INPUT));
}

void Emulate::releaseMouseLeft() {
    INPUT input[1] = {};

    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTUP;

    SendInput(1, input, sizeof(INPUT));
}

void Emulate::pressMouseRight() {
    INPUT input[1] = {};

    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;

    SendInput(1, input, sizeof(INPUT));
}

void Emulate::releaseMouseRight() {
    INPUT input[1] = {};

    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_RIGHTUP;

    SendInput(1, input, sizeof(INPUT));
}

void Emulate::sleep(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}