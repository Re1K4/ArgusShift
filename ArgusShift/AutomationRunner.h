#pragma once
#include "AutomationConfig.h"
#include <atomic>
#include <string>
#include <thread>

// Executes an Automation::Project on a worker thread.
// Each step is selected by Control::NOW; when a step's condition becomes true its
// action runs and Control::changeCtrl() moves to the configured next step.
// Pressing ESC (or calling stop()) ends the run.
class AutomationRunner {
public:
    AutomationRunner() = default;
    ~AutomationRunner();
    AutomationRunner(const AutomationRunner&) = delete;
    AutomationRunner& operator=(const AutomationRunner&) = delete;

    // Validate and start. Returns false with an error message when it cannot start.
    bool start(const Automation::Project& project, std::wstring& error);

    // Request stop and wait for the worker thread
    void stop();

    bool isRunning() const { return running.load(); }

    // Index of the step currently executing (-1 when idle)
    int currentStep() const { return current.load(); }

private:
    void threadMain(Automation::Project project);
    bool sleepInterruptible(int ms);

    std::thread worker;
    std::atomic<bool> running{ false };
    std::atomic<bool> stopRequested{ false };
    std::atomic<int> current{ -1 };
};
