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
    bool history = true;    // false = not written to the history log (type = days)
    // Usage spike alarm (spike_used / spike_minutes): alarm when the number goes DOWN by
    // spikeUsed or more within at least spikeMinutes. 0 = no alarm for this indicator.
    double spikeUsed = 0;
    int spikeMinutes = 0;
};

struct Result {
    std::vector<IndVal> inds;
    std::string status = "Waiting";
    bool ok = false;       // last run succeeded
    bool stale = false;    // inds are from an older successful run
    bool running = false;
    bool blocked = false;  // last run did not fetch: exit IP country is not [app] exit_country (or unknown)
    time_t updated = 0;    // time of the last successful run
    // Last usage spike (src/spike.cpp). Shown as "!" next to the Updated time until the user
    // clicks it. spikeAt = 0: no spike to show.
    time_t spikeAt = 0;
    std::string spikeText;
};

// On success: new inds, status "OK". On failure: old inds kept and marked stale,
// status = error text. Returns true on success.
bool runService(const Service& s, Result& r);
