// runtime.cpp
// Implementation of the Orchestrator class and the runtime entry points started from the UI.

#include "runtime.hpp"
#include "client/webullstream.hpp"
#include "../ui/ui.hpp"
#include "../core/transformer.hpp"
#include "common/datetime.hpp"
#include "core/engine.hpp"
#include "core/structures.hpp"
#include "../common/storage.hpp"
#include "predictionanalytics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <ftxui/dom/elements.hpp>

#include <deque>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace Runtime {

    // ========== ORCHESTRATOR IMPLEMENTATION ========== //

    std::optional<std::size_t> GetSecondsSinceMarketOpen(const std::int64_t timestampMs) {
        using namespace std::chrono;

        const sys_time<milliseconds> timestamp{
            milliseconds{timestampMs}
        };

        const auto* zone = locate_zone("America/New_York");

        const zoned_time<milliseconds> easternTime{
            zone,
            timestamp
        };

        const local_time<milliseconds> localTime = easternTime.get_local_time();
        const auto localDay = floor<days>(localTime);
        const year_month_day date{localDay};
        const weekday dayOfWeek{sys_days{date}};

        // Regular trading sessions occur Monday through Friday.
        if (dayOfWeek == Saturday || dayOfWeek == Sunday) {
            return std::nullopt;
        }

        constexpr std::int64_t MARKET_OPEN_SECONDS = 9 * 60 * 60 + 30 * 60;
        constexpr std::int64_t MARKET_CLOSE_SECONDS = 16 * 60 * 60;

        const std::int64_t secondsSinceMidnight = duration_cast<seconds>(localTime - localDay).count();

        // Outside regular trading hours.
        if (secondsSinceMidnight < MARKET_OPEN_SECONDS ||
            secondsSinceMidnight >= MARKET_CLOSE_SECONDS)
        {
            return std::nullopt;
        }

        return static_cast<std::size_t>(
            secondsSinceMidnight - MARKET_OPEN_SECONDS
        );
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
        const auto normalizationPath = Common::Storage::METADATA_BASE_PATH / (name + ".normalization.json");

        // HalfHourIndex creates an empty index when its file is missing. Avoid
        // accidentally creating/saving an empty index during live prediction.
        return std::filesystem::exists(indexPath) && 
            std::filesystem::exists(metadataPath) &&
            std::filesystem::exists(normalizationPath);
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

        struct QuoteStreamViewState {
            std::string status = "Waiting for market data";
            std::string symbol = "SPY";
            bool has_quote = false;
            bool has_delta = false;
            double best_bid = 0.0;
            double best_ask = 0.0;
            double mid_price = 0.0;
            double spread = 0.0;
            double latest_delta = 0.0;
            double session_delta = 0.0;
            std::int64_t latest_timestamp_ms = 0;
            std::uint64_t quote_count = 0;
            std::size_t queue_size = 0;
            std::deque<std::pair<std::int64_t, double>> recent_deltas;
        } quoteViewState;

        auto view = UI::Get().PushView(
            ftxui::CatchEvent(
                ftxui::Renderer([&quoteViewState] {
                    const ftxui::Color deltaColor = quoteViewState.latest_delta > 0.0005
                        ? ftxui::Color::Green
                        : (quoteViewState.latest_delta < -0.0005
                            ? ftxui::Color::Red : ftxui::Color::GrayDark);
                    const ftxui::Color sessionColor = quoteViewState.session_delta > 0.0005
                        ? ftxui::Color::Green
                        : (quoteViewState.session_delta < -0.0005
                            ? ftxui::Color::Red : ftxui::Color::GrayDark);

                    const std::string quoteTime = quoteViewState.has_quote
                        ? Common::DateTime(static_cast<Common::UnixTimestamp>(quoteViewState.latest_timestamp_ms / 1000)).ToString_Time()
                        : "--:--:--";

                    std::string queueBar = "[";
                    for (std::size_t i = 0; i < 30; ++i) {
                        queueBar += i < quoteViewState.queue_size ? "█" : "·";
                    }
                    queueBar += "]";

                    static constexpr std::array<const char*, 8> SparkLevels{
                        "·", "▁", "▂", "▃", "▄", "▅", "▆", "█"
                    };
                    double maxDelta = 0.0;
                    for (const auto& [timestamp, delta] : quoteViewState.recent_deltas) {
                        (void)timestamp;
                        maxDelta = std::max(maxDelta, std::abs(delta));
                    }

                    ftxui::Elements sparkline;
                    for (const auto& [timestamp, delta] : quoteViewState.recent_deltas) {
                        (void)timestamp;
                        const std::size_t level = maxDelta < 0.0005
                            ? 0
                            : std::min<std::size_t>(7, static_cast<std::size_t>(std::ceil(std::abs(delta) / maxDelta * 7.0)));
                        const ftxui::Color pointColor = delta > 0.0005
                            ? ftxui::Color::Green
                            : (delta < -0.0005 ? ftxui::Color::Red : ftxui::Color::GrayDark);
                        sparkline.push_back(ftxui::text(SparkLevels[level]) | ftxui::color(pointColor));
                    }
                    if (sparkline.empty()) {
                        sparkline.push_back(ftxui::text("Waiting for completed one-second quotes…") | ftxui::color(ftxui::Color::GrayDark));
                    }

                    double rollingDelta = 0.0;
                    for (const auto& [timestamp, delta] : quoteViewState.recent_deltas) {
                        (void)timestamp;
                        rollingDelta += delta;
                    }
                    const ftxui::Color rollingColor = rollingDelta > 0.0005
                        ? ftxui::Color::Green
                        : (rollingDelta < -0.0005
                            ? ftxui::Color::Red : ftxui::Color::GrayDark);

                    const std::string deltaText = quoteViewState.has_delta
                        ? std::format("${:+.4f}", quoteViewState.latest_delta)
                        : "waiting";
                    const std::string sessionText = quoteViewState.has_delta
                        ? std::format("${:+.4f}", quoteViewState.session_delta)
                        : "waiting";
                    const std::string rollingText = quoteViewState.has_delta
                        ? std::format("${:+.4f}", rollingDelta)
                        : "waiting";
                    const std::string bidText = quoteViewState.has_quote
                        ? std::format("${:.4f}", quoteViewState.best_bid) : "--";
                    const std::string askText = quoteViewState.has_quote
                        ? std::format("${:.4f}", quoteViewState.best_ask) : "--";
                    const std::string midText = quoteViewState.has_quote
                        ? std::format("${:.4f}", quoteViewState.mid_price) : "--";
                    const std::string spreadText = quoteViewState.has_quote
                        ? std::format("${:.4f}", quoteViewState.spread) : "--";

                    const ftxui::Element bookPanel = ftxui::window(
                        ftxui::text("TOP OF BOOK") | ftxui::bold,
                        ftxui::vbox({
                            ftxui::text(std::format("Bid     {}", bidText)),
                            ftxui::text(std::format("Ask     {}", askText)),
                            ftxui::text(std::format("Mid     {}", midText)) | ftxui::bold,
                            ftxui::text(std::format("Spread  {}", spreadText)),
                            ftxui::text(quoteViewState.has_quote
                                ? std::format("Last quote  {}", quoteTime)
                                : "Last quote  --:--:--")
                        })
                    ) | ftxui::flex;

                    const ftxui::Element movementPanel = ftxui::window(
                        ftxui::text("PRICE MOVEMENT") | ftxui::bold,
                        ftxui::vbox({
                            ftxui::text(std::format("Last 1s Δ   {}", deltaText)) | ftxui::color(deltaColor) | ftxui::bold,
                            ftxui::text(std::format("Session Δ  {}", sessionText)) | ftxui::color(sessionColor),
                            ftxui::text(std::format("Last 30s Δ {}  ({} obs)", rollingText, quoteViewState.recent_deltas.size())) | ftxui::color(rollingColor),
                            ftxui::text("Green = up   Red = down") | ftxui::color(ftxui::Color::GrayDark)
                        })
                    ) | ftxui::flex;

                    return ftxui::window(
                        ftxui::text("WEBULL  /  " + quoteViewState.symbol + "  /  SANDBOX") | ftxui::bold,
                        ftxui::vbox({
                            ftxui::hbox({
                                ftxui::text("●  " + quoteViewState.status) |
                                    ftxui::color(quoteViewState.has_quote ? ftxui::Color::Green : ftxui::Color::Yellow) |
                                    ftxui::bold,
                                ftxui::filler(),
                                ftxui::text(std::format("{} quote updates", quoteViewState.quote_count))
                            }),
                            ftxui::separator(),
                            ftxui::hbox({bookPanel, movementPanel}),
                            ftxui::window(
                                ftxui::text("RECENT ONE-SECOND MIDPOINT CHANGES  (oldest → newest)"),
                                ftxui::vbox({
                                    ftxui::hbox(std::move(sparkline)),
                                    ftxui::hbox({
                                        ftxui::text("−") | ftxui::color(ftxui::Color::Red),
                                        ftxui::text(" down   "),
                                        ftxui::text("+") | ftxui::color(ftxui::Color::Green),
                                        ftxui::text(" up")
                                    })
                                })
                            ) | ftxui::flex,
                            ftxui::window(
                                ftxui::text("ROLLING INPUT QUEUE") | ftxui::bold,
                                ftxui::vbox({
                                    ftxui::text(queueBar),
                                    ftxui::text(std::format("{} / 30 one-second records ready for prediction", quoteViewState.queue_size)),
                                    ftxui::text("Press Escape to return to the menu.") | ftxui::color(ftxui::Color::GrayDark)
                                })
                            )
                        })
                    ) | ftxui::flex;
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
            bool observed = true;
        };

        PredictionAnalytics predictionAnalytics;

        auto LogEvaluationReports = [](std::vector<PredictionEvaluationReport> reports) {
            for (const PredictionEvaluationReport& report : reports) {
                Logger::Info(PredictionAnalytics::FormatReport(report), true);
            }
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
            const std::int64_t timestamp_ms = record.timestamp_ms;
            const double mid_delta = record.record.mid_delta;
            const bool observed = record.observed;

            LogEvaluationReports(
                predictionAnalytics.ObserveRecord(timestamp_ms, mid_delta, observed)
            );

            std::size_t queueSize = 0;
            {
                std::lock_guard<std::mutex> lock(m_quote);

                if (quoteQueue.size() >= 30) quoteQueue.pop_front();
                quoteQueue.push_back(std::move(record));
                ++quoteQueueRevision;
                queueSize = quoteQueue.size();
            }

            UI::Get().Update([&] {
                quoteViewState.queue_size = queueSize;
                if (observed) {
                    quoteViewState.has_delta = true;
                    quoteViewState.latest_delta = mid_delta;
                    quoteViewState.session_delta += mid_delta;

                    const std::int64_t second = timestamp_ms / 1000;
                    while (quoteViewState.recent_deltas.empty() == false &&
                        second - quoteViewState.recent_deltas.front().first >= 30) {
                        quoteViewState.recent_deltas.pop_front();
                    }
                    quoteViewState.recent_deltas.emplace_back(second, mid_delta);
                }
            });
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
                    duplicate.observed = false;

                    EnqueueRecord(std::move(duplicate));
                }
            }
        };

        std::uint64_t rawQuoteCount = 0;
        auto lastQuoteViewRefresh = std::chrono::steady_clock::now() - std::chrono::seconds(1);

        this->QuoteClient.SetQuoteStreamCallback([&](Client::WebullQuote quote) {
            ++rawQuoteCount;

            const auto now = std::chrono::steady_clock::now();
            if (now - lastQuoteViewRefresh >= std::chrono::milliseconds(200)) {
                lastQuoteViewRefresh = now;
                const bool hasTopOfBook = quote.asks.empty() == false && quote.bids.empty() == false;
                const double bestAsk = hasTopOfBook ? quote.asks.front().Price : 0.0;
                const double bestBid = hasTopOfBook ? quote.bids.front().Price : 0.0;
                const double midPrice = hasTopOfBook ? (bestAsk + bestBid) / 2.0 : 0.0;
                const std::string symbol = quote.symbol;
                const std::int64_t quoteTimestamp = quote.timestamp_ms;
                const std::uint64_t quoteCount = rawQuoteCount;

                UI::Get().Update([&] {
                    quoteViewState.status = hasTopOfBook ? "Live data receiving" : "Receiving quote updates";
                    quoteViewState.symbol = symbol.empty() ? "SPY" : symbol;
                    quoteViewState.has_quote = hasTopOfBook;
                    quoteViewState.best_bid = bestBid;
                    quoteViewState.best_ask = bestAsk;
                    quoteViewState.mid_price = midPrice;
                    quoteViewState.spread = bestAsk - bestBid;
                    quoteViewState.latest_timestamp_ms = quoteTimestamp;
                    quoteViewState.quote_count = quoteCount;
                });
            }

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

        constexpr std::size_t INPUT_WINDOW_LENGTH = 30;
        constexpr std::size_t PREDICTION_HORIZON = 30;
        constexpr std::size_t MAX_PREDICTION_MATCHES = 32;
        constexpr std::size_t MAX_CACHED_RECORD_FILES = 128;

        std::unordered_map<std::string, std::vector<Core::MLRecord>> historicalRecordCache{};

        auto LoadHistoricalRecords = [&](const std::array<char, 8> &tradingDayId)
            -> const std::vector<Core::MLRecord>& {

            // Convert YYYYMMDD into YYYY-MM-DD.
            std::string date(tradingDayId.data(), tradingDayId.size());
            date.insert(4, "-");
            date.insert(7, "-");

            // Return cached records if this day has already been loaded.
            auto cached = historicalRecordCache.find(date);

            if (cached != historicalRecordCache.end()) {
                return cached->second;
            }

            // Keep the cache bounded.
            if (historicalRecordCache.size() >= MAX_CACHED_RECORD_FILES) {
                historicalRecordCache.clear();
            }

            const std::filesystem::path recordPath =
                Common::Storage::RECORDS_BASE_PATH / (date + ".rec");

            std::ifstream file(recordPath, std::ios::binary | std::ios::ate);

            if (!file.is_open()) {
                throw std::runtime_error(
                    "Unable to open historical record file: " + recordPath.string()
                );
            }

            const std::streamoff fileSize = file.tellg();
            const std::streamoff recordSize =
                static_cast<std::streamoff>(sizeof(Core::MLRecord));

            if (fileSize < 0 || fileSize % recordSize != 0) {
                throw std::runtime_error(
                    "Invalid historical record file size: " + recordPath.string()
                );
            }

            const std::size_t recordCount =
                static_cast<std::size_t>(fileSize / recordSize);

            std::vector<Core::MLRecord> records(recordCount);

            file.seekg(0, std::ios::beg);

            if (fileSize > 0) {
                const std::streamsize bytesToRead =
                    static_cast<std::streamsize>(fileSize);

                file.read(
                    reinterpret_cast<char*>(records.data()),
                    bytesToRead
                );

                if (file.gcount() != bytesToRead) {
                    throw std::runtime_error(
                        "Failed reading historical record file: " + recordPath.string()
                    );
                }
            }

            file.close();

            Logger::Info(
                std::format(
                    "Loaded {} historical records from {}",
                    records.size(),
                    recordPath.filename().string()
                ),
                true
            );

            return historicalRecordCache.emplace(
                date,
                std::move(records)
            ).first->second;
        };

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

                        Logger::Info(
                            std::format(
                                "Starting prediction at: {}",
                                Common::DateTime::GetCurrentDateTime().ToString_Time()
                            ),
                            true
                        );

                        // ======================================== //
                        // STEP 1: NORMALIZE AND QUERY FAISS
                        // ======================================== //

                        const std::vector<float> queryVector = activeIndex->NormalizeQuery(queryRecords);

                        const std::vector<Core::HalfHourIndex::QueryResult> queryResults =
                            activeIndex->Query(queryVector, MAX_QUERY_RESULTS);

                        if (queryResults.empty() == false) {

                            // ======================================== //
                            // STEP 2: RETRIEVE HISTORICAL FUTURES
                            // ======================================== //

                            struct HistoricalFuture {
                                double weight;
                                std::array<Core::MLRecord, PREDICTION_HORIZON> records;
                            };

                            std::vector<HistoricalFuture> historicalFutures{};
                            historicalFutures.reserve(MAX_PREDICTION_MATCHES);

                            constexpr double DISTANCE_EPSILON = 1e-6;

                            // Query results are ordered from nearest to furthest.
                            // Select the first 32 matches with valid future records.
                            for (const Core::HalfHourIndex::QueryResult& result : queryResults) {

                                if (historicalFutures.size() >= MAX_PREDICTION_MATCHES) {
                                    break;
                                }

                                if (std::isfinite(result.distance) == false ||
                                    result.distance < 0.0f) {
                                    continue;
                                }

                                // Load the original, unnormalized records for this trading day.
                                const std::vector<Core::MLRecord> &historicalRecords = LoadHistoricalRecords(result.tradingDayId);

                                // The matched pattern occupies:
                                // recordIndex ... recordIndex + 29
                                //
                                // Its future begins immediately afterward.
                                const std::size_t futureStart = 
                                    static_cast<std::size_t>(result.recordIndex) +
                                    INPUT_WINDOW_LENGTH;

                                // Ensure the complete prediction horizon exists.
                                if (futureStart > historicalRecords.size() ||
                                    PREDICTION_HORIZON >
                                        historicalRecords.size() - futureStart) {
                                    continue;
                                }

                                HistoricalFuture future{};

                                // Inverse-distance weighting.
                                future.weight =
                                    1.0 / (static_cast<double>(result.distance) +
                                        DISTANCE_EPSILON);

                                // Copy the next 30 original MLRecords.
                                for (std::size_t i = 0; i < PREDICTION_HORIZON; ++i) {
                                    future.records[i] = historicalRecords[futureStart + i];
                                }

                                historicalFutures.push_back(std::move(future));
                            }

                            // ======================================== //
                            // STEP 3: CALCULATE WEIGHTED FUTURES
                            // ======================================== //

                            if (historicalFutures.empty() == false) {

                                std::array<double, PREDICTION_HORIZON> expectedMidDelta{};

                                double totalWeight = 0.0;

                                // Calculate the weighted average midpoint change
                                // for each second in the prediction horizon.
                                for (const HistoricalFuture& future : historicalFutures) {

                                    totalWeight += future.weight;

                                    for (std::size_t i = 0; i < PREDICTION_HORIZON; ++i) {
                                        expectedMidDelta[i] +=
                                            future.weight * future.records[i].mid_delta;
                                    }
                                }

                                // Normalize the weighted averages.
                                for (double &delta : expectedMidDelta) {
                                    delta /= totalWeight;
                                }

                                // ======================================== //
                                // STEP 4: CALCULATE CUMULATIVE FUTURES
                                // ======================================== //

                                std::array<double, PREDICTION_HORIZON> expectedCumulativeMidDelta{};

                                double cumulativeDelta = 0.0;

                                for (std::size_t i = 0; i < PREDICTION_HORIZON; ++i) {

                                    cumulativeDelta += expectedMidDelta[i];

                                    expectedCumulativeMidDelta[i] = cumulativeDelta;
                                }

                                // ======================================== //
                                // STEP 5: EXTRACT FORECAST HORIZONS
                                // ======================================== //

                                std::array<double, PredictionEvaluationHorizons.size()> expectedChanges{};
                                for (std::size_t i = 0; i < PredictionEvaluationHorizons.size(); ++i) {
                                    expectedChanges[i] = expectedCumulativeMidDelta[PredictionEvaluationHorizons[i] - 1];
                                }

                                LogEvaluationReports(predictionAnalytics.SubmitForecast({
                                    .as_of_timestamp_ms = queryTimestampMs,
                                    .historical_matches = historicalFutures.size(),
                                    .cumulative_changes = expectedChanges
                                }));
                            }
                            else {
                                Logger::Warning(
                                    "No historical matches had sufficient future records."
                                );
                            }
                        }
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(30 * 1000));
        }
        // ========== END PREDICTION LOOP ========== //

        this->QuoteClient.Stop();
        LogEvaluationReports(predictionAnalytics.FlushIncomplete());
    }

    // ========== END ORCHESTRATOR IMPLEMENTATION ========== //
    
    void BeginSandboxRuntime() {
        Orchestrator orchestrator(Client::WebullEnvironment::Sandbox);
        orchestrator.Run();
    }   

}
