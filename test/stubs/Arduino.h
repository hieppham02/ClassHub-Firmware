#pragma once
#include <string>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <ArduinoJson.h>
class String {
    std::string text;
    mutable size_t offset = 0;
public:
    String() = default;
    String(const char *s):text(s ? s : "") {}
    String(const std::string &s):text(s) {}
    const char *c_str() const { return text.c_str(); }
    size_t length() const { return text.size(); }
    bool isEmpty() const { return text.empty(); }
    String substring(size_t start, size_t end) const { return text.substr(start,end-start); }
    void toLowerCase() { std::transform(text.begin(),text.end(),text.begin(),[](char c){return (char)std::tolower(c);}); }
    long toInt() const { return std::strtol(text.c_str(),nullptr,10); }
    bool operator==(const String &other) const { return text == other.text; }
    bool operator!=(const String &other) const { return !(*this == other); }
    size_t write(uint8_t c) { text += (char)c; return 1; }
    size_t write(const uint8_t *data, size_t count) { text.append((const char*)data,count); return count; }
    int read() const { return offset < text.size() ? (unsigned char)text[offset++] : -1; }
    size_t readBytes(char *out, size_t count) const { size_t n = std::min(count,text.size()-offset); memcpy(out,text.data()+offset,n); offset += n; return n; }
};
ARDUINOJSON_BEGIN_PUBLIC_NAMESPACE
template<> struct Converter<String> {
    static void toJson(const String &s, JsonVariant destination) { destination.set(std::string(s.c_str())); }
    static String fromJson(JsonVariantConst source) {
        if (source.is<const char*>()) return source.as<const char*>();
        if (source.is<int64_t>()) return std::to_string(source.as<int64_t>());
        return "";
    }
    static bool checkJson(JsonVariantConst source) { return source.is<const char*>(); }
};
ARDUINOJSON_END_PUBLIC_NAMESPACE
struct SerialStub { void println(const char*) {} };
static SerialStub Serial;
static unsigned long fakeMillis = 0;
inline unsigned long millis() { return fakeMillis; }
inline void configTime(int,int,const char*,const char*) {}
