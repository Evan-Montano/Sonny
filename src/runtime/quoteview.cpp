// quoteview.cpp
// Implementation of the live quote-stream view.

#include "quoteview.hpp"
#include "common/datetime.hpp"
#include "core/inputconfig.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace Runtime {

    namespace {

        /**
         * @brief Moves smaller than this are shown as flat. Matches the direction threshold used by the analytics.
         */
        constexpr double FLAT_THRESHOLD = 0.0005;

        ftxui::Color DirectionColor(const double delta) {
            if (delta > FLAT_THRESHOLD) return ftxui::Color::Green;
            if (delta < -FLAT_THRESHOLD) return ftxui::Color::Red;
            return ftxui::Color::GrayDark;
        }

        std::string PriceOrPlaceholder(const bool available, const double value) {
            return available ? std::format("${:.4f}", value) : "--";
        }

        std::string DeltaOrPlaceholder(const bool available, const double value) {
            return available ? std::format("${:+.4f}", value) : "waiting";
        }

        ftxui::Element RenderSparkline(const std::deque<std::pair<std::int64_t, double>>& deltas) {
            static constexpr std::array<const char*, 8> LEVELS{"·", "▁", "▂", "▃", "▄", "▅", "▆", "█"};

            if (deltas.empty()) {
                return ftxui::text("Waiting for completed one-second quotes…") | ftxui::color(ftxui::Color::GrayDark);
            }

            // Bar height is relative to the largest move on screen; colour carries the direction.
            double maxDelta = 0.0;
            for (const auto& [second, delta] : deltas) {
                maxDelta = std::max(maxDelta, std::abs(delta));
            }

            ftxui::Elements bars;
            for (const auto& [second, delta] : deltas) {
                const std::size_t level = maxDelta < FLAT_THRESHOLD
                    ? 0
                    : std::min<std::size_t>(7, static_cast<std::size_t>(std::ceil(std::abs(delta) / maxDelta * 7.0)));
                bars.push_back(ftxui::text(LEVELS[level]) | ftxui::color(DirectionColor(delta)));
            }

            return ftxui::hbox(std::move(bars));
        }

        ftxui::Element RenderWindowBar(const std::size_t windowSize) {
            std::string bar = "[";
            for (std::size_t i = 0; i < Core::LIVE_INPUT_WINDOW_SECONDS; ++i) {
                bar += i < windowSize ? "█" : "·";
            }
            bar += "]";

            return ftxui::text(bar);
        }

    }

    // ========== QUOTE VIEW STATE ========== //

    void QuoteViewState::OnQuote(const Client::WebullQuote& quote, const std::uint64_t quoteCount) {
        const bool hasTopOfBook = quote.Asks.empty() == false && quote.Bids.empty() == false;

        Status = hasTopOfBook ? "Live data receiving" : "Receiving quote updates";
        Symbol = quote.Symbol.empty() ? "SPY" : quote.Symbol;
        HasQuote = hasTopOfBook;
        BestAsk = hasTopOfBook ? quote.Asks.front().Price : 0.0;
        BestBid = hasTopOfBook ? quote.Bids.front().Price : 0.0;
        MidPrice = hasTopOfBook ? (BestAsk + BestBid) / 2.0 : 0.0;
        Spread = BestAsk - BestBid;
        LatestTimestampMs = quote.TimestampMs;
        QuoteCount = quoteCount;
    }

    void QuoteViewState::OnRecord(const LiveRecord& record, const std::size_t windowSize) {
        const double delta = record.Record.MidDelta;
        const std::int64_t second = record.TimestampMs / 1000;

        WindowSize = windowSize;
        HasDelta = true;
        LatestDelta = delta;
        SessionDelta += delta;

        // Keep exactly the seconds the longest input window can see.
        while (RecentDeltas.empty() == false &&
               second - RecentDeltas.front().first >= static_cast<std::int64_t>(Core::MAX_INPUT_WINDOW_SECONDS)) {
            RecentDeltas.pop_front();
        }
        RecentDeltas.emplace_back(second, delta);
    }

    void QuoteViewState::OnWindowReset() {
        WindowSize = 0;
        RecentDeltas.clear();
    }

    // ========== END QUOTE VIEW STATE ========== //


    // ========== QUOTE VIEW RENDERING ========== //

    ftxui::Element RenderQuoteView(const QuoteViewState& state) {
        double rollingDelta = 0.0;
        for (const auto& [second, delta] : state.RecentDeltas) {
            rollingDelta += delta;
        }

        const std::string quoteTime = state.HasQuote
            ? Common::DateTime::FromTimestampMilliseconds(state.LatestTimestampMs).ToString_Time()
            : "--:--:--";

        const ftxui::Element bookPanel = ftxui::window(
            ftxui::text("TOP OF BOOK") | ftxui::bold,
            ftxui::vbox({
                ftxui::text(std::format("Bid     {}", PriceOrPlaceholder(state.HasQuote, state.BestBid))),
                ftxui::text(std::format("Ask     {}", PriceOrPlaceholder(state.HasQuote, state.BestAsk))),
                ftxui::text(std::format("Mid     {}", PriceOrPlaceholder(state.HasQuote, state.MidPrice))) | ftxui::bold,
                ftxui::text(std::format("Spread  {}", PriceOrPlaceholder(state.HasQuote, state.Spread))),
                ftxui::text(std::format("Last quote  {}", quoteTime))
            })
        ) | ftxui::flex;

        const ftxui::Element movementPanel = ftxui::window(
            ftxui::text("PRICE MOVEMENT") | ftxui::bold,
            ftxui::vbox({
                ftxui::text(std::format("Last 1s Δ   {}", DeltaOrPlaceholder(state.HasDelta, state.LatestDelta)))
                    | ftxui::color(DirectionColor(state.LatestDelta)) | ftxui::bold,
                ftxui::text(std::format("Session Δ  {}", DeltaOrPlaceholder(state.HasDelta, state.SessionDelta)))
                    | ftxui::color(DirectionColor(state.SessionDelta)),
                ftxui::text(std::format(
                    "Last {}s Δ {}  ({} obs)",
                    Core::MAX_INPUT_WINDOW_SECONDS,
                    DeltaOrPlaceholder(state.HasDelta, rollingDelta),
                    state.RecentDeltas.size()
                )) | ftxui::color(DirectionColor(rollingDelta)),
                ftxui::text("Green = up   Red = down") | ftxui::color(ftxui::Color::GrayDark)
            })
        ) | ftxui::flex;

        const ftxui::Element sparklinePanel = ftxui::window(
            ftxui::text("RECENT ONE-SECOND MIDPOINT CHANGES  (oldest → newest)"),
            ftxui::vbox({
                RenderSparkline(state.RecentDeltas),
                ftxui::hbox({
                    ftxui::text("−") | ftxui::color(ftxui::Color::Red),
                    ftxui::text(" down   "),
                    ftxui::text("+") | ftxui::color(ftxui::Color::Green),
                    ftxui::text(" up")
                })
            })
        ) | ftxui::flex;

        const ftxui::Element windowPanel = ftxui::window(
            ftxui::text("ROLLING INPUT WINDOW") | ftxui::bold,
            ftxui::vbox({
                RenderWindowBar(state.WindowSize),
                ftxui::text(std::format(
                    "{} / {} one-second records ready for comparison",
                    state.WindowSize,
                    Core::LIVE_INPUT_WINDOW_SECONDS
                )),
                ftxui::text("Press Escape to return to the menu.") | ftxui::color(ftxui::Color::GrayDark)
            })
        );

        return ftxui::window(
            ftxui::text(std::format("WEBULL  /  {}  /  {}", state.Symbol, state.Environment)) | ftxui::bold,
            ftxui::vbox({
                ftxui::hbox({
                    ftxui::text("●  " + state.Status)
                        | ftxui::color(state.HasQuote ? ftxui::Color::Green : ftxui::Color::Yellow)
                        | ftxui::bold,
                    ftxui::filler(),
                    ftxui::text(std::format("{} quote updates", state.QuoteCount))
                }),
                ftxui::separator(),
                ftxui::hbox({bookPanel, movementPanel}),
                sparklinePanel,
                windowPanel
            })
        ) | ftxui::flex;
    }

    // ========== END QUOTE VIEW RENDERING ========== //

}
