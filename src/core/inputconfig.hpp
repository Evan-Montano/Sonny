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
    inline constexpr std::array<std::size_t, 6> INPUT_WINDOW_CONFIGURATIONS{
        // 10, 15, 20, 30, 45, 60
        10, 15, 20, 30, 45, 60
    };

    /**
     * @brief Longest configured window. The live rolling window keeps this many records so
     * every shorter configuration can be cut from the same snapshot.
     */
    inline constexpr std::size_t MAX_INPUT_WINDOW_SECONDS = INPUT_WINDOW_CONFIGURATIONS.back();

    /**
     * @brief Default window length for HalfHourIndex constructors that omit a size.
     * "Update Indexes" builds every entry in INPUT_WINDOW_CONFIGURATIONS.
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
        "INPUT_WINDOW_SECONDS must be 10, 15, 20, 30, 45, or 60."
    );

    /**
     * @brief When true, the live runtime searches and forecasts with every entry in
     * INPUT_WINDOW_CONFIGURATIONS, each on its own. When false, only SELECTED_INPUT_WINDOW_SECONDS runs.
     *
     * Each active window keeps all 13 of its half-hour indexes in memory (magnitude and direction),
     * so running every window at once needs several times the memory of a single one.
     * "Update Indexes" always builds every configured window regardless of this setting.
     */
    inline constexpr bool USE_ALL_INPUT_WINDOWS = false;

    /**
     * @brief The one input window the live runtime uses while USE_ALL_INPUT_WINDOWS is false.
     * Must be an entry in INPUT_WINDOW_CONFIGURATIONS.
     */
    inline constexpr std::size_t SELECTED_INPUT_WINDOW_SECONDS = 60;

    static_assert(
        IsSupportedInputWindow(SELECTED_INPUT_WINDOW_SECONDS),
        "SELECTED_INPUT_WINDOW_SECONDS must be an entry in INPUT_WINDOW_CONFIGURATIONS."
    );

    /**
     * @brief Resolves the two settings above into the list of input windows to run.
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
     * @brief The input windows the live runtime runs, as set by USE_ALL_INPUT_WINDOWS and
     * SELECTED_INPUT_WINDOW_SECONDS.
     */
    inline std::vector<std::size_t> ActiveInputWindows() {
        return SelectInputWindows(USE_ALL_INPUT_WINDOWS, SELECTED_INPUT_WINDOW_SECONDS);
    }

    /**
     * @brief Number of floats in a FAISS vector: one feature (mid_delta) per second.
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
     * @brief Appends a record, dropping the oldest one once MAX_INPUT_WINDOW_SECONDS are held.
     */
    template<typename T>
    void AppendInputWindowRecord(std::deque<T>& records, T record) {
        if (records.size() >= MAX_INPUT_WINDOW_SECONDS) {
            records.pop_front();
        }

        records.push_back(std::move(record));
    }

}
