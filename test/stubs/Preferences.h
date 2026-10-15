#pragma once
#include "Arduino.h"
#include <map>
static std::map<std::string,std::string> durableNvs;
static bool failNvsWrites = false;
class Preferences {
public:
    bool begin(const char*,bool) { return true; }
    bool isKey(const char *key) { return durableNvs.count(key) != 0; }
    String getString(const char *key,const char *fallback) { auto it = durableNvs.find(key); return it == durableNvs.end() ? String(fallback) : String(it->second); }
    size_t putString(const char *key,const String &value) { if(failNvsWrites) return 0; durableNvs[key] = value.c_str(); return value.length(); }
};
