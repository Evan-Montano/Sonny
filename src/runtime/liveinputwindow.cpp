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

        if (Records.size() == CAPACITY) Records.pop_front();
        Records.push_back(record);
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

        if (Records.size() < CAPACITY || Revision == SnapshotRevision) {
            return std::nullopt;
        }

        Snapshot snapshot{};
        snapshot.Records.reserve(Core::LIVE_INPUT_WINDOW_SECONDS);

        for (std::size_t i = 0; i < Core::LIVE_INPUT_WINDOW_SECONDS; ++i) {
            snapshot.Records.push_back(Records[i].Record);
        }

        snapshot.LastTimestampMs = Records[Core::LIVE_INPUT_WINDOW_SECONDS - 1].TimestampMs;
        for (std::size_t i = 0; i < snapshot.Future.size(); ++i) {
            snapshot.Future[i] = Records[Core::LIVE_INPUT_WINDOW_SECONDS + i];
        }
        SnapshotRevision = Revision;

        return snapshot;
    }

    // ========== END LIVE INPUT WINDOW ========== //

}
