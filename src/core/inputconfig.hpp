// inputconfig.hpp
// Shared configuration for the historical input window: how many one-second records make up a query.

#pragma once

#include <cstddef>
#include <array>
#include <deque>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Core {

    /**
     * @brief Every input-window length (in seconds) the runtime compares side by side.
     * Each length has its own set of indexes under storage/indexes/<N>s/.
     */
    inline constexpr auto INPUT_WINDOW_CONFIGURATIONS = std::to_array<std::size_t>({
        20, 30, 45
    });

    /**
     * @brief Longest configured window, used as the live buffer size when every window is active.
     */
    inline constexpr std::size_t MAX_INPUT_WINDOW_SECONDS = INPUT_WINDOW_CONFIGURATIONS.back();

    /**
     * @brief Default window length for HalfHourIndex constructors that omit a size.
     * "Update Indexes" builds the windows returned by ActiveInputWindows().
     */
    inline constexpr std::size_t DEFAULT_INPUT_WINDOW_SECONDS = 30;

    constexpr bool IsSupportedInputWindow(const std::size_t inputWindowSeconds) {
        for (const std::size_t configuredWindow : INPUT_WINDOW_CONFIGURATIONS) {
            if (configuredWindow == inputWindowSeconds) return true;
        }

        return false;
    }

    static_assert(
        IsSupportedInputWindow(DEFAULT_INPUT_WINDOW_SECONDS),
        "DEFAULT_INPUT_WINDOW_SECONDS must be an entry in INPUT_WINDOW_CONFIGURATIONS."
    );

    /**
     * @brief When true, the live runtime searches and forecasts with every entry in
     * INPUT_WINDOW_CONFIGURATIONS, each on its own. When false, only SELECTED_INPUT_WINDOW_SECONDS runs.
     *
     * Each active window keeps up to its configured segment limit of half-hour indexes in memory
     * (magnitude and direction). The runtime defaults to one segment per window when all windows
     * are active. "Update Indexes" builds exactly the windows this setting makes active, so the
     * builder and the runtime always agree on which windows have indexes.
     */
    inline constexpr bool USE_ALL_INPUT_WINDOWS = false;

    /**
     * @brief The one input window the live runtime uses while USE_ALL_INPUT_WINDOWS is false.
     * Must be an entry in INPUT_WINDOW_CONFIGURATIONS.
     */
    inline constexpr std::size_t SELECTED_INPUT_WINDOW_SECONDS = DEFAULT_INPUT_WINDOW_SECONDS;

    static_assert(
        IsSupportedInputWindow(SELECTED_INPUT_WINDOW_SECONDS),
        "SELECTED_INPUT_WINDOW_SECONDS must be an entry in INPUT_WINDOW_CONFIGURATIONS."
    );

    /**
     * @brief Number of recent records the live runtime needs to retain for its active windows.
     * All-window mode needs the longest configured window; single-window mode needs only its selection.
     */
    inline constexpr std::size_t LIVE_INPUT_WINDOW_SECONDS =
        USE_ALL_INPUT_WINDOWS ? MAX_INPUT_WINDOW_SECONDS : SELECTED_INPUT_WINDOW_SECONDS;

    /**
     * @brief Configured maximum of half-hour segments kept per input window (1 to 13).
     *
     * Single-window runtime mode uses this limit. All-window mode defaults to one segment per window.
     *
     * A session has 13 segments. When fewer are allowed, the ones nearest the current time of day
     * are loaded, and the set follows the clock as the session moves on: in the last hour with a
     * limit of 8, the 8 segments from 12:00 to the close are searched.
     *
     * Each index stores every overlapping window in full, so memory grows with the input window.
     * With the current corpus (about 4.09 million windows per segment) one segment costs roughly
     * 34 MB per second of input window for its two indexes and their metadata:
     * about 1.6 GB at 45s, 1.1 GB at 30s, and 0.7 GB at 20s. Multiply by the selected per-window
     * limit and the number of active windows to get the total. Values above 13 behave as 13.
     */
    inline constexpr std::size_t MAX_LOADED_SEGMENTS = 3;

    /**
     * @brief Runtime's default per-window segment limit for the current input-window mode.
     * All-window mode uses one segment per window to keep aggregate memory use down.
     */
    inline constexpr std::size_t DEFAULT_ACTIVE_MAX_LOADED_SEGMENTS =
        USE_ALL_INPUT_WINDOWS ? 1 : MAX_LOADED_SEGMENTS;

    static_assert(MAX_LOADED_SEGMENTS >= 1, "MAX_LOADED_SEGMENTS must allow at least one segment.");

    /**
     * @brief Resolves the input-window settings above into the list of input windows to run.
     * Windows are never combined: each one returned gets its own search, ranking, and forecast.
     *
     * @param useAllInputWindows True to return every configured window, in configuration order.
     * @param selectedInputWindowSeconds The single window returned otherwise.
     * @throws std::invalid_argument If a single window is requested that is not configured.
     */
    inline std::vector<std::size_t> SelectInputWindows(
        const bool useAllInputWindows,
        const std::size_t selectedInputWindowSeconds
    ) {
        if (useAllInputWindows) {
            return {INPUT_WINDOW_CONFIGURATIONS.begin(), INPUT_WINDOW_CONFIGURATIONS.end()};
        }

        if (IsSupportedInputWindow(selectedInputWindowSeconds) == false) {
            throw std::invalid_argument("The selected input window is not a configured input window.");
        }

        return {selectedInputWindowSeconds};
    }

    /**
     * @brief The input windows the live runtime runs and "Update Indexes" builds, as set by
     * USE_ALL_INPUT_WINDOWS and SELECTED_INPUT_WINDOW_SECONDS.
     */
    inline std::vector<std::size_t> ActiveInputWindows() {
        return SelectInputWindows(USE_ALL_INPUT_WINDOWS, SELECTED_INPUT_WINDOW_SECONDS);
    }

    /**
     * @brief Number of floats in a FAISS vector: one feature (MidDelta) per second.
     */
    constexpr std::size_t InputVectorDimension(const std::size_t inputWindowSeconds) {
        return inputWindowSeconds;
    }

    /**
     * @brief Index of the first .rec record after a matched window.
     * A window stored at recordIndex covers [recordIndex, recordIndex + inputWindowSeconds),
     * so the "future" of that match starts immediately after its last second.
     */
    constexpr std::size_t HistoricalFutureStart(
        const std::size_t recordIndex,
        const std::size_t inputWindowSeconds
    ) {
        return recordIndex + inputWindowSeconds;
    }

    /**
     * @brief Returns the newest inputWindowSeconds records from an oldest-to-newest span.
     *
     * @throws std::invalid_argument If the window is unsupported or there are too few records.
     */
    template<typename T>
    std::span<const T> LatestInputWindow(
        const std::span<const T> records,
        const std::size_t inputWindowSeconds
    ) {
        if (IsSupportedInputWindow(inputWindowSeconds) == false ||
            records.size() < inputWindowSeconds) {
            throw std::invalid_argument("The requested input window is unavailable.");
        }

        return records.last(inputWindowSeconds);
    }

    /**
     * @brief Appends a record, dropping the oldest one once LIVE_INPUT_WINDOW_SECONDS are held.
     */
    template<typename T>
    void AppendInputWindowRecord(std::deque<T>& records, T record) {
        if (records.size() >= LIVE_INPUT_WINDOW_SECONDS) {
            records.pop_front();
        }

        records.push_back(std::move(record));
    }

}
