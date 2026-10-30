// dukascopy.cpp
// Implementation of the Dukascopy namespace for exporting historical raw tick
// data into a binary format used by Sonny's ML architecture.

#include "dukascopy.hpp"

#include "../common/jsonutil.hpp"
#include "../common/logger.hpp"
#include "../common/storage.hpp"
#include "../client/simplecurlwrapper.hpp"
#include "../core/setup.hpp"
#include "../core/structures.hpp"
#include "../ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <ftxui/dom/elements.hpp>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Dukascopy {

    inline static const std::string infoUrl =
        "https://jetta.dukascopy.com/v1/instruments/SPY.US-USD";

    inline static const std::string ticksUrlBase =
        "https://jetta.dukascopy.com/v1/ticks/SPY.US-USD/";

    // Regular US session length: 9:30 to 16:00 New York time (6.5 hours).
    constexpr Common::UnixTimestamp sessionLengthSeconds = 23400;

    std::unordered_map<std::string, std::string> headers = {
        { "Accept", "*/*" },
        { "Accept-Language", "en-US,en;q=0.6" },
        { "Cache-Control", "no-cache" },
        { "Origin", "https://widgets.dukascopy.com" },
        { "Pragma", "no-cache" },
        { "Priority", "u=1, i" },
        { "Referer", "https://widgets.dukascopy.com/" },
        { "sec-ch-ua", "\"Not:A-Brand\";v=\"8\", \"Chromium\";v=\"156\", \"Brave\";v=\"156\"" },
        { "sec-ch-ua-mobile", "?0" },
        { "sec-ch-ua-platform", "\"Windows\"" },
        { "sec-fetch-dest", "empty" },
        { "sec-fetch-mode", "cors" },
        { "sec-fetch-site", "same-site" },
        { "sec-gpc", "1" },
        { "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/156.0.0.0 Safari/537.36" }
    };

    struct ApiTickResponse {
        Common::UnixTimestamp Timestamp;
        float Multiplier;
        float Ask;
        float Bid;

        // Deltas.
        std::vector<std::int32_t> Times;
        std::vector<std::int16_t> Asks;
        std::vector<std::int16_t> Bids;

        // Actual volume values.
        std::vector<std::int32_t> AskVolumes;
        std::vector<std::int32_t> BidVolumes;
    };

    struct OHLCV_BidAsk {
        Common::UnixTimestamp Timestamp;

        std::int64_t OpenBid;
        std::int64_t HighBid;
        std::int64_t LowBid;
        std::int64_t CloseBid;

        std::int64_t OpenAsk;
        std::int64_t HighAsk;
        std::int64_t LowAsk;
        std::int64_t CloseAsk;

        std::uint32_t BidVolume;
        std::uint32_t AskVolume;

        float Multiplier;
    };

    std::string BuildTicksUrl(
        const Common::DateTime& dt)
    {
        return ticksUrlBase +
            std::to_string(dt.GetYear()) + "/" +
            std::to_string(dt.GetMonth()) + "/" +
            std::to_string(dt.GetDay()) + "/";
    }

    /**
     * @brief A single decoded tick with an absolute time and absolute integer prices.
     */
    struct Tick {
        std::int64_t TimeMs;
        std::int64_t Bid;
        std::int64_t Ask;
        std::uint32_t BidVolume;
        std::uint32_t AskVolume;
        float Multiplier;
    };

    /**
     * @brief Decodes the delta-encoded hourly responses into absolute ticks ordered by time.
     * 
     * @param tickResponses 
     * @return std::vector<Tick> 
     */
    std::vector<Tick> DecodeTicks(const std::vector<ApiTickResponse>& tickResponses) {
        std::vector<Tick> ticks;

        for (const ApiTickResponse& response : tickResponses) {
            const float multiplier = response.Multiplier;

            // The first tick carries the absolute prices; later ticks are deltas from the previous tick.
            // Times are deltas too, starting from the response timestamp (milliseconds).
            auto runningBid = static_cast<std::int64_t>(std::round(response.Bid / multiplier));
            auto runningAsk = static_cast<std::int64_t>(std::round(response.Ask / multiplier));
            auto runningTime = static_cast<std::int64_t>(response.Timestamp);

            const std::size_t tickCount = std::min({
                response.Times.size(),
                response.Bids.size(),
                response.Asks.size(),
                response.BidVolumes.size(),
                response.AskVolumes.size()
            });

            for (std::size_t i = 0; i < tickCount; ++i) {
                runningTime += response.Times[i];

                if (i > 0) {
                    runningBid += response.Bids[i];
                    runningAsk += response.Asks[i];
                }

                ticks.push_back(Tick{
                    .TimeMs = runningTime,
                    .Bid = runningBid,
                    .Ask = runningAsk,
                    .BidVolume = static_cast<std::uint32_t>(response.BidVolumes[i]),
                    .AskVolume = static_cast<std::uint32_t>(response.AskVolumes[i]),
                    .Multiplier = multiplier
                });
            }
        }

        std::ranges::stable_sort(ticks, {}, &Tick::TimeMs);
        return ticks;
    }

    /**
     * @brief Creates a zero-volume candle whose prices all equal the given tick's prices.
     */
    OHLCV_BidAsk FlatCandle(const Common::UnixTimestamp second, const Tick& tick) {
        return OHLCV_BidAsk{
            .Timestamp = second,

            .OpenBid = tick.Bid,
            .HighBid = tick.Bid,
            .LowBid = tick.Bid,
            .CloseBid = tick.Bid,

            .OpenAsk = tick.Ask,
            .HighAsk = tick.Ask,
            .LowAsk = tick.Ask,
            .CloseAsk = tick.Ask,

            .BidVolume = 0,
            .AskVolume = 0,

            .Multiplier = tick.Multiplier
        };
    }

    /**
     * @brief Folds a tick into a candle that already holds an earlier tick from the same second.
     */
    void AddTickToCandle(OHLCV_BidAsk& candle, const Tick& tick) {
        candle.HighBid = std::max(candle.HighBid, tick.Bid);
        candle.LowBid = std::min(candle.LowBid, tick.Bid);
        candle.CloseBid = tick.Bid;

        candle.HighAsk = std::max(candle.HighAsk, tick.Ask);
        candle.LowAsk = std::min(candle.LowAsk, tick.Ask);
        candle.CloseAsk = tick.Ask;

        candle.BidVolume += tick.BidVolume;
        candle.AskVolume += tick.AskVolume;

        candle.Multiplier = tick.Multiplier;
    }

    /**
     * @brief Builds exactly one candle per second of the regular session, so record N is always
     * N seconds after the open. Seconds without ticks repeat the previous close with zero volume.
     * 
     * @param tickResponses Hourly responses for the day.
     * @param sessionOpen Session open as a Unix timestamp in seconds.
     * @param sessionClose Session close as a Unix timestamp in seconds (exclusive).
     * @return One candle per session second, or empty if no ticks fall inside the session.
     */
    std::vector<OHLCV_BidAsk>
    ExtractOneSecondCandlesticks(
        const std::vector<ApiTickResponse>& tickResponses,
        const Common::UnixTimestamp sessionOpen,
        const Common::UnixTimestamp sessionClose)
    {
        const std::vector<Tick> ticks = DecodeTicks(tickResponses);

        const auto toMs = [](const Common::UnixTimestamp seconds) {
            return static_cast<std::int64_t>(seconds) * 1000;
        };

        auto next = std::ranges::lower_bound(ticks, toMs(sessionOpen), {}, &Tick::TimeMs);
        const auto sessionEnd = std::ranges::lower_bound(ticks, toMs(sessionClose), {}, &Tick::TimeMs);

        if (next == sessionEnd) {
            return {};
        }

        // Opening seconds without ticks carry the last pre-open quote,
        // or the first session quote when nothing was quoted before the open.
        const Tick* previous = next == ticks.begin() ? &*next : &*std::prev(next);

        std::vector<OHLCV_BidAsk> result;
        result.reserve(static_cast<std::size_t>(sessionClose - sessionOpen));

        for (Common::UnixTimestamp second = sessionOpen; second < sessionClose; ++second) {
            OHLCV_BidAsk candle = FlatCandle(second, *previous);

            const std::int64_t secondEndMs = toMs(second + 1);
            bool firstTick = true;

            for (; next != sessionEnd && next->TimeMs < secondEndMs; ++next) {
                if (firstTick) {
                    candle = FlatCandle(second, *next);
                    firstTick = false;
                }

                AddTickToCandle(candle, *next);
                previous = &*next;
            }

            result.push_back(candle);
        }

        return result;
    }

    bool SaveDayCorpusToDisk(
        const std::vector<OHLCV_BidAsk>& candleSticks,
        const Common::DateTime& dt)
    {
        using namespace Common::Storage;

        if (FileExists(CORPUS_BASE_PATH) == false &&
            CreateDirectory(CORPUS_BASE_PATH) == false) {

            Logger::Error("Could not create corpus directory");
            throw std::runtime_error(
                "Could not create corpus directory"
            );
        }

        const std::string corpusFile = (dt.ToString_Date() + ".corpus");

        std::ofstream corpusFileStream(CORPUS_BASE_PATH / corpusFile, std::ios::app);
        
        if (corpusFileStream.is_open() == false) {
            Logger::Error("Could not open corpus file");

            throw std::runtime_error(
                "Could not open corpus file"
            );
        }

        // The multiplier is metadata for the entire file,
        // so write it once rather than once per candle.
        corpusFileStream
            << "multiplier="
            << candleSticks.front().Multiplier
            << '\n';

        for (const OHLCV_BidAsk& bidAsk : candleSticks) {
            corpusFileStream
                << bidAsk.OpenBid << ','
                << bidAsk.HighBid << ','
                << bidAsk.LowBid << ','
                << bidAsk.CloseBid << ','
                << bidAsk.OpenAsk << ','
                << bidAsk.HighAsk << ','
                << bidAsk.LowAsk << ','
                << bidAsk.CloseAsk
                << '\n';
        }
        corpusFileStream.close();
        return true;
    }

    bool SaveDayRecordsToDisk(
        const std::vector<OHLCV_BidAsk>& candleSticks,
        const Common::DateTime& dt)
    {
        using namespace Common::Storage;

        if (FileExists(Common::Storage::RECORDS_BASE_PATH) == false &&
            CreateDirectory(Common::Storage::RECORDS_BASE_PATH) == false) {

            Logger::Error("Could not create records directory");

            throw std::runtime_error(
                "Could not create records directory"
            );
        }

        std::string recFile = (dt.ToString_Date() + ".rec");
        std::ofstream recFileStream(
            Common::Storage::RECORDS_BASE_PATH / recFile,
            std::ios::binary | std::ios::trunc
        );

        if (recFileStream.is_open() == false) {
            Logger::Error(
                std::format(
                    "Could not open record file: {}",
                    recFile
                )
            );

            throw std::runtime_error(
                "Could not open record file"
            );
        }

        static_assert(sizeof(Core::MLRecord) == 12);

        const float multiplier =
            candleSticks.front().Multiplier;

        const auto getMid =
            [multiplier](const OHLCV_BidAsk& candle) -> float {
                const float bid =
                    static_cast<float>(candle.CloseBid) * multiplier;

                const float ask =
                    static_cast<float>(candle.CloseAsk) * multiplier;

                return (bid + ask) * 0.5f;
            };

        const auto getSpread =
            [multiplier](const OHLCV_BidAsk& candle) -> float {
                const float bid =
                    static_cast<float>(candle.CloseBid) * multiplier;

                const float ask =
                    static_cast<float>(candle.CloseAsk) * multiplier;

                return ask - bid;
            };

        float previousMid =
            getMid(candleSticks.front());

        float previousSpread =
            getSpread(candleSticks.front());

        for (std::size_t i = 0; i < candleSticks.size(); ++i) {
            constexpr float firstDeltaSentinel = 0.0f;

            const OHLCV_BidAsk& candle =
                candleSticks[i];

            const float mid = getMid(candle);
            const float spread = getSpread(candle);

            const Core::MLRecord record{
                .mid_delta =
                    i == 0
                        ? firstDeltaSentinel
                        : mid - previousMid,

                .spread_delta =
                    i == 0
                        ? firstDeltaSentinel
                        : spread - previousSpread,

                .volume = static_cast<float>(candle.BidVolume)
            };

            recFileStream.write(
                reinterpret_cast<const char*>(&record),
                sizeof(record)
            );

            if (!recFileStream) {
                Logger::Error("Failed writing ML record");

                throw std::runtime_error(
                    "Failed writing ML record"
                );
            }

            previousMid = mid;
            previousSpread = spread;
        }
        recFileStream.close();
        return true;
    }

    /**
     * @brief Executes a GET request and parses the body as JSON.
     * 
     * @param curlWrapper 
     * @param request 
     * @return The parsed body, or nothing if the request failed, returned a non-2xx status, or was not valid JSON.
     */
    std::optional<Common::JsonUtility> FetchJson(Client::SimpleCurlWrapper &curlWrapper, Client::CurlRequest &request) {
        std::string body;
        request.SetCallback([&body](std::string response) {
            body = std::move(response);
        });

        const Client::CurlRequestEnum result = curlWrapper.ExecuteHttpRequest(request);
        const std::string& uri = request.GetFormattedUri();

        if (result != Client::CurlRequestEnum::SUCCESS) {
            Logger::Warning(std::format("Request failed: {} ({})", uri, request.GetErrorMessage()), true);
            return std::nullopt;
        }

        // The browser path does not report a status, so 0 is accepted.
        const long status = request.GetResponseCode();
        if (status != 0 && (status < 200 || status >= 300)) {
            Logger::Warning(std::format("Request returned HTTP {}: {}", status, uri), true);
            return std::nullopt;
        }

        if (body.empty()) {
            return std::nullopt;
        }

        try {
            return Common::JsonUtility(body);
        }
        catch (const std::invalid_argument &) {
            Logger::Warning(std::format("Response was not valid JSON: {}", uri), true);
            return std::nullopt;
        }
    }

    /**
     * @brief Reads one hour of tick data from its JSON response.
     * 
     * @param hourJson 
     * @return The tick data, or nothing if any required field is missing or invalid.
     */
    std::optional<ApiTickResponse> ParseTickResponse(const Common::JsonUtility &hourJson) {
        ApiTickResponse res{};

        hourJson.TryGet<Common::UnixTimestamp>("timestamp", res.Timestamp);
        hourJson.TryGet<float>("multiplier", res.Multiplier);
        hourJson.TryGet<float>("bid", res.Bid);
        hourJson.TryGet<float>("ask", res.Ask);
        hourJson.TryGet<std::vector<std::int32_t>>("times", res.Times);
        hourJson.TryGet<std::vector<std::int16_t>>("asks", res.Asks);
        hourJson.TryGet<std::vector<std::int16_t>>("bids", res.Bids);
        hourJson.TryGet<std::vector<std::int32_t>>("askVolumes", res.AskVolumes);
        hourJson.TryGet<std::vector<std::int32_t>>("bidVolumes", res.BidVolumes);

        if (res.Timestamp > 0 &&
            res.Multiplier > 0 &&
            res.Bid > 0 &&
            res.Ask > 0 &&
            res.Times.empty() == false &&
            res.Asks.empty() == false &&
            res.Bids.empty() == false &&
            res.AskVolumes.empty() == false &&
            res.BidVolumes.empty() == false) {
            return res;
        }

        return std::nullopt;
    }

    void ExportFullDay(const Common::DateTime& dt, Client::SimpleCurlWrapper &curlWrapper) {
        std::vector<OHLCV_BidAsk> candleSticks;

        {
            const std::string ticksUrl = BuildTicksUrl(dt);

            std::vector<ApiTickResponse> tickResponses;

            Client::CurlRequest hourRequest("");
            hourRequest.AddHeader(headers);

            // Dukascopy hours for the regular US trading session.
            for (std::size_t hour = 13; hour < 20; ++hour) {
                hourRequest.ReassignUri(
                    ticksUrl + std::to_string(hour)
                );

                const std::optional<Common::JsonUtility> hourJson = FetchJson(curlWrapper, hourRequest);
                if (hourJson) {
                    std::optional<ApiTickResponse> response = ParseTickResponse(*hourJson);
                    if (response) {
                        tickResponses.push_back(std::move(*response));
                    }
                }
            }

            // The session open is computed in New York time, so it follows daylight saving changes.
            const Common::UnixTimestamp sessionOpen = Common::DateTime(
                dt.GetYear(), dt.GetMonth(), dt.GetDay(), 9, 30, 0, true
            ).GetTimestamp();

            candleSticks = ExtractOneSecondCandlesticks(
                tickResponses,
                sessionOpen,
                sessionOpen + sessionLengthSeconds
            );
        }

        if (candleSticks.empty() == false) {
            if (SaveDayCorpusToDisk(candleSticks, dt)
                && SaveDayRecordsToDisk(candleSticks, dt)) {
                Logger::Info(
                    std::format(
                        "Day processed: {} ({} candles)",
                        dt.ToString_Date(),
                        candleSticks.size()
                    ), true
                );
            }
        }
        else {
            Logger::Warning(
                std::format(
                    "Day skipped: {}",
                    dt.ToString_Date()
                ), true
            );
        }
    }

    void BeginCorpusExport(bool replaceExisting) {
        {
            std::string headerMessage = replaceExisting ? 
            "      Beginning full corpus export" : "     Beginning missing records export";
            Logger::Info("========================================", true);
            Logger::Info(headerMessage, true);
            Logger::Info("========================================", true);
        }

        std::string status{};
        std::string currentDate{};
        std::size_t daysElapsed = 0;

        auto view = UI::Get().PushView([&] {
            const float progress =
                Core::Setup::TOTAL_DAYS == 0
                ? 0.0f
                : static_cast<float>(daysElapsed) /
                static_cast<float>(Core::Setup::TOTAL_DAYS);

            return ftxui::window(
                ftxui::text("Downloading Historical Records") | ftxui::bold,
                ftxui::vbox({
                    ftxui::text(status),
                    ftxui::separator(),
                    ftxui::hbox({
                        ftxui::text("Current date: "),
                        ftxui::text(currentDate) | ftxui::bold,
                    }),
                    ftxui::gauge(progress),
                    ftxui::text(
                        std::format(
                            "{} / {} days",
                            daysElapsed,
                            Core::Setup::TOTAL_DAYS
                        )
                    )
                }) |
                ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 55)
            );
        });

        UI::Get().Refresh();

        // Wrapper running browser instance. Will be sharing this object throughout the full corpus run.
        Client::SimpleCurlWrapper curlWrapper;

        std::string description{};

        {
            Client::CurlRequest infoRequest(infoUrl);
            infoRequest.AddHeader(headers);

            const std::optional<Common::JsonUtility> spyData = FetchJson(curlWrapper, infoRequest);
            if (spyData) {
                spyData->TryGet<std::string>("description", description);
            }
            else {
                Logger::Error(
                    "Unable to access info URI. Terminating process.", 
                    true
                );
            }
        }

        if (description.empty() == false) {
            Logger::Info(
                std::format(
                    "Description: {}",
                    description
                ), true
            );

            using namespace Common::Storage;
            Common::DateTime exportDate = Core::Setup::BEGIN_DATE;

            for (; exportDate <= Core::Setup::END_DATE; exportDate.NextDay()) {

                UI::Get().Update([&] {
                    currentDate = exportDate.ToString_Date();
                    status = std::format("Processing {}", currentDate);
                });

                const std::filesystem::path corpusPath = CORPUS_BASE_PATH / (exportDate.ToString_Date() + ".corpus");
                const std::filesystem::path recordsPath = RECORDS_BASE_PATH / (exportDate.ToString_Date() + ".rec");

                if (replaceExisting) {
                    // Ensure that the record file does not exist, or is empty,
                    // then run the export for that day.

                    if (FileExists(corpusPath)) {
                        DeleteFile(corpusPath);
                    }
                    if (FileExists(recordsPath)) {
                        DeleteFile(recordsPath);
                    }

                    if (exportDate.IsWeekday()) {
                        ExportFullDay(exportDate, curlWrapper);
                    }
                    else {
                        Logger::Warning(std::format(
                            "Weekend skipped: {}",
                            exportDate.ToString_Date()),
                            true
                        );
                    }
                }
                else {
                    // Check first if the record file exists and skip it if so.
                    // If one exists but not the other, delete and run export.
                    if (FileExists(corpusPath) == false || FileExists(recordsPath) == false) {
                        DeleteFile(corpusPath);
                        DeleteFile(recordsPath);
                        if (exportDate.IsWeekday()) {
                            ExportFullDay(exportDate, curlWrapper);
                        }
                        else {
                            Logger::Warning(std::format(
                                "Weekend skipped: {}",
                                exportDate.ToString_Date()),
                                true
                            );
                        }
                    }
                }

                UI::Get().Update([&] { ++daysElapsed; });
            }
        }
        else {
            Logger::Error("Description empty. Terminating process.", true);
        }
    }

}