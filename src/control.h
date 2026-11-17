#pragma once

#include <string>

// APKL_CONTROL=<fifo>: test harness that injects input and takes screenshots
namespace control {
void install();
std::string takeScreenshotRequest();  // empty if none pending
bool writePng(const std::string& file, int w, int h, const unsigned char* rgbaBottomUp);
}
