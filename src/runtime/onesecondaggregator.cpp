// onesecondaggregator.cpp
// Implementation of the quote-to-one-second-record aggregator.

#include "onesecondaggregator.hpp"
#include "core/transformer.hpp"

#include <utility>

namespace Runtime {

    // ========== ONE SECOND AGGREGATOR ========== //

    OneSecondAggregator::OneSecondAggregator(RecordCallback onRecord, ResetCallback onReset)
        : OnRecord(std::move(onRecord)), OnReset(std::move(onReset)) {}

    std::int64_t OneSecondAggregator::SecondOf(const Client::WebullQuote& quote) {
        return quote.TimestampMs / 1000;
    }

    void OneSecondAggregator::AddQuote(Client::WebullQuote quote) {
        if (ActiveQuote.has_value() == false) {
            ActiveQuote = std::move(quote);
            return;
        }

        const std::int64_t activeSecond = SecondOf(*ActiveQuote);
        const std::int64_t quoteSecond = SecondOf(quote);

        if (quoteSecond < activeSecond) {
            // That second's record may already be emitted; late quotes are dropped rather than revising history.
            return;
        }

        if (quoteSecond == activeSecond) {
            // Keep the newest snapshot: historical candles use each second's closing quote.
            if (quote.TimestampMs >= ActiveQuote->TimestampMs) {
                ActiveQuote = std::move(quote);
            }
            return;
        }

        // A later second has started, so the active second is complete.
        const std::int64_t quietSeconds = quoteSecond - activeSecond - 1;

        if (quietSeconds > MAX_CARRIED_FORWARD_SECONDS) {
            ResetBaseline();
        }
        else {
            FinalizeSecond(*ActiveQuote);
            CarryForward(activeSecond + 1, quietSeconds);
        }

        ActiveQuote = std::move(quote);
    }

    void OneSecondAggregator::FinalizeSecond(const Client::WebullQuote& quote) {
        // Without both sides of the book there is no midpoint, so the price series is broken here.
        if (quote.Asks.empty() || quote.Bids.empty()) {
            ResetBaseline();
            return;
        }

        // The first usable second becomes the baseline; there is nothing to diff it against yet.
        if (BaselineQuote.has_value() == false) {
            BaselineQuote = quote;
            return;
        }

        const std::optional<Core::MLRecord> record = Core::WebullQuoteToMLRecord(*BaselineQuote, quote);

        if (record.has_value() == false) {
            ResetBaseline();
            return;
        }

        BaselineQuote = quote;
        OnRecord(LiveRecord{
            .Record = *record,
            .TimestampMs = quote.TimestampMs,
            .Observed = true
        });
    }

    void OneSecondAggregator::CarryForward(const std::int64_t firstSecond, const std::int64_t count) {
        if (BaselineQuote.has_value() == false) {
            return;
        }

        // Zero deltas match how the .rec export fills quiet seconds. The next observed second then
        // carries the full move since the baseline, so the cumulative price path stays exact.
        for (std::int64_t i = 0; i < count; ++i) {
            OnRecord(LiveRecord{
                .Record = Core::MLRecord{.MidDelta = 0.0f},
                .TimestampMs = (firstSecond + i) * 1000,
                .Observed = false
            });
        }
    }

    void OneSecondAggregator::ResetBaseline() {
        BaselineQuote.reset();
        OnReset();
    }

    // ========== END ONE SECOND AGGREGATOR ========== //

}
