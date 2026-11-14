// Usage report. Runs in a SEPARATE short-lived process ("usage-monitor.exe --report <service file>"),
// so the tray app never holds the history or the HTML in memory.
#pragma once
#include <string>

// Reads the last historyDays of history\*.tsv for one service, writes reports\<service>.html
// and opens it in the default browser. Returns the process exit code.
int runReport(const std::string& serviceFile, bool openBrowser = true);
