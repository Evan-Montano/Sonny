// onesecondaggregator.cpp
// Implementation of the quote-to-one-second-record aggregator.

#include "onesecondaggregator.hpp"
#include "core/transformer.hpp"

#include <utility>

namespace Runtime {

    // ========== ONE SECOND AGGREGATOR ========== //

    OneSecondAggregator::OneSecondAggregator(RecordCallback onRecord, ResetCallback onReset)
        : on_record_(std::move(onRecord)), on_reset_(std::move(onReset)) {}

    std::int64_t OneSecondAggregator::SecondOf(const Client::WebullQuote& quote) {
        return quote.timestamp_ms / 1000;
    }

    void OneSecondAggregator::AddQuote(Client::WebullQuote quote) {
        if (active_quote_.has_value() == false) {
            active_quote_ = std::move(quote);
            return;
        }

        const std::int64_t activeSecond = SecondOf(*active_quote_);
        const std::int64_t quoteSecond = SecondOf(quote);

        if (quoteSecond < activeSecond) {
            // That second's record may already be emitted; late quotes are dropped rather than revising history.
            return;
        }

        if (quoteSecond == activeSecond) {
            // Keep the newest snapshot: historical candles use each second's closing quote.
            if (quote.timestamp_ms >= active_quote_->timestamp_ms) {
                active_quote_ = std::move(quote);
            }
            return;
        }

        // A later second has started, so the active second is complete.
        const std::int64_t quietSeconds = quoteSecond - activeSecond - 1;

        if (quietSeconds > MAX_CARRIED_FORWARD_SECONDS) {
            ResetBaseline();
        }
        else {
            FinalizeSecond(*active_quote_);
            CarryForward(activeSecond + 1, quietSeconds);
        }

        active_quote_ = std::move(quote);
    }

    void OneSecondAggregator::FinalizeSecond(const Client::WebullQuote& quote) {
        // Without both sides of the book there is no midpoint, so the price series is broken here.
        if (quote.asks.empty() || quote.bids.empty()) {
            ResetBaseline();
            return;
        }

        // The first usable second becomes the baseline; there is nothing to diff it against yet.
        if (baseline_quote_.has_value() == false) {
            baseline_quote_ = quote;
            return;
        }

        const std::optional<Core::MLRecord> record = Core::WebullQuoteToMLRecord(*baseline_quote_, quote);

        if (record.has_value() == false) {
            ResetBaseline();
            return;
        }

        baseline_quote_ = quote;
        on_record_(LiveRecord{
            .record = *record,
            .timestamp_ms = quote.timestamp_ms,
            .observed = true
        });
    }

    void OneSecondAggregator::CarryForward(const std::int64_t firstSecond, const std::int64_t count) {
        if (baseline_quote_.has_value() == false) {
            return;
        }

        // Zero deltas match how the .rec export fills quiet seconds. The next observed second then
        // carries the full move since the baseline, so the cumulative price path stays exact.
        for (std::int64_t i = 0; i < count; ++i) {
            on_record_(LiveRecord{
                .record = Core::MLRecord{.mid_delta = 0.0f/*, .spread_delta = 0.0f, .volume = 0.0f*/},
                .timestamp_ms = (firstSecond + i) * 1000,
                .observed = false
            });
        }
    }

    void OneSecondAggregator::ResetBaseline() {
        baseline_quote_.reset();
        on_reset_();
    }

    // ========== END ONE SECOND AGGREGATOR ========== //

}
