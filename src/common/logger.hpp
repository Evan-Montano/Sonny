// logger.hpp
// Responsible for logging messages to the appropriate log file under the
// ~/storage/logs directory.
// The log file is named based on the current date in the format yyyymmdd.log.

#pragma once

#include <filesystem>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <queue>
#include <fstream>
#include <deque>
#include <vector>
#include <functional>
#include <cstdint>

/**
 * @brief Queues log messages for daily files and the UI console on a background worker.
 */
class Logger{
public:
    // MEMBERS

    enum class Level {
        Debug,
        Info,
        Warning,
        Error
    };

    /**
     * @brief Carries one queued log message and its console generation.
     */
    struct LogStruct {
        // MEMBERS

        Level Level;
        std::string Message;
        bool LogToConsole = false;
        std::uint64_t Generation = 0;
    };

    // CONSTRUCTOR
    Logger() = delete;

    // METHODS
    static void Start();
    static void Stop();
    static bool ClearCurrentLogFile();

    static void Debug(const std::string& message, const bool &logToConsole = false);
    static void Info(const std::string& message, const bool& logToConsole = false);
    static void Warning(const std::string& message, const bool& logToConsole = false);
    static void Error(const std::string& message, const bool& logToConsole = false);

    static std::vector<std::string> GetConsoleMessages();

private:
    // DESTRUCTOR
    ~Logger() {
        Stop();
    }

    // MEMBERS
    static inline constexpr std::size_t MAX_CONSOLE_MESSAGES = 9;
    static inline std::filesystem::path LogDir = "storage/logs";

    static inline std::mutex Mutex;
    static inline std::mutex FileMutex;
    static inline std::condition_variable Cv;
    static inline std::queue<LogStruct> MessageQueue;
    static inline std::deque<std::string> ConsoleMessages;
    static inline std::thread WorkerThread;
    static inline std::ofstream LogFile;
    static inline bool Running = false;
    static inline std::uint64_t LogGeneration = 0;
    static inline std::function<void()> ConsoleRefreshCallback;

    // METHODS
    static void SetConsoleRefreshCallback(std::function<void()> callback);

    static void Log(Level level, const std::string& message, const bool& logToConsole = false);
    static void Observer();
};
