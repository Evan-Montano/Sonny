// Tracks unsuccessful daily exports so missing-only downloads do not repeat them.

#pragma once

#include "common/jsonutil.hpp"
#include "common/storage.hpp"

#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Dukascopy {

    /**
     * @brief Persists failed daily record filenames so missing-only downloads can skip them.
     */
    class FailedRecordFiles {
    public:
        // CONSTRUCTOR

        /** @brief Load the persisted failures, starting empty when the tracking file is absent. */
        explicit FailedRecordFiles(std::filesystem::path path) : Path(std::move(path)) {
            if (Common::Storage::FileExists(Path)) {
                const auto files = Common::JsonUtility::FromFile(Path).Get<std::vector<std::string>>("failed_record_files");
                Files.insert(files.begin(), files.end());
            }
        }

        // METHODS

        /** @brief Skip known failures only when keeping existing records; full downloads retry them. */
        bool ShouldSkip(const std::string& recordFile, const bool replaceExisting) const {
            return !replaceExisting && Files.contains(recordFile);
        }

        /** @brief Persist a failed daily record creation immediately. */
        void MarkFailed(const std::string& recordFile) { SetFailed(recordFile, true); }

        /** @brief Remove a recovered day from the persisted failure set. */
        void ClearFailure(const std::string& recordFile) { SetFailed(recordFile, false); }

    private:
        // MEMBERS

        /** @brief Location of the failure tracking JSON file. */
        std::filesystem::path Path;

        /** @brief Failed record filenames, deduplicated and sorted for persistence. */
        std::set<std::string> Files;

        // METHODS

        void SetFailed(const std::string& recordFile, const bool failed) {
            if (Files.contains(recordFile) == failed) return;

            // Commit to disk before updating memory, so a failed save cannot silently lose a failure.
            auto updated = Files;
            if (failed) updated.insert(recordFile);
            else updated.erase(recordFile);

            Common::JsonUtility json;
            json.Set("failed_record_files", std::vector<std::string>(updated.begin(), updated.end()));
            Common::Storage::WriteFileAtomically(Path, json.ToString());
            Files = std::move(updated);
        }
    };

}
