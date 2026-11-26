#pragma once
#include "Logger.h"
#include <opencv2/opencv.hpp>
#include <string>
#include <iostream>
#include <windows.h>


class TemplateMatching {
public:
    cv::Rect lastMatchResult;
    TemplateMatching(const std::string& relativePath);
    ~TemplateMatching();
    bool match(const cv::Rect& region, double fuzzy);
    bool matchBinary(const cv::Rect& region, double fuzzy);

    // Save top-down 32bpp BGRX pixels (w x h) as an image usable as a template.
    // The format follows the extension (.png recommended). Unicode paths are supported.
    static bool saveImage(const std::wstring& path, int w, int h, const uint32_t* bgrx);

private:
    HDC hScreenDC;
    cv::Mat image;
    cv::Mat captureScreen(const cv::Rect& region);
    cv::Mat loadResourceData(int resourceId);
};
