// storage.cpp
// Helper method implementations for filesystem shenanigans.

#include "storage.hpp"
#include "logger.hpp"

#include <format>

namespace Common {
    namespace Storage {

        bool FileExists(const std::filesystem::path& path) {
            return std::filesystem::exists(path);
        }

        bool IsFileEmpty(const std::filesystem::path &path) {
            return std::filesystem::is_empty(path);
        }

        void DeleteFile(const std::filesystem::path &path) {
            try {
                if (FileExists(path))
                    std::filesystem::remove(path);
            }
            catch (const std::filesystem::filesystem_error &error) {
                Logger::Error(std::format("Error while deleting file at path {}: {}", path.string(), error.what()), true);
            } 
        }

        void DeleteDirectory(const std::filesystem::path &path) {
            try {
                std::filesystem::remove_all(path);
            }
            catch (const std::filesystem::filesystem_error &error) {
                Logger::Error(std::format("Error while deleting directory at path {}: {}", path.string(), error.what()), true);
            } 
        }

        bool CreateDirectory(const std::filesystem::path &path) {
            return std::filesystem::create_directories(path);
        }

        std::ofstream OpenFile_App(const std::filesystem::path &basePath, const std::string &fileName, const bool &binaryMode) {
            const std::filesystem::path outFile = 
                basePath / fileName;
            auto mode = binaryMode ? std::ios::binary | std::ios::trunc : std::ios::app;
            return std::ofstream(
                outFile,
                mode
            );
        }

    }
}