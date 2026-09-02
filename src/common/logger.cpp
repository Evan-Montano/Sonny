// logger.cpp
// Implementation of the Logger class.

#include "logger.hpp"
#include "datetime.hpp"

#include <stdexcept>
#include <iostream>

namespace fs = std::filesystem;

void Logger::Start() {
    if (running == false) {
        running = true;
        if (!fs::exists(log_dir) && !fs::create_directories(log_dir)) {
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

    LogFile.flush();
    LogFile.close();
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

        LogFile << "[" << time << "] "
            << "[" << level_string << "] "
            << log.Message
            << '\n';
            
        if (log.LogToConsole) {
            std::cout << "[" << time << "] "
                << "[" << level_string << "] "
                << log.Message
                << std::endl;
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

void Logger::Log(Level level, const std::string& message, const bool& logToConsole) {
    {
        LogStruct log {
            level,
            message,
            logToConsole
        };

        std::lock_guard lock(Mutex);
        MessageQueue.push(log);
    }

    Cv.notify_one();
}