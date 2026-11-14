// Runs the steps of one service and formats its indicators.
#pragma once
#include "config.h"
#include <ctime>

enum Level { LV_NORMAL = 0, LV_WARN = 1, LV_ALERT = 2 };

struct IndVal {
    std::string text;  // "Label: value", ready to show
    int level = LV_NORMAL;
    bool tray = false;  // include in the tray tooltip
    double pie = -1;    // remaining part 0..100 for the small pie; -1 = no pie
    // For the history log (src/history.cpp):
    std::string id;     // indicator id ([indicator.<id>])
    std::string label;  // label after variables are filled in
    double num = 0;     // the number shown: percent, remain (bytes when scaled), or value
    std::string pre, post;  // text around the number: "$" / "%" / " GB" ... post "B" = bytes
};

struct Result {
    std::vector<IndVal> inds;
    std::string status = "Waiting";
    bool ok = false;       // last run succeeded
    bool stale = false;    // inds are from an older successful run
    bool running = false;
    time_t updated = 0;    // time of the last successful run
};

// On success: new inds, status "OK". On failure: old inds kept and marked stale,
// status = error text. Returns true on success.
bool runService(const Service& s, Result& r);
