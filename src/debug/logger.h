#pragma once

#include <Windows.h>
#include <string>
#include <deque>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <ctime>

/// @file logger.h
/// @brief Thread-safe ring-buffer logger.  Outputs to the Win32 console
///        and keeps the last 256 entries for display in the ImGui debug tab.

enum class LogLevel { DBG, INFO, WARN, ERR };

struct LogEntry {
    LogLevel    level;
    std::string timestamp;
    std::string message;
};

class Logger {
public:
    static Logger& get() { static Logger s; return s; }

    void log(LogLevel lvl, const std::string& msg) {
        LogEntry e;
        e.level     = lvl;
        e.timestamp = now();
        e.message   = msg;

        // Colour output to the Win32 console.
        HANDLE hcon = GetStdHandle(STD_OUTPUT_HANDLE);
        WORD col = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE; // white default
        switch (lvl) {
        case LogLevel::DBG:  col = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
        case LogLevel::INFO: col = FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
        case LogLevel::WARN: col = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
        case LogLevel::ERR:  col = FOREGROUND_RED | FOREGROUND_INTENSITY; break;
        }
        const char* prefix[] = { "[DBG] ", "[INF] ", "[WRN] ", "[ERR] " };
        SetConsoleTextAttribute(hcon, col);
        std::cout << e.timestamp << "  " << prefix[(int)lvl] << msg << "\n";
        SetConsoleTextAttribute(hcon, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

        std::lock_guard<std::mutex> lk(m_mtx);
        m_entries.push_back(std::move(e));
        if (m_entries.size() > 256)
            m_entries.pop_front();
    }

    // Convenience wrappers.
    void dbg (const std::string& m) { log(LogLevel::DBG,  m); }
    void info(const std::string& m) { log(LogLevel::INFO, m); }
    void warn(const std::string& m) { log(LogLevel::WARN, m); }
    void err (const std::string& m) { log(LogLevel::ERR,  m); }

    std::deque<LogEntry> entries() {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_entries;
    }

private:
    Logger() = default;
    std::deque<LogEntry> m_entries;
    std::mutex           m_mtx;

    static std::string now() {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return buf;
    }
};

// Shorthand macros.
#define LOG_DBG(msg)  Logger::get().dbg(msg)
#define LOG_INFO(msg) Logger::get().info(msg)
#define LOG_WARN(msg) Logger::get().warn(msg)
#define LOG_ERR(msg)  Logger::get().err(msg)
