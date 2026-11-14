// Time zones by IANA name ("Europe/Helsinki") or Windows key ("FLE Standard Time").
#pragma once
#include <windows.h>
#include <string>
#include <ctime>

struct TimeZone {
    bool valid = false;  // false = use the PC's local time zone
    DYNAMIC_TIME_ZONE_INFORMATION dtzi{};
};

// Empty name = PC local time. Returns false (and err) for an unknown name.
bool findTimeZone(const std::string& name, TimeZone& out, std::string& err);

// UTC time -> wall clock in the zone.
SYSTEMTIME toZone(time_t utc, const TimeZone& tz);

// Short text, no leading zeros except minutes:
//   same day: "4:00"   within 6 days: "Tue 4:00"   else: "11/18 4:00"
// withWeekday=false skips the weekday form (used for past times).
// roundMinute=true rounds to the nearest minute (API reset times like 01:59:59.97 -> 2:00);
// false truncates (an update at 10:13:40 is shown as 10:13, like a clock).
std::wstring shortTime(time_t utc, const TimeZone& tz, bool withWeekday, bool roundMinute);

// Local wall time in the zone -> UTC (for API times without an offset).
time_t fromZone(const SYSTEMTIME& local, const TimeZone& tz);

// App-wide display zone ([app] timezone= in usage-monitor.ini). Set once at
// startup, before the worker thread starts; nullptr = not configured.
void setAppTimeZone(const TimeZone& tz);
const TimeZone* appTimeZone();
