#pragma once

// Start the automation editor window. `initialProject` may be nullptr.
// Returns the process exit code when the window is closed.
int RunGui(const wchar_t* initialProject);
