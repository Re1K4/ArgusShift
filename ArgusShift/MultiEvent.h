#pragma once
#include "Event.h"
#include <vector>
#include <opencv2/opencv.hpp>
#include "Logger.h"
#include <windows.h>
#include <sstream>
#include <stdexcept>
#include <type_traits>

enum MatchMode {
    ALL_MATCH,
    ANY_MATCH
};

class MultiEvent {
public:
    // Receive multiple Events with variable argument templates
    template<typename... Events,
        typename = std::enable_if_t<(std::is_same_v<Events, Event> && ...)>>
    MultiEvent(Events&... events) {
        static_assert(sizeof...(events) >= 2, "MultiEvent requires at least two Event objects.");
        (eventList.push_back(&events), ...); //Variable Argument Expansion
        hScreen = GetDC(NULL);
        calculateBoundingRect();
    }

    // Receive Events from a list built at runtime (e.g. GUI configuration)
    explicit MultiEvent(const std::vector<Event*>& events);

    ~MultiEvent();

    bool checkColorMulti(MatchMode mode = ALL_MATCH, bool debugFlag = false);
    bool checkRGBMulti(MatchMode mode = ALL_MATCH, bool debugFlag = false);

    // Recompute the capture area; call after changing the x / y of the Events
    void calculateBoundingRect();

private:
    std::vector<Event*> eventList;
    cv::Rect boundingRect;
    cv::Mat screenCapture;
    HDC hScreen;

    void captureScreen();
    cv::Vec3b getPixelFromCapture(int x, int y);
};