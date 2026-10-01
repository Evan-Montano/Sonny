// storage.cpp
// Helper method implementations for filesystem shenanigans.

#include "storage.hpp"
#include "common/datetime.hpp"
#include "common/jsonutil.hpp"
#include "logger.hpp"

#include <filesystem>
#include <format>
#include <fstream>

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

        void CreateFile(const std::filesystem::path &path) {
            try {
                std::ofstream file(path);
                if (file.is_open()) {
                    file.close();
                }
            }
            catch (const std::filesystem::filesystem_error &error) {
                Logger::Error(std::format("Error while creating file at path {}: {}", path.string(), error.what()), true);
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

        // Will probably delete, this isn't useful, so don't use it.
        std::ofstream OpenFile_App(const std::filesystem::path &basePath, const std::string &fileName, const bool &binaryMode) {
            const std::filesystem::path outFile = 
                basePath / fileName;
            auto mode = binaryMode ? std::ios::binary | std::ios::trunc : std::ios::app;
            return std::ofstream(
                outFile,
                mode
            );
        }

        // Settings

        /**
         * @brief Save the current settings to disk.
         * 
         */
        void Settings::Save() {
            if (CreateDirectory(SETTINGS_BASE_PATH) == false) {
                throw std::runtime_error(
                    "Unable to create settings directory: " + SETTINGS_BASE_PATH.string()
                );
            }

            json.Set("START_DATE", START_DATE.GetTimestamp());
            json.Set("END_DATE", END_DATE.GetTimestamp());

            const std::filesystem::path filePath = SettingsFilePath();

            std::ofstream file(filePath, std::ios::out | std::ios::trunc);

            if (file.is_open() == false) {
                throw std::runtime_error(
                    "Unable to open settings file for writing: " + filePath.string()
                );
            }

            file << json.ToString();

            if (file.good() == false) {
                file.close();

                throw std::runtime_error(
                    "Unable to write settings file: " + filePath.string()
                );
            }

            file.close();
        }

        /**
         * @brief Reload the settings from disk.
         * 
         */
        void Settings::Reload() {
            const std::filesystem::path filePath = SettingsFilePath();

            if (FileExists(filePath) == false) {
                Reset();
                Save();
                return;
            }

            json = Common::JsonUtility::FromFile(filePath);

            Common::UnixTimestamp startTimestamp = 0;
            Common::UnixTimestamp endTimestamp = 0;

            if (json.TryGet("START_DATE", startTimestamp)) {
                START_DATE = Common::DateTime(startTimestamp, true);
            }
            if (json.TryGet("END_DATE", endTimestamp)) {
                END_DATE = Common::DateTime(endTimestamp, true);
            }
        }

        /**
         * @brief Rest all settings to their default value;
         * 
         */
        void Settings::Reset() {
            json = Common::JsonUtility{};
            START_DATE = Common::DateTime::GetCurrentDateTime();
            END_DATE = Common::DateTime::GetCurrentDateTime();
        }

        /**
         * @brief Check whether a setting exists.
         *
         * @param key Setting key.
         * @return true if the setting exists.
         * @return false if the setting does not exist.
         */
        bool Settings::Has(const std::string &key) const
        {
            nlohmann::json value;

            return json.TryGet(key, value);
        }

        /**
         * @brief Remove a generic setting.
         *
         * @param key Setting key.
         * @return true if the setting was removed.
         * @return false if the setting did not exist.
         */
        bool Settings::Remove(const std::string &key)
        {
            return json.Erase(key);
        }

    }
}