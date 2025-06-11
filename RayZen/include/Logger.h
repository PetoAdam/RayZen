#pragma once
#include <iostream>
#include <string>
#include <mutex>

enum class LogLevel { DEBUG, INFO, ERROR };

class Logger {
public:
    static void setLevel(LogLevel lvl) { getInstance().level = lvl; }
    static LogLevel getLevel() { return getInstance().level; }

    static void debug(const std::string& msg) {
        if (getInstance().level <= LogLevel::DEBUG)
            getInstance().log("[DEBUG] ", msg, std::cout);
    }
    static void info(const std::string& msg) {
        if (getInstance().level <= LogLevel::INFO)
            getInstance().log("[INFO] ", msg, std::cout);
    }
    static void error(const std::string& msg) {
        if (getInstance().level <= LogLevel::ERROR)
            getInstance().log("[ERROR] ", msg, std::cerr);
    }

private:
    LogLevel level = LogLevel::INFO;
    std::mutex mtx;

    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }
    void log(const std::string& prefix, const std::string& msg, std::ostream& os) {
        std::lock_guard<std::mutex> lock(mtx);
        os << prefix << msg << std::endl;
    }
};
