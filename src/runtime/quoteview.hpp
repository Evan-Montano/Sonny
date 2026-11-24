// quoteview.hpp
// State and rendering for the live quote-stream view shown while the runtime is running.

#pragma once

#include "client/webullstream.hpp"
#include "onesecondaggregator.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <utility>

namespace Runtime {

    /**
     * @brief Everything the quote view displays.
     * The renderer reads this on the UI thread, so every change must happen inside UI::Get().Update.
     */
    struct QuoteViewState {
        // MEMBERS

        /**
         * @brief Label for the Webull environment shown in the title bar.
         */
        std::string Environment = "SANDBOX";

        std::string Status = "Waiting for market data";
        std::string Symbol = "SPY";

        /**
         * @brief True once a quote with both sides of the book has arrived.
         */
        bool HasQuote = false;

        /**
         * @brief True once at least one one-second record has been produced.
         */
        bool HasDelta = false;

        double BestBid = 0.0;
        double BestAsk = 0.0;
        double MidPrice = 0.0;
        double Spread = 0.0;
        double LatestDelta = 0.0;

        /**
         * @brief Sum of every one-second midpoint change since the view opened.
         */
        double SessionDelta = 0.0;

        std::int64_t LatestTimestampMs = 0;
        std::uint64_t QuoteCount = 0;

        /**
         * @brief Records currently held by the live input window.
         */
        std::size_t WindowSize = 0;

        /**
         * @brief (Unix second, midpoint change) pairs covering the last MAX_INPUT_WINDOW_SECONDS.
         */
        std::deque<std::pair<std::int64_t, double>> RecentDeltas;

        // METHODS

        /**
         * @brief Refresh the top-of-book panel from a raw quote.
         *
         * @param quote Latest quote from the stream.
         * @param quoteCount Total quotes received so far.
         */
        void OnQuote(const Client::WebullQuote& quote, std::uint64_t quoteCount);

        /**
         * @brief Add a completed second to the movement panels.
         *
         * @param record Record produced by the aggregator.
         * @param windowSize Records now held by the live input window.
         */
        void OnRecord(const LiveRecord& record, std::size_t windowSize);

        /**
         * @brief Show that the live input window was discarded and is refilling.
         */
        void OnWindowReset();
    };

    /**
     * @brief Draw the quote view.
     *
     * @param state Current view state.
     * @return The complete view element.
     */
    ftxui::Element RenderQuoteView(const QuoteViewState& state);

}
