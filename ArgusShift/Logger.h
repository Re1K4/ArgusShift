#pragma once
#include <string>
#include <iostream>
#include <fstream>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <functional>
#include <mutex>

class Logger {
public:
    static void initialize(const std::string& filename = "", bool consoleOutput = true);
    static void info(const std::string& message);
    static void warning(const std::string& message);
    static void error(const std::string& message);

    // Additional output destination (e.g. GUI log view). Pass nullptr to remove.
    static void setSink(std::function<void(const std::string&)> sink);

private:
    static void log(const std::string& level, const std::string& message);
    static std::string getCurrentTime();

    static std::ofstream logFile;
    static bool consoleOutputEnabled;
    static std::function<void(const std::string&)> sink;
    static std::mutex mutex;

    Logger() = delete;
};