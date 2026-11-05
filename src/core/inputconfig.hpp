// Shared configuration for the historical input window.

#pragma once

#include <cstddef>
#include <array>
#include <deque>
#include <span>
#include <stdexcept>
#include <utility>

namespace Core {

    inline constexpr std::array<std::size_t, 4> INPUT_WINDOW_CONFIGURATIONS{
        10, 15, 20, 30
    };
    inline constexpr std::size_t MAX_INPUT_WINDOW_SECONDS =
        INPUT_WINDOW_CONFIGURATIONS.back();

    inline constexpr std::size_t INPUT_WINDOW_SECONDS = 30;

    constexpr bool IsSupportedInputWindow(const std::size_t inputWindowSeconds) {
        for (const std::size_t configuredWindow : INPUT_WINDOW_CONFIGURATIONS) {
            if (configuredWindow == inputWindowSeconds) return true;
        }

        return false;
    }

    static_assert(
        IsSupportedInputWindow(INPUT_WINDOW_SECONDS),
        "INPUT_WINDOW_SECONDS must be 10, 15, 20, or 30."
    );

    constexpr std::size_t InputVectorDimension(const std::size_t inputWindowSeconds) {
        return inputWindowSeconds * 2;
    }

    constexpr std::size_t HistoricalFutureStart(
        const std::size_t recordIndex,
        const std::size_t inputWindowSeconds
    ) {
        return recordIndex + inputWindowSeconds;
    }

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

    template<typename T>
    void AppendInputWindowRecord(std::deque<T>& records, T record) {
        if (records.size() >= MAX_INPUT_WINDOW_SECONDS) {
            records.pop_front();
        }

        records.push_back(std::move(record));
    }

}
