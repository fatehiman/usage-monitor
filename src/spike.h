// Usage spike alarm. Each [indicator.*] may have:
//   spike_used = 5        alarm when the number goes down by 5 or more (5 %, $5, 5 IRT ...)
//   spike_minutes = 1     ... within this time. It is a minimum: the newest reading that is at
//                          least this old is used, so a longer check interval is fine.
// Readings are kept in memory only (worker thread), so the first run after start is a baseline.
#pragma once
#include "config.h"
#include "runner.h"

// Call after each successful run. On a spike it sets r.spikeAt / r.spikeText and returns true.
bool spikeCheck(const Service& s, Result& r);
