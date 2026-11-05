// liveinputwindow.cpp
// Implementation of the shared rolling input window.

#include "liveinputwindow.hpp"
#include "common/logger.hpp"
#include "core/inputconfig.hpp"

#include <format>

namespace Runtime {

    // ========== LIVE INPUT WINDOW ========== //

    std::size_t LiveInputWindow::Push(const LiveRecord& record) {
        std::lock_guard lock(mutex_);

        if (records_.empty() == false) {
            const std::int64_t expectedSecond = records_.back().timestamp_ms / 1000 + 1;
            const std::int64_t actualSecond = record.timestamp_ms / 1000;

            // The aggregator should already guarantee this; clearing keeps a bug there from producing misaligned queries.
            if (actualSecond != expectedSecond) {
                Logger::Warning(std::format(
                    "Live input window expected second {} but received {}; clearing the window.",
                    expectedSecond,
                    actualSecond
                ));
                records_.clear();
            }
        }

        Core::AppendInputWindowRecord(records_, record);
        ++revision_;

        return records_.size();
    }

    void LiveInputWindow::Clear() {
        std::lock_guard lock(mutex_);

        records_.clear();
        ++revision_;
    }

    std::optional<LiveInputWindow::Snapshot> LiveInputWindow::TakeSnapshotIfUpdated() {
        std::lock_guard lock(mutex_);

        if (records_.size() < Core::MAX_INPUT_WINDOW_SECONDS || revision_ == snapshot_revision_) {
            return std::nullopt;
        }

        Snapshot snapshot{};
        snapshot.records.reserve(records_.size());

        for (const LiveRecord& record : records_) {
            snapshot.records.push_back(record.record);
        }

        snapshot.last_timestamp_ms = records_.back().timestamp_ms;
        snapshot_revision_ = revision_;

        return snapshot;
    }

    // ========== END LIVE INPUT WINDOW ========== //

}
