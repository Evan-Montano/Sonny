// logger.hpp
// Responsible for logging messages to the appropriate log file under the
// ~/storage/logs directory.
// The log file is named based on the current date in the format yyyymmdd.log.

#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <queue>
#include <fstream>

class Logger {
public:
    enum class Level {
        Debug,
        Info,
        Warning,
        Error
    };

    struct LogStruct {
        Level Level;
        std::string Message;
        bool LogToConsole = false;
    };

    // CONSTRUCTOR
    Logger() = delete;

    // METHODS
    static void Start();
    static void Stop();

    static void Debug(const std::string& message, const bool &logToConsole = false);
    static void Info(const std::string& message, const bool& logToConsole = false);
    static void Warning(const std::string& message, const bool& logToConsole = false);
    static void Error(const std::string& message, const bool& logToConsole = false);

private:
    // MEMBERS
    static inline std::filesystem::path log_dir = "storage/logs";

    static inline std::mutex Mutex;
    static inline std::condition_variable Cv;
    static inline std::queue<LogStruct> MessageQueue;
    static inline std::thread WorkerThread;
    static inline std::ofstream LogFile;
    static inline bool running = false;

    // METHODS
    static void Log(Level level, const std::string& message, const bool& logToConsole = false);
    static void Observer();
};