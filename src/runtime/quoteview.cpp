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
            for (std::size_t i = 0; i < Core::MAX_INPUT_WINDOW_SECONDS; ++i) {
                bar += i < windowSize ? "█" : "·";
            }
            bar += "]";

            return ftxui::text(bar);
        }

    }

    // ========== QUOTE VIEW STATE ========== //

    void QuoteViewState::OnQuote(const Client::WebullQuote& quote, const std::uint64_t quoteCount) {
        const bool hasTopOfBook = quote.asks.empty() == false && quote.bids.empty() == false;

        status = hasTopOfBook ? "Live data receiving" : "Receiving quote updates";
        symbol = quote.symbol.empty() ? "SPY" : quote.symbol;
        has_quote = hasTopOfBook;
        best_ask = hasTopOfBook ? quote.asks.front().Price : 0.0;
        best_bid = hasTopOfBook ? quote.bids.front().Price : 0.0;
        mid_price = hasTopOfBook ? (best_ask + best_bid) / 2.0 : 0.0;
        spread = best_ask - best_bid;
        latest_timestamp_ms = quote.timestamp_ms;
        quote_count = quoteCount;
    }

    void QuoteViewState::OnRecord(const LiveRecord& record, const std::size_t windowSize) {
        const double delta = record.record.mid_delta;
        const std::int64_t second = record.timestamp_ms / 1000;

        window_size = windowSize;
        has_delta = true;
        latest_delta = delta;
        session_delta += delta;

        // Keep exactly the seconds the longest input window can see.
        while (recent_deltas.empty() == false &&
               second - recent_deltas.front().first >= static_cast<std::int64_t>(Core::MAX_INPUT_WINDOW_SECONDS)) {
            recent_deltas.pop_front();
        }
        recent_deltas.emplace_back(second, delta);
    }

    void QuoteViewState::OnWindowReset() {
        window_size = 0;
        recent_deltas.clear();
    }

    // ========== END QUOTE VIEW STATE ========== //


    // ========== QUOTE VIEW RENDERING ========== //

    ftxui::Element RenderQuoteView(const QuoteViewState& state) {
        double rollingDelta = 0.0;
        for (const auto& [second, delta] : state.recent_deltas) {
            rollingDelta += delta;
        }

        const std::string quoteTime = state.has_quote
            ? Common::DateTime(static_cast<Common::UnixTimestamp>(state.latest_timestamp_ms / 1000)).ToString_Time()
            : "--:--:--";

        const ftxui::Element bookPanel = ftxui::window(
            ftxui::text("TOP OF BOOK") | ftxui::bold,
            ftxui::vbox({
                ftxui::text(std::format("Bid     {}", PriceOrPlaceholder(state.has_quote, state.best_bid))),
                ftxui::text(std::format("Ask     {}", PriceOrPlaceholder(state.has_quote, state.best_ask))),
                ftxui::text(std::format("Mid     {}", PriceOrPlaceholder(state.has_quote, state.mid_price))) | ftxui::bold,
                ftxui::text(std::format("Spread  {}", PriceOrPlaceholder(state.has_quote, state.spread))),
                ftxui::text(std::format("Last quote  {}", quoteTime))
            })
        ) | ftxui::flex;

        const ftxui::Element movementPanel = ftxui::window(
            ftxui::text("PRICE MOVEMENT") | ftxui::bold,
            ftxui::vbox({
                ftxui::text(std::format("Last 1s Δ   {}", DeltaOrPlaceholder(state.has_delta, state.latest_delta)))
                    | ftxui::color(DirectionColor(state.latest_delta)) | ftxui::bold,
                ftxui::text(std::format("Session Δ  {}", DeltaOrPlaceholder(state.has_delta, state.session_delta)))
                    | ftxui::color(DirectionColor(state.session_delta)),
                ftxui::text(std::format(
                    "Last {}s Δ {}  ({} obs)",
                    Core::MAX_INPUT_WINDOW_SECONDS,
                    DeltaOrPlaceholder(state.has_delta, rollingDelta),
                    state.recent_deltas.size()
                )) | ftxui::color(DirectionColor(rollingDelta)),
                ftxui::text("Green = up   Red = down") | ftxui::color(ftxui::Color::GrayDark)
            })
        ) | ftxui::flex;

        const ftxui::Element sparklinePanel = ftxui::window(
            ftxui::text("RECENT ONE-SECOND MIDPOINT CHANGES  (oldest → newest)"),
            ftxui::vbox({
                RenderSparkline(state.recent_deltas),
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
                RenderWindowBar(state.window_size),
                ftxui::text(std::format(
                    "{} / {} one-second records ready for comparison",
                    state.window_size,
                    Core::MAX_INPUT_WINDOW_SECONDS
                )),
                ftxui::text("Press Escape to return to the menu.") | ftxui::color(ftxui::Color::GrayDark)
            })
        );

        return ftxui::window(
            ftxui::text(std::format("WEBULL  /  {}  /  {}", state.symbol, state.environment)) | ftxui::bold,
            ftxui::vbox({
                ftxui::hbox({
                    ftxui::text("●  " + state.status)
                        | ftxui::color(state.has_quote ? ftxui::Color::Green : ftxui::Color::Yellow)
                        | ftxui::bold,
                    ftxui::filler(),
                    ftxui::text(std::format("{} quote updates", state.quote_count))
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
