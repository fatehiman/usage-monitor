// Small helpers: UTF-8 <-> UTF-16, strings, files, log.
#pragma once
#include <string>
#include <vector>

std::wstring W(const std::string& s);
std::string U(const std::wstring& w);

std::string trim(const std::string& s);
std::string lower(std::string s);
std::string upper(std::string s);
bool startsWith(const std::string& s, const std::string& prefix);
std::vector<std::string> split(const std::string& s, char sep);

std::string exeDir();                       // folder of the .exe, UTF-8
std::string expandEnv(const std::string& s); // %USERPROFILE% etc.
std::string urlEncode(const std::string& s);
bool readFileUtf8(const std::string& path, std::string& out, size_t maxBytes);

void logLine(const std::string& msg);       // usage-monitor.log next to the exe
