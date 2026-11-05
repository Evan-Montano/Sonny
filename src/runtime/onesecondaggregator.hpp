// onesecondaggregator.hpp
// Turns the raw Webull quote stream into one MLRecord per second, using the same rules as the historical .rec export.

#pragma once

#include "client/webullstream.hpp"
#include "core/structures.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace Runtime {

    /**
     * @brief One live second of market data.
     */
    struct LiveRecord {
        /**
         * @brief Midpoint and spread change since the previous second.
         */
        Core::MLRecord record{};

        /**
         * @brief A timestamp inside the second this record describes (timestamp_ms / 1000 is that second).
         */
        std::int64_t timestamp_ms{};

        /**
         * @brief False when no quote arrived during the second and the previous price was carried forward.
         */
        bool observed = true;
    };

    /**
     * @brief Buckets quotes by second and emits a gap-free sequence of LiveRecords.
     *
     * Mirrors the Dukascopy export: each second is represented by its closing quote, and a quiet
     * second repeats the previous close (zero deltas). Whenever continuity cannot be guaranteed,
     * the reset callback fires so the consumer can discard its rolling window.
     *
     * Not thread-safe; call AddQuote from a single thread (the stream worker).
     */
    class OneSecondAggregator {
    public:
        // MEMBERS

        /**
         * @brief Longest run of quiet seconds that is carried forward. Anything longer is treated as
         * a feed interruption and resets the stream rather than inventing that much flat data.
         */
        static constexpr std::int64_t MAX_CARRIED_FORWARD_SECONDS = 5;

        using RecordCallback = std::function<void(const LiveRecord&)>;
        using ResetCallback = std::function<void()>;

        // CONSTRUCTOR

        /**
         * @brief Construct an aggregator that reports through the given callbacks.
         *
         * @param onRecord Receives each completed second, oldest to newest, with no gaps.
         * @param onReset Called when continuity is lost; records before and after a reset must not be joined.
         */
        OneSecondAggregator(RecordCallback onRecord, ResetCallback onReset);

        // METHODS

        /**
         * @brief Feed one quote from the stream.
         * A second is finalized only once a quote from a later second arrives.
         *
         * @param quote Latest quote snapshot.
         */
        void AddQuote(Client::WebullQuote quote);

    private:
        // METHODS

        /**
         * @brief Emit the record for a completed second, using its closing quote.
         */
        void FinalizeSecond(const Client::WebullQuote& quote);

        /**
         * @brief Emit zero-delta records for seconds that had no quotes.
         *
         * @param firstSecond Unix second of the first quiet second.
         * @param count Number of consecutive quiet seconds.
         */
        void CarryForward(std::int64_t firstSecond, std::int64_t count);

        /**
         * @brief Forget the price baseline and notify the consumer that continuity was lost.
         */
        void ResetBaseline();

        /**
         * @brief Unix second a quote belongs to.
         */
        static std::int64_t SecondOf(const Client::WebullQuote& quote);

        // MEMBERS

        /**
         * @brief Receives completed seconds.
         */
        RecordCallback on_record_;

        /**
         * @brief Receives continuity resets.
         */
        ResetCallback on_reset_;

        /**
         * @brief Latest quote of the second currently being collected.
         */
        std::optional<Client::WebullQuote> active_quote_;

        /**
         * @brief Closing quote of the last finalized second; deltas are measured from it.
         */
        std::optional<Client::WebullQuote> baseline_quote_;
    };

}
