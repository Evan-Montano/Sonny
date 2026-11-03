// runtime.cpp
// Implementation of the Orchestrator class and the runtime entry points started from the UI.

#include "runtime.hpp"
#include "client/webullstream.hpp"
#include "../ui/ui.hpp"
#include "../core/transformer.hpp"
#include "core/engine.hpp"
#include "core/structures.hpp"
#include "../common/storage.hpp"

#include <ftxui/dom/elements.hpp>

#include <deque>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>

namespace Runtime {

    // ========== ORCHESTRATOR IMPLEMENTATION ========== //

    std::optional<std::size_t> GetSecondsSinceMarketOpen(const std::int64_t timestampMs) {
        // TODO
        return 1;
    }

    std::string GetHalfHourIndexName(const std::size_t segment) {
        constexpr std::size_t MARKET_OPEN_MINUTES = 9 * 60 + 30;
        const std::size_t startMinutes = MARKET_OPEN_MINUTES + segment * 30;
        const std::size_t endMinutes = startMinutes + 30;

        return std::format(
            "{:02}{:02}-{:02}{:02}",
            startMinutes / 60,
            startMinutes % 60,
            endMinutes / 60,
            endMinutes % 60
        );
    }

    bool HalfHourIndexFilesExist(const std::size_t segment) {
        const std::string name = GetHalfHourIndexName(segment);
        const auto indexPath = Common::Storage::INDEXES_BASE_PATH / (name + ".index");
        const auto metadataPath = Common::Storage::METADATA_BASE_PATH / (name + ".meta");

        // HalfHourIndex creates an empty index when its file is missing. Avoid
        // accidentally creating/saving an empty index during live prediction.
        return std::filesystem::exists(indexPath) && std::filesystem::exists(metadataPath);
    }

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

        struct QueuedMLRecord {
            Core::MLRecord record;
            std::int64_t timestamp_ms;
        };

        std::mutex m_quote;
        std::deque<QueuedMLRecord> quoteQueue{};
        std::uint64_t quoteQueueRevision = 0;

        // One-second aggregation state.
        std::optional<std::int64_t> activeSecond{};
        std::optional<Client::WebullQuote> activeSecondQuote{};

        std::optional<Client::WebullQuote> previousSecondQuote{};
        std::optional<QueuedMLRecord> previousSecondRecord{};

        // Maximum number of consecutive missing seconds we will synthesize.
        // Six or more missing seconds invalidate the entire rolling window.
        constexpr std::int64_t MAX_MISSING_SECONDS = 5;

        // ========== QUOTE STREAM QUEUE ========== //

        auto EnqueueRecord = [&](QueuedMLRecord record) {
            std::lock_guard<std::mutex> lock(m_quote);

            if (quoteQueue.size() >= 30) quoteQueue.pop_front();
            quoteQueue.push_back(std::move(record));
            ++quoteQueueRevision;
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
                        previousSecondRecord = { *record, quote.timestamp_ms};
                        EnqueueRecord({ std::move(*record), quote.timestamp_ms });
                        
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
                    QueuedMLRecord duplicate = *previousSecondRecord;
                    duplicate.timestamp_ms += (i + 1) * 1000;

                    EnqueueRecord(std::move(duplicate));
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
                        ++quoteQueueRevision;
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
        // Figure out which HalfHourIndex that is necessary to be loaded in based on the number of seconds passed since the market start. 
        // The loop is going to be (either periodically or for every single iteration in the loop) 
        // checking if a new index should be loaded in based on the same metric.

        // Checks under a mutex to make sure that the quoteQueue has 30 elements. It can have less, but will never have more.

        // If the above condition passes, we save a snapshot of the MLRecord quoteQueue into a vector.
        // At this stage we release the lock on the queue mutex so we can process without blocking the data stream.

        // We must then pass the vector through a normalization function provided by the index.
        // Pass the normalized vector into the HalfHourIndex query method and complete the prediction, which is currently TODO.

        constexpr std::size_t MAX_QUERY_RESULTS = 128;
        constexpr std::size_t SECONDS_PER_HALF_HOUR = 1800;

        std::uint64_t lastProcessedRevision = 0;
        std::optional<std::size_t> activeSegment{};
        std::unique_ptr<Core::HalfHourIndex> activeIndex{};

        while (this->Running.load()) {
            std::vector<Core::MLRecord> queryRecords{};
            std::int64_t queryTimestampMs = 0;

            // Take a copy while holding the mutex, then do all processing unlocked
            {
                std::lock_guard<std::mutex> lock(m_quote);

                if (quoteQueue.size() == 30 && quoteQueueRevision != lastProcessedRevision) {
                    queryRecords.reserve(quoteQueue.size());

                    for (const QueuedMLRecord &queuedRecord : quoteQueue) {
                        queryRecords.push_back(queuedRecord.record);
                    }

                    queryTimestampMs = quoteQueue.back().timestamp_ms;
                    lastProcessedRevision = quoteQueueRevision;
                }
            }

            if (queryRecords.size() == 30) {
                const std::optional<std::size_t> secondsSinceOpen = GetSecondsSinceMarketOpen(queryTimestampMs);

                if (secondsSinceOpen.has_value()) {
                    const std::size_t segment = *secondsSinceOpen / SECONDS_PER_HALF_HOUR;

                    // Load a new index only when the live quote enters another half-hour segment.
                    if (activeSegment != segment) {
                        activeIndex.reset();
                        activeSegment = segment;

                        if (HalfHourIndexFilesExist(segment)) {
                            activeIndex = std::make_unique<Core::HalfHourIndex>(*secondsSinceOpen);
                        }
                    }

                    if (activeIndex != nullptr) {
                        std::vector<float> queryVector = activeIndex->NormalizeQuery(queryRecords);
                        [[maybe_unused]] const std::vector<Core::HalfHourIndex::QueryResult> queryResults = 
                            activeIndex->Query(queryVector, MAX_QUERY_RESULTS);
                        
                        const Core::HalfHourIndex::QueryResult &result = queryResults[0];

                        // Convert YYYYMMDD into YYYY-MM-DD.
                        std::string date(result.tradingDayId.data(), result.tradingDayId.size());
                        date.insert(4, "-");
                        date.insert(7, "-");

                        const std::filesystem::path recordPath = Common::Storage::RECORDS_BASE_PATH / (date + ".rec");

                        // Open the historical record file.
                        std::ifstream file(recordPath, std::ios::binary);
                        if (!file) {
                            throw std::runtime_error("Unable to open historical record file.");
                        }

                        // Navigate to the beginning of the matching 30-record window.
                        const std::streamoff byteOffset = static_cast<std::streamoff>(result.recordIndex) * sizeof(Core::MLRecord);
                        file.seekg(byteOffset);

                        // Read the original 30 records.
                        std::array<Core::MLRecord, 30> historicalWindow{};
                        file.read(reinterpret_cast<char*>(historicalWindow.data()), sizeof(historicalWindow));

                        if (!file) {
                            throw std::runtime_error("Failed to read historical window.");
                        }
                    }
                }
            }
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