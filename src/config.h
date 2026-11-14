// Loads services\*.conf (INI style). One file = one service.
#pragma once
#include <string>
#include <vector>
#include "tz.h"

struct KV {
    std::string key, val;
    int line = 0;
};

struct Section {
    std::string id;  // the part after "step." / "indicator."
    int line = 0;
    std::vector<KV> kv;
    const KV* find(const std::string& key) const;
    std::string get(const std::string& key, const std::string& def = "") const;
};

struct Service {
    std::string file;  // file name, used as the stable key
    std::string name, icon, site, userAgent;
    std::string proxy = "system";
    TimeZone tz;  // "timezone" key: times from the API are shown in this zone
    bool enabled = true, debug = false;
    int order = 100, intervalMin = 60, timeoutSec = 20;
    size_t maxBody = 2 * 1024 * 1024;
    std::vector<Section> steps, indicators;
    std::string error;  // config error: shown in the Status column, service is not run
};

// Files that start with "_" are ignored. Result is sorted by order, then name.
std::vector<Service> loadServices(const std::string& dir);
