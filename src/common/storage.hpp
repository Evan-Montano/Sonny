// storage.hpp
// Helper method declarations for filesystem shenanigans.

#pragma once

#include "../common/datetime.hpp"
#include "../common/jsonutil.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace Common {
    namespace Storage {

        /**
         * @brief Base path for corpus files directory.
         * 
         */
        const std::filesystem::path CORPUS_BASE_PATH = "storage/corpus/";

        /**
         * @brief Base path for the record files directory.
         * 
         */
        const std::filesystem::path RECORDS_BASE_PATH = "storage/records/";

        /**
         * @brief Base path for the indexes files directory.
         * 
         */
        const std::filesystem::path INDEXES_BASE_PATH = "storage/indexes/";

        /**
         * @brief Base path for the indexes files directory.
         * 
         */
        const std::filesystem::path METADATA_BASE_PATH = "storage/metadata/";

        /**
         * @brief Returns if a file at the specified path exists.
         *
         * @param path File path.
         * @return true if the file exists.
         * @return false if the file does not exist.
         */
        bool FileExists(const std::filesystem::path &path);

        /**
         * @brief Returns if a file at the specified path is empty.
         *
         * @param path File path.
         * @return true if the file is empty.
         * @return false if the file is not empty.
         */
        bool IsFileEmpty(const std::filesystem::path &path);

        /**
         * @brief Deletes the file at the specified path.
         *
         * @param path File path.
         */
        void DeleteFile(const std::filesystem::path &path);

        /**
         * @brief Creates a file at the specified path.
         *
         * @param path File path.
         */
        void CreateFile(const std::filesystem::path &path);

        /**
         * @brief Deletes the specified directory and its contents recursively.
         *
         * @param path Directory path.
         */
        void DeleteDirectory(const std::filesystem::path &path);

        /**
         * @brief Creates the specified directory.
         *
         * @param path Directory path.
         * @return true if the directory was created.
         * @return false if the directory could not be created.
         */
        bool CreateDirectory(const std::filesystem::path &path);

        /**
         * @brief Creates a file if it does not exist and opens it in append mode.
         * Optionally opens the file in binary mode.
         *
         * @param basePath Base directory path.
         * @param fileName File name.
         * @param binaryMode Whether the file should be opened in binary mode.
         * @return Open file stream.
         */
        std::ofstream OpenFile_App(
            const std::filesystem::path &basePath,
            const std::string &fileName,
            const bool &binaryMode = false
        );


        /**
         * @brief Application settings stored in a persistent JSON file.
         *
         * Settings is a singleton. The settings file is loaded when the
         * singleton is first accessed.
         */
        class Settings {
        public:

            /**
             * @brief Get the singleton Settings instance.
             *
             * @return Reference to the Settings instance.
             */
            static Settings &Get() {
                static Settings instance;
                return instance;
            }

            /**
             * @brief Save the current settings to disk.
             */
            void Save();

            /**
             * @brief Reload the settings from disk.
             */
            void Reload();

            /**
             * @brief Reset all settings to their default values.
             */
            void Reset();

            /**
             * @brief Check whether a setting exists.
             *
             * @param key Setting key.
             * @return true if the setting exists.
             * @return false if the setting does not exist.
             */
            bool Has(const std::string &key) const;

            /**
             * @brief Set a generic setting value.
             *
             * @tparam T Type of the setting value.
             * @param key Setting key.
             * @param value Setting value.
             */
            template<typename T>
            void Set(const std::string &key, const T &value) {
                json.Set(key, value);
            }

            /**
             * @brief Get a generic setting value.
             *
             * @tparam T Type to convert the setting to.
             *
             * @param key Setting key.
             * @return Setting value.
             */
            template<typename T>
            T Get(const std::string &key) const {
                return json.Get<T>(key);
            }

            /**
             * @brief Remove a generic setting.
             *
             * @param key Setting key.
             * @return true if the setting was removed.
             * @return false if the setting did not exist.
             */
            bool Remove(const std::string &key);

            /**
             * @brief Begin date for export data.
             */
            Common::DateTime START_DATE = Common::DateTime::GetCurrentDateTime();

            /**
             * @brief End date for export data.
             */
            Common::DateTime END_DATE = Common::DateTime::GetCurrentDateTime();

        private:

            /**
             * @brief Construct the Settings object and load settings from disk.
             */
            Settings() {
                Reload();
            }

            Settings(const Settings&) = delete;
            Settings &operator=(const Settings&) = delete;
            Settings(Settings&&) = delete;
            Settings &operator=(Settings&&) = delete;

            /**
             * @brief Base path for settings directory.
             */
            const std::filesystem::path SETTINGS_BASE_PATH = "storage/settings/";

            /**
             * @brief File name for Sonny application settings.
             */
            const std::string SETTINGS_FILE_NAME = "sonny_settings.json";

            /**
             * @brief Path to the settings file.
             */
            std::filesystem::path SettingsFilePath() const {
                return SETTINGS_BASE_PATH / SETTINGS_FILE_NAME;
            }

            /**
             * @brief JSON representation of the application settings.
             */
            Common::JsonUtility json{};
        };

    }
}