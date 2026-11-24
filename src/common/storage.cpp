// storage.cpp
// Helper method implementations for filesystem shenanigans.

#include "storage.hpp"
#include "common/datetime.hpp"
#include "common/jsonutil.hpp"
#include "logger.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

        bool FileExists(const std::filesystem::path &path, std::error_code &error) noexcept {
            return std::filesystem::exists(path, error);
        }

        bool DeleteFile(const std::filesystem::path &path, std::error_code &error) noexcept {
            return std::filesystem::remove(path, error);
        }

        bool CreateDirectory(const std::filesystem::path &path, std::error_code &error) {
            return std::filesystem::create_directories(path, error);
        }

        void RenameFile(const std::filesystem::path &from, const std::filesystem::path &to) {
            std::filesystem::rename(from, to);
        }

        void RenameFile(const std::filesystem::path &from, const std::filesystem::path &to, std::error_code &error) noexcept {
            std::filesystem::rename(from, to, error);
        }

        std::uintmax_t FileSize(const std::filesystem::path &path, std::error_code &error) noexcept {
            return std::filesystem::file_size(path, error);
        }

        std::ifstream OpenInputFile(const std::filesystem::path &path, const std::ios::openmode mode) {
            return std::ifstream(path, mode);
        }

        std::ofstream OpenOutputFile(const std::filesystem::path &path, const std::ios::openmode mode) {
            return std::ofstream(path, mode);
        }

        std::string ReadFile(const std::filesystem::path &path) {
            auto file = OpenInputFile(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                throw std::runtime_error("Unable to open file for reading: " + path.string());
            }
            const std::streamoff size = file.tellg();
            if (size < 0 || static_cast<std::uintmax_t>(size) > std::string{}.max_size() ||
                size > std::numeric_limits<std::streamsize>::max()) {
                throw std::runtime_error("Invalid file size: " + path.string());
            }
            std::string contents(static_cast<std::size_t>(size), '\0');
            file.seekg(0);
            if (size > 0) file.read(contents.data(), static_cast<std::streamsize>(size));
            if (!file || file.gcount() != size) {
                throw std::runtime_error("Failed reading complete file: " + path.string());
            }
            return contents;
        }

        void WriteFile(const std::filesystem::path &path, const std::string_view contents) {
            if (contents.size() > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
                throw std::out_of_range("File contents exceed the supported stream size.");
            }
            auto file = OpenOutputFile(path, std::ios::binary | std::ios::trunc);
            if (!file.is_open()) {
                throw std::runtime_error("Unable to open file for writing: " + path.string());
            }
            if (!contents.empty()) file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            file.flush();
            if (!file) {
                throw std::runtime_error("Failed writing file: " + path.string());
            }
            file.close();
            if (file.fail()) {
                throw std::runtime_error("Failed closing file: " + path.string());
            }
        }

        void WriteFileAtomically(const std::filesystem::path &path, const std::string_view contents) {
            if (!path.parent_path().empty()) CreateDirectory(path.parent_path());
            struct stat destination{};
            const bool destinationExists = ::stat(path.c_str(), &destination) == 0;
            if (!destinationExists && errno != ENOENT) {
                throw std::system_error(errno, std::generic_category(), "Unable to inspect destination: " + path.string());
            }
            // A short, independent name also works when the destination name is near NAME_MAX.
            const std::string pattern = (path.parent_path() / ".sonny-XXXXXX").string();
            std::vector<char> temporaryName(pattern.begin(), pattern.end());
            temporaryName.push_back('\0');
            // Set close-on-exec at creation so a concurrent fork/exec cannot inherit the descriptor.
            int descriptor = ::mkostemp(temporaryName.data(), O_CLOEXEC);
            if (descriptor < 0) {
                throw std::system_error(errno, std::generic_category(), "Unable to stage file: " + path.string());
            }
            try {
                std::size_t offset = 0;
                while (offset < contents.size()) {
                    const std::size_t count = std::min(contents.size() - offset,
                        static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
                    const ssize_t written = ::write(descriptor, contents.data() + offset, count);
                    if (written < 0 && errno == EINTR) continue;
                    if (written <= 0) {
                        throw std::system_error(written == 0 ? EIO : errno, std::generic_category(), "Failed staging file: " + path.string());
                    }
                    offset += static_cast<std::size_t>(written);
                }
                // New files remain private (0600); replacements retain the existing permission bits.
                if (destinationExists && ::fchmod(descriptor, destination.st_mode & 07777) != 0) {
                    throw std::system_error(errno, std::generic_category(), "Unable to preserve file permissions: " + path.string());
                }
                const int closeResult = ::close(descriptor);
                descriptor = -1;
                if (closeResult != 0) {
                    throw std::system_error(errno, std::generic_category(), "Failed closing staged file: " + path.string());
                }
                // Never remove the destination as a fallback: a failed rename must preserve it.
                RenameFile(temporaryName.data(), path);
            }
            catch (...) {
                if (descriptor >= 0) ::close(descriptor);
                ::unlink(temporaryName.data());
                throw;
            }
        }

        // Settings

        /**
         * @brief Save the current settings to disk.
         *
         */
        void Settings::Save() {
            // create_directories returns false when the directory already exists; that is success.
            CreateDirectory(SETTINGS_BASE_PATH);

            json.Set("START_DATE_MS", START_DATE.GetTimestampMilliseconds());
            json.Set("END_DATE_MS", END_DATE.GetTimestampMilliseconds());
            json.Erase("START_DATE");
            json.Erase("END_DATE");

            WriteFileAtomically(SettingsFilePath(), json.ToString());
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

            // Migrate legacy files whose unsuffixed timestamp fields contain whole seconds.
            if (json.TryGet("START_DATE_MS", startTimestamp)) {
                START_DATE = Common::DateTime::FromTimestampMilliseconds(startTimestamp, Common::TimeZone::NewYork);
            }
            else if (json.TryGet("START_DATE", startTimestamp)) {
                START_DATE = Common::DateTime(startTimestamp, true);
            }
            if (json.TryGet("END_DATE_MS", endTimestamp)) {
                END_DATE = Common::DateTime::FromTimestampMilliseconds(endTimestamp, Common::TimeZone::NewYork);
            }
            else if (json.TryGet("END_DATE", endTimestamp)) {
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
            return json.Contains(key);
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
