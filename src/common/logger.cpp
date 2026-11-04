// logger.cpp
// Implementation of the Logger class.

#include "logger.hpp"
#include "datetime.hpp"
#include "../ui/ui.hpp"

#include <stdexcept>
#include <iostream>

namespace fs = std::filesystem;

void Logger::Start() {
    Logger::SetConsoleRefreshCallback([] {
        UI::Get().Refresh();
    });

    if (running == false) {
        running = true;
        if (fs::exists(log_dir) == false && fs::create_directories(log_dir) == false) {
            throw std::runtime_error(
                "Failed to create log directory: " + log_dir.string()
            );
        }

        const std::string date = Common::DateTime::GetCurrentDateTime().ToString_Date();
        const fs::path log_file = log_dir / (date + ".log");
        LogFile.open(log_file, std::ios::app);

        if (LogFile.is_open() == false) {
            throw std::runtime_error(
                "Failed to open log file: " + log_file.string()
            );
        }

        WorkerThread = std::thread(&Logger::Observer);
    }
}

void Logger::Stop() {
    {
        std::lock_guard lock(Mutex);
        running = false;
    }

    Cv.notify_one();

    if (WorkerThread.joinable()) {
        WorkerThread.join();
    }

    {
        std::lock_guard lock(FileMutex);
        LogFile.flush();
        LogFile.close();
    }
}

bool Logger::ClearCurrentLogFile() {
    const fs::path log_file = log_dir / (Common::DateTime::GetCurrentDateTime().ToString_Date() + ".log");
    std::scoped_lock lock(Mutex, FileMutex);

    // Discard messages queued before the user's clear action so they cannot
    // be written back into the newly emptied file afterward.
    while (MessageQueue.empty() == false) MessageQueue.pop();
    ++LogGeneration;

    if (LogFile.is_open()) {
        LogFile.flush();
        LogFile.close();
    }

    std::ofstream cleared(log_file, std::ios::out | std::ios::trunc);
    if (cleared.is_open() == false) return false;
    cleared.close();

    if (running) {
        LogFile.open(log_file, std::ios::out | std::ios::app);
        if (LogFile.is_open() == false) return false;
    }

    return true;
}

void Logger::Observer() {
    while (true) {
        std::unique_lock lock(Mutex);

        Cv.wait(lock, [] {
            return MessageQueue.empty() == false || running == false;
        });

        if (running == false && MessageQueue.empty()) {
            break;
        }

        LogStruct log = std::move(MessageQueue.front());
        MessageQueue.pop();

        lock.unlock();
        
        std::string level_string;
        switch (log.Level) {
            case Level::Debug:
                level_string = "DEBUG";
                break;            
            case Level::Info:
                level_string = "INFO";
                break;            
            case Level::Warning:
                level_string = "WARN";
                break;            
            case Level::Error:
                level_string = "ERROR";
                break;
        }
        
        std::string time = Common::DateTime::GetCurrentDateTime().ToString_Time();

        const std::string formatted = 
            "[" + time + "] "
            "[" + level_string + "] "
            + log.Message;
        
        {
            std::scoped_lock lock(Mutex, FileMutex);
            if (log.Generation != LogGeneration) continue;
            if (LogFile.is_open()) LogFile << formatted << '\n';
        }

        if (log.LogToConsole) {
            std::lock_guard lock(Mutex);

            ConsoleMessages.push_back(formatted);

            if (ConsoleMessages.size() > MAX_CONSOLE_MESSAGES) {
                ConsoleMessages.pop_front();
            }

            if (ConsoleRefreshCallback) {
                ConsoleRefreshCallback();
            }
        }
    }
}

void Logger::Debug(const std::string& message, const bool& logToConsole) {
    Log(Level::Debug, message, logToConsole);
}

void Logger::Info(const std::string& message, const bool& logToConsole) {
    Log(Level::Info, message, logToConsole);
}

void Logger::Warning(const std::string& message, const bool& logToConsole) {
    Log(Level::Warning, message, logToConsole);
}

void Logger::Error(const std::string& message, const bool& logToConsole) {
    Log(Level::Error, message, logToConsole);
}

void Logger::Log(const Level level, const std::string& message, const bool& logToConsole) {
    {
        std::lock_guard lock(Mutex);
        const LogStruct log {
            .Level = level,
            .Message = message,
            .LogToConsole = logToConsole,
            .Generation = LogGeneration
        };
        MessageQueue.push(log);
    }

    Cv.notify_one();
}

std::vector<std::string> Logger::GetConsoleMessages() {
    std::lock_guard lock(Mutex);
    return {
        ConsoleMessages.begin(),
        ConsoleMessages.end()
    };
}

void Logger::SetConsoleRefreshCallback(std::function<void()> callback) {
    ConsoleRefreshCallback = std::move(callback);
}
