// runtime.cpp
// Implementation of the Orchestrator class and the runtime entry points started from the UI.

#include "runtime.hpp"
#include "client/webullstream.hpp"
#include "../ui/ui.hpp"
#include "../core/transformer.hpp"
#include "core/structures.hpp"

#include <ftxui/dom/elements.hpp>

#include <deque>
#include <mutex>
#include <optional>

namespace Runtime {

    // ========== ORCHESTRATOR IMPLEMENTATION ========== //

    // === CONSTRUCTOR === //
    Orchestrator::Orchestrator(const Client::WebullEnvironment environment) 
    : EnvironmentType(environment) {}

    Orchestrator::~Orchestrator() {
        this->QuoteClient.RequestStop();
    }
    // === END CONSTRUCTOR === //

    // === METHODS === //
    void Orchestrator::Run() {
        this->Running.store(true);

        auto view = UI::Get().PushView(
            ftxui::CatchEvent(
                ftxui::Renderer([] {
                    return ftxui::window(
                        ftxui::text("Sandbox Webull Quote Stream") | ftxui::bold,
                        ftxui::vbox({
                            ftxui::text("Environment: Sandbox"),
                            ftxui::text("Status: Running"),
                            ftxui::text("Press Escape to return.")
                        })
                    );
                }),
                [this](ftxui::Event event) {
                    if (event == ftxui::Event::Escape) {
                        this->Running.store(false);
                        return true;
                    }

                    return false;
                }
            )
        );

        // This section is responsible for the stream and prediction pipeline. Rough draft of the process:
        // In a live runtime loop, the following is occuring continuously:
        // 1. Read, parse, and queue incoming quote ticks (separate thread to update UI quote stream display)
        // 2. Building one-second quote records
        // 3. Normalizing one-second quote records
        // 4. When >= 30 size vector is built and normalized, pass as a query vector into the appropriate HalfHourIndex
        // 5. Get results from the index query and pass into separate function for historical record lookup.
        // 6. Get results from historical lookup and calculate futures.
        // 7. Update futures display on UI.

        std::mutex m_quote;
        std::deque<Core::MLRecord> quoteQueue{};

        // One-second aggregation state.
        std::optional<std::int64_t> activeSecond{};
        std::optional<Client::WebullQuote> activeSecondQuote{};

        std::optional<Client::WebullQuote> previousSecondQuote{};
        std::optional<Core::MLRecord> previousSecondRecord{};

        // Maximum number of consecutive missing seconds we will synthesize.
        // Six or more missing seconds invalidate the entire rolling window.
        constexpr std::int64_t MAX_MISSING_SECONDS = 5;

        // ========== QUOTE STREAM QUEUE ========== //

        auto EnqueueRecord = [&](Core::MLRecord record) {
            std::lock_guard<std::mutex> lock(m_quote);

            if (quoteQueue.size() >= 30) quoteQueue.pop_front();
            quoteQueue.push_back(std::move(record));
        };

        // Finalize the last snapshot received during a completed second.
        auto FinalizeSecond = [&](const Client::WebullQuote &quote) {
            bool valid = 
                quote.asks.empty() == false &&
                quote.bids.empty() == false;
            
            if (valid) {
                if (previousSecondQuote.has_value()) {
                    std::optional<Core::MLRecord> record = Core::WebullQuoteToMLRecord(*previousSecondQuote, quote);
                    if (record.has_value()) {
                        previousSecondRecord = *record;
                        EnqueueRecord(std::move(*record));
                        
                        previousSecondQuote = quote;
                    }
                    else {
                        previousSecondQuote.reset();
                        previousSecondRecord.reset();
                    }
                }
                else {
                    // Establish the initial reference snapshot.
                    // No MLRecord can be produced until another snapshot arrives.
                    previousSecondQuote = quote;
                }
            }
            else {
                previousSecondQuote.reset();
                previousSecondRecord.reset();
            }
        };

        // Fill missing seconds using the most recently generated MLRecord.
        // If no MLRecord exists yet, there is nothing to duplicate.
        auto DuplicateMissingSeconds = [&](std::int64_t missingSeconds) {
            if (previousSecondRecord.has_value()) {
                for (std::int64_t i = 0; i < missingSeconds; ++i) {
                    EnqueueRecord(*previousSecondRecord);
                }
            }
        };

        this->QuoteClient.SetQuoteStreamCallback([&](Client::WebullQuote quote) {
            const std::int64_t quoteSecond = quote.timestamp_ms / 1000;

            if (activeSecond.has_value() == false) {
                // First quote establishes the initial aggregation bucket.
                activeSecond = quoteSecond;
                activeSecondQuote = std::move(quote);
            }
            else if (quoteSecond < *activeSecond) {
                // Ignore quotes belonging to an earlier second. In the future I may implement an "updating" previous MLRecords.
            }
            else if (quoteSecond == *activeSecond) {
                // Retain the latest snapshot within the active second
                if (activeSecondQuote.has_value() == false ||
                    quote.timestamp_ms >= activeSecondQuote->timestamp_ms) {
                    activeSecondQuote = std::move(quote);
                }
            }
            else {
                const std::int64_t missingSeconds = quoteSecond - *activeSecond - 1;

                if (missingSeconds > MAX_MISSING_SECONDS) {
                    // Long interruption: discard the entire rolling window.
                    {
                        std::lock_guard<std::mutex> lock(m_quote);
                        quoteQueue.clear();
                    }

                    // Discard the old aggregation beseline as well.
                    previousSecondQuote.reset();
                    previousSecondRecord.reset();

                    // Establish the new aggregation baseline.
                    activeSecond = quoteSecond;
                    activeSecondQuote = std::move(quote);
                }
                else {
                    // Finalize the previous second before filling any gaps.
                    if (activeSecondQuote.has_value()) {
                        FinalizeSecond(*activeSecondQuote);
                    }

                    // Duplicate the most recent MLRecord for each missing second.
                    DuplicateMissingSeconds(missingSeconds);

                    // Begin aggregating the newly received second.
                    activeSecond = quoteSecond;
                    activeSecondQuote = std::move(quote);
                }
            }
        });
        // ========== END QUOTE STREAM QUEUE ========== //

        // Start quote tcp connection and begin data stream.
        this->QuoteClient.Start(this->EnvironmentType);
        UI::Get().Refresh();

        // ========== PREDICTION LOOP ========== //
        while (this->Running.load()) {
            // TODO
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // ========== END PREDICTION LOOP ========== //

        this->QuoteClient.RequestStop();
    }

    // ========== END ORCHESTRATOR IMPLEMENTATION ========== //
    
    void BeginSandboxRuntime() {
        Orchestrator orchestrator(Client::WebullEnvironment::Sandbox);
        orchestrator.Run();
    }   

}