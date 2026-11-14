// Usage history: one log file per day in <exe>\history\YYYY-MM-DD.tsv (display time zone).
// Each successful service run appends one line per indicator:
//   <utc epoch> TAB <service file> TAB <indicator id> TAB <number> TAB <pre> TAB <post> TAB <label>
// Files older than [app] history_days (default 30) are deleted.
#pragma once
#include <string>
#include <vector>
#include <ctime>
#include "runner.h"

// [app] settings from usage-monitor.ini next to the exe.
struct AppSettings {
    int historyDays = 30;  // history_days = 30    keep / read this many days of logs
    int weekStart = 6;     // week_start = sat     first day of the week in the report (0 = Sun .. 6 = Sat)
    std::string spikeSound;  // spike_sound = C:\x.wav   sound of a usage spike (empty = Windows "Exclamation")
};

// Reads usage-monitor.ini [app]: timezone (sets the app display zone), history_days, week_start, spike_sound.
void loadAppSettings();
const AppSettings& appSettings();
std::wstring iniPath();

std::string historyDir();

// Day number (days since 1970-01-01) of a UTC time in the display zone.
int displayDay(time_t utc);
// "YYYY-MM-DD" of a day number.
std::string dayName(int day);

void historyAppend(const std::string& serviceFile, const std::vector<IndVal>& inds, time_t when);
// Deletes history files older than historyDays. Cheap; the worker calls it once per day.
void historyCleanup();
