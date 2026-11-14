// Tiny JSON path reader. It walks the text, it does not build a tree.
// Path examples:  a.b.c   list[0].name   [0].uuid   limits[kind=session].percent
#pragma once
#include <string>

// Returns true when the path exists. Strings are unescaped; numbers, true/false
// are returned as text; null gives ""; objects/arrays give their raw JSON text.
bool jsonGet(const std::string& doc, const std::string& path, std::string& out);
