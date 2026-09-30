#pragma once

// In-app log: every native log line goes to logcat as before and is also
// kept in a ring buffer (from app launch), which the developer "App log"
// screen reads via vesper_get_log — no adb needed, nothing scrolls away.
// Header-only so each translation unit (and the host GPU test) links alone.

#include <android/log.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>

namespace vesper {

struct AppLog {
    static constexpr size_t kMaxLines = 4000;
    std::mutex mutex;
    std::deque<std::string> lines;
    uint64_t dropped = 0;

    void add(int prio, const char* tag, const char* msg) {
        using namespace std::chrono;
        auto now = system_clock::now();
        std::time_t t = system_clock::to_time_t(now);
        int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
        std::tm tm{};
        localtime_r(&t, &tm);
        char head[48];
        const char level = prio >= ANDROID_LOG_ERROR ? 'E' : (prio >= ANDROID_LOG_WARN ? 'W' : 'I');
        std::snprintf(head, sizeof(head), "%02d:%02d:%02d.%03d %c/", tm.tm_hour, tm.tm_min, tm.tm_sec, ms, level);
        std::string line = std::string(head) + tag + ": " + msg;
        std::lock_guard<std::mutex> lk(mutex);
        lines.push_back(std::move(line));
        if (lines.size() > kMaxLines) { lines.pop_front(); ++dropped; }
    }
};

inline AppLog& appLog() {
    static AppLog log;
    return log;
}

} // namespace vesper

// Drop-in for __android_log_print: logcat + in-app buffer.
inline int vesperLog(int prio, const char* tag, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    vesper::appLog().add(prio, tag, buf);
    return __android_log_print(prio, tag, "%s", buf);
}
