// liveinputwindow.cpp
// Implementation of the shared rolling input window.

#include "liveinputwindow.hpp"
#include "common/logger.hpp"
#include "core/inputconfig.hpp"

#include <format>

namespace Runtime {

    // ========== LIVE INPUT WINDOW ========== //

    std::size_t LiveInputWindow::Push(const LiveRecord& record) {
        std::lock_guard lock(Mutex);

        if (Records.empty() == false) {
            const std::int64_t expectedSecond = Records.back().TimestampMs / 1000 + 1;
            const std::int64_t actualSecond = record.TimestampMs / 1000;

            // The aggregator should already guarantee this; clearing keeps a bug there from producing misaligned queries.
            if (actualSecond != expectedSecond) {
                Logger::Warning(std::format(
                    "Live input window expected second {} but received {}; clearing the window.",
                    expectedSecond,
                    actualSecond
                ));
                Records.clear();
            }
        }

        Core::AppendInputWindowRecord(Records, record);
        ++Revision;

        return Records.size();
    }

    void LiveInputWindow::Clear() {
        std::lock_guard lock(Mutex);

        Records.clear();
        ++Revision;
    }

    std::optional<LiveInputWindow::Snapshot> LiveInputWindow::TakeSnapshotIfUpdated() {
        std::lock_guard lock(Mutex);

        if (Records.size() < Core::LIVE_INPUT_WINDOW_SECONDS || Revision == SnapshotRevision) {
            return std::nullopt;
        }

        Snapshot snapshot{};
        snapshot.Records.reserve(Records.size());

        for (const LiveRecord& record : Records) {
            snapshot.Records.push_back(record.Record);
        }

        snapshot.LastTimestampMs = Records.back().TimestampMs;
        SnapshotRevision = Revision;

        return snapshot;
    }

    // ========== END LIVE INPUT WINDOW ========== //

}
