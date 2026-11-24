// storage.hpp
// Helper method declarations for filesystem shenanigans.

#pragma once

#include "../common/datetime.hpp"
#include "../common/jsonutil.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

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
        /** @brief Non-throwing inspection; missing files return false with no error. */
        bool FileExists(const std::filesystem::path &path, std::error_code &error) noexcept;

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
        /** @brief Remove a file, returning whether it existed; failures are returned in error. */
        bool DeleteFile(const std::filesystem::path &path, std::error_code &error) noexcept;

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
         * @return false if the directory already exists.
         * @throws std::filesystem::filesystem_error If the directories cannot be created.
         */
        bool CreateDirectory(const std::filesystem::path &path);
        bool CreateDirectory(const std::filesystem::path &path, std::error_code &error);

        /** @brief Rename or replace a file. On failure neither file is explicitly removed. */
        void RenameFile(const std::filesystem::path &from, const std::filesystem::path &to);
        void RenameFile(const std::filesystem::path &from, const std::filesystem::path &to, std::error_code &error) noexcept;
        std::uintmax_t FileSize(const std::filesystem::path &path, std::error_code &error) noexcept;

        /** @brief Open streams for domain-specific parsing/streaming; callers check their state. */
        std::ifstream OpenInputFile(const std::filesystem::path &path, std::ios::openmode mode = std::ios::in);
        std::ofstream OpenOutputFile(const std::filesystem::path &path, std::ios::openmode mode = std::ios::out | std::ios::trunc);

        /** @brief Read or write complete byte strings, throwing on open/read/write/close failure. */
        std::string ReadFile(const std::filesystem::path &path);
        void WriteFile(const std::filesystem::path &path, std::string_view contents);

        /**
         * @brief Stage a complete file beside its destination, then replace it with one rename.
         * Creates parent directories and uses a unique temporary file. A failure retains the old
         * destination and cleans up the temporary file. Atomic visibility does not imply crash durability.
         * Replacements retain permission bits; new files are private (0600). Staging is close-on-exec.
         */
        void WriteFileAtomically(const std::filesystem::path &path, std::string_view contents);

        /** @brief Read native-layout records, rejecting partial trailing records and short reads. */
        template<typename T>
        std::vector<T> ReadBinaryRecords(const std::filesystem::path &path) {
            static_assert(std::is_trivially_copyable_v<T>);
            auto file = OpenInputFile(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                throw std::runtime_error("Unable to open file for reading: " + path.string());
            }
            const std::streamoff size = file.tellg();
            if (size < 0 || size % sizeof(T) != 0 ||
                static_cast<std::uintmax_t>(size) > std::numeric_limits<std::size_t>::max() ||
                size > std::numeric_limits<std::streamsize>::max()) {
                throw std::runtime_error("Invalid binary record file size: " + path.string());
            }
            std::vector<T> records(static_cast<std::size_t>(size) / sizeof(T));
            file.seekg(0);
            if (size > 0) file.read(reinterpret_cast<char*>(records.data()), static_cast<std::streamsize>(size));
            if (!file || file.gcount() != size) {
                throw std::runtime_error("Failed reading complete file: " + path.string());
            }
            return records;
        }

        /** @brief Write native-layout records, checking completion and close errors. */
        template<typename T>
        void WriteBinaryRecords(const std::filesystem::path &path, std::span<const T> records) {
            static_assert(std::is_trivially_copyable_v<T>);
            WriteFile(path, std::string_view(reinterpret_cast<const char*>(records.data()), records.size_bytes()));
        }


        /**
         * @brief Application settings stored in a persistent JSON file.
         *
         * Settings is a singleton. The settings file is loaded when the
         * singleton is first accessed. Date fields are saved as signed *_MS timestamps;
         * legacy START_DATE/END_DATE values in seconds are accepted on reload.
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
