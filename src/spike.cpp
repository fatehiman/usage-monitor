#include "spike.h"
#include "util.h"
#include <cstdio>
#include <deque>
#include <map>

namespace {
struct Reading {
    time_t t;
    double v;
};
std::map<std::string, std::deque<Reading>> g_readings;  // "file/indicator id" -> readings, oldest first

std::string fmt(double v, const IndVal& ind) {
    char b[48];
    // Whole numbers for percent, IRT, credits ...; cents for small money amounts.
    bool whole = ind.post == "%" || ind.spikeUsed >= 10;
    snprintf(b, sizeof b, whole ? "%.0f" : "%.2f", v);
    return ind.pre + b + ind.post;
}
}  // namespace

bool spikeCheck(const Service& s, Result& r) {
    time_t now = r.updated ? r.updated : time(nullptr);
    std::string found;
    for (auto& ind : r.inds) {
        std::string key = s.file + "/" + ind.id;
        if (ind.spikeUsed <= 0) { g_readings.erase(key); continue; }
        auto& q = g_readings[key];
        time_t window = (time_t)ind.spikeMinutes * 60;
        // Compare with the highest reading since the newest one that is at least "window" old
        // (the window is a minimum). Newer readings count too: the same drop in less time is faster.
        while (q.size() > 1 && now - q[1].t >= window) q.pop_front();  // drop the older ones
        const Reading* base = nullptr;
        for (auto& rd : q)
            if (!base || rd.v >= base->v) base = &rd;
        if (base) {
            double used = base->v - ind.num;  // the shown number goes down when usage goes up
            if (used >= ind.spikeUsed) {
                int mins = (int)((now - base->t + 30) / 60);
                if (mins < 1) mins = 1;
                std::string label = ind.label.empty() ? ind.id : ind.label;
                std::string t = label + ": " + fmt(base->v, ind) + " -> " + fmt(ind.num, ind) + " in " +
                                std::to_string(mins) + " min (limit: " + fmt(ind.spikeUsed, ind) + " in " +
                                std::to_string(ind.spikeMinutes) + " min)";
                found += (found.empty() ? "" : "\r\n") + t;
                q.clear();  // new baseline, so the same drop does not ring again on the next run
            }
        }
        q.push_back({now, ind.num});
        if (q.size() > 200) q.pop_front();
    }
    if (found.empty()) return false;
    r.spikeAt = now;
    r.spikeText = found;
    logLine(s.file + ": usage spike: " + found);
    return true;
}
