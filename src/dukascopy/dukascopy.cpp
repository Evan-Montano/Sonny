// dukascopy.cpp
// Implementation of the Dukascopy namespace for exporting historical raw tick
// data into a binary format used by Sonny's ML architecture.

#include "dukascopy.hpp"

#include "../common/jsonutil.hpp"
#include "../common/logger.hpp"
#include "../common/storage.hpp"
#include "../client/simplecurlwrapper.hpp"
#include "../core/setup.hpp"
#include "../ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <ftxui/dom/elements.hpp>
#include <stdexcept>
#include <cstdint>
#include <string>
#include <vector>

namespace Dukascopy {

    inline static const std::string infoUrl =
        "https://jetta.dukascopy.com/v1/instruments/SPY.US-USD";

    inline static const std::string ticksUrlBase =
        "https://jetta.dukascopy.com/v1/ticks/SPY.US-USD/";

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

    std::vector<OHLCV_BidAsk>
    ExtractOneSecondCandlesticks(
        const std::vector<ApiTickResponse>& tickResponses,
        const Common::DateTime& dt)
    {
        std::vector<OHLCV_BidAsk> result;

        if (tickResponses.empty() == false) {
            Common::UnixTimestamp timestamp = tickResponses.front().Timestamp;

            for (const ApiTickResponse& response : tickResponses) {
                const float multiplier = response.Multiplier;

                auto runningBid =
                    static_cast<std::int64_t>(
                        std::round(response.Bid / multiplier)
                    );

                auto runningAsk =
                    static_cast<std::int64_t>(
                        std::round(response.Ask / multiplier)
                    );

                std::int32_t runningTime = 
                    (response.Timestamp + response.Times[0]) % 1000;

                std::uint32_t runningBidVolume =
                    response.BidVolumes[0];

                std::uint32_t runningAskVolume =
                    response.AskVolumes[0];

                std::vector<std::int64_t> currentBidRow;
                std::vector<std::int64_t> currentAskRow;

                currentBidRow.push_back(runningBid);
                currentAskRow.push_back(runningAsk);

                for (std::size_t i = 1; i < response.Bids.size(); ++i) {
                    runningTime += response.Times[i];

                    if (runningTime >= 1000) {
                        result.push_back(
                            OHLCV_BidAsk{
                                .Timestamp = ++timestamp,

                                .OpenBid = currentBidRow.front(),
                                .HighBid = *std::ranges::max_element(currentBidRow),
                                .LowBid = *std::ranges::min_element(currentBidRow),
                                .CloseBid = currentBidRow.back(),

                                .OpenAsk = currentAskRow.front(),
                                .HighAsk = *std::ranges::max_element(currentAskRow),
                                .LowAsk = *std::ranges::min_element(currentAskRow),
                                .CloseAsk = currentAskRow.back(),

                                .BidVolume = runningBidVolume,
                                .AskVolume = runningAskVolume,

                                .Multiplier = multiplier
                            }
                        );

                        currentBidRow.clear();
                        currentAskRow.clear();

                        runningBidVolume = 0;
                        runningAskVolume = 0;

                        runningTime -= 1000;
                    }

                    runningBid += response.Bids[i];
                    runningAsk += response.Asks[i];

                    runningBidVolume += response.BidVolumes[i];
                    runningAskVolume += response.AskVolumes[i];

                    currentBidRow.push_back(runningBid);
                    currentAskRow.push_back(runningAsk);
                }
            }
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

        std::ofstream corpusFileStream = OpenFile_App(CORPUS_BASE_PATH, corpusFile);
        
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
        std::ofstream recFileStream = 
            OpenFile_App(Common::Storage::RECORDS_BASE_PATH, recFile, true);

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

        struct MLRecord {
            float mid_delta;
            float spread_delta;
            std::uint32_t volume;
        };

        static_assert(sizeof(MLRecord) == 12);

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

            const MLRecord record{
                .mid_delta =
                    i == 0
                        ? firstDeltaSentinel
                        : mid - previousMid,

                .spread_delta =
                    i == 0
                        ? firstDeltaSentinel
                        : spread - previousSpread,

                .volume = candle.BidVolume
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

    void ExportFullDay(const Common::DateTime& dt) {
        std::vector<OHLCV_BidAsk> candleSticks;

        {
            const std::string ticksUrl = BuildTicksUrl(dt);

            std::vector<ApiTickResponse> tickResponses;

            Client::SimpleCurlWrapper curl;
            Client::CurlRequest hourRequest("");
            hourRequest.AddHeader(
                "User-Agent",
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/153.0.0.0 Safari/537.36"
            );

            hourRequest.SetCallback(
                [&](const std::string& response) {
                    const Common::JsonUtility hourJson(response);

                    ApiTickResponse res{};

                    hourJson.TryGet<Common::UnixTimestamp>(
                        "timestamp",
                        res.Timestamp
                    );

                    hourJson.TryGet<float>(
                        "multiplier",
                        res.Multiplier
                    );

                    hourJson.TryGet<float>(
                        "bid",
                        res.Bid
                    );

                    hourJson.TryGet<float>(
                        "ask",
                        res.Ask
                    );

                    hourJson.TryGet<std::vector<std::int32_t>>(
                        "times",
                        res.Times
                    );

                    hourJson.TryGet<std::vector<std::int16_t>>(
                        "asks",
                        res.Asks
                    );

                    hourJson.TryGet<std::vector<std::int16_t>>(
                        "bids",
                        res.Bids
                    );

                    hourJson.TryGet<std::vector<std::int32_t>>(
                        "askVolumes",
                        res.AskVolumes
                    );

                    hourJson.TryGet<std::vector<std::int32_t>>(
                        "bidVolumes",
                        res.BidVolumes
                    );

                    if (res.Timestamp > 0 &&
                        res.Multiplier > 0 &&
                        res.Bid > 0 &&
                        res.Ask > 0 &&
                        res.Times.empty() == false &&
                        res.Asks.empty() == false &&
                        res.Bids.empty() == false &&
                        res.AskVolumes.empty() == false &&
                        res.BidVolumes.empty() == false) {

                        tickResponses.push_back(std::move(res));
                    }
                }
            );

            // Dukascopy hours for the regular US trading session.
            for (std::size_t hour = 13; hour < 20; ++hour) {
                hourRequest.ReassignUri(
                    ticksUrl + std::to_string(hour)
                );

                curl.ExecuteHttpRequest(hourRequest);
            }

            candleSticks = ExtractOneSecondCandlesticks(
                tickResponses,
                dt
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

    std::string UpdateStatusBar(const long& daysElapsed) {

        const static long totalDays = Core::Setup::END_DATE - Core::Setup::BEGIN_DATE;
        constexpr std::size_t BAR_WIDTH = 50;
        const double progress = std::clamp(
            static_cast<double>(daysElapsed) / totalDays,
            0.0,
            1.0
        );

        const std::size_t filled = static_cast<std::size_t>(progress * BAR_WIDTH);

        return "[" +
            std::string(filled, '=') +
            std::string(BAR_WIDTH - filled, ' ') +
            "]";
    }

    void BeginCorpusExport(bool replaceExisting) {
        {
            std::string headerMessage = replaceExisting ? 
            "      Beginning full corpus export" : "     Beginning missing records export";
            Logger::Info("========================================", true);
            Logger::Info(headerMessage, true);
            Logger::Info("========================================", true);
        }

        std::string status = "";
        auto view = UI::Get().PushView([&] {
            return ftxui::window(
                ftxui::text("Downloading Historical Records") | ftxui::bold,
                ftxui::vbox({
                    ftxui::text(status)
                })
            );
        });

        UI::Get().Refresh();

        Common::JsonUtility spyData(std::string("{}"));

        {
            Client::SimpleCurlWrapper curl;
            Client::CurlRequest infoRequest(infoUrl);
            infoRequest.AddHeader(
                "User-Agent",
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/153.0.0.0 Safari/537.36"
            );

            infoRequest.SetCallback(
                [&](const std::string& response) {
                    if (response.empty() == false) {
                        spyData = Common::JsonUtility(response);
                    }
                    else {
                        Logger::Error(
                            "Unable to access info URI. Terminating process.", 
                            true
                        );
                    }
                }
            );

            curl.ExecuteHttpRequest(infoRequest);
        }

        std::string description{};
        spyData.TryGet<std::string>("description", description);

        if (description.empty() == false) {
            Logger::Info(
                std::format(
                    "Description: {}",
                    description
                ), true
            );

            using namespace Common::Storage;
            Common::DateTime exportDate = Core::Setup::BEGIN_DATE;

            for (long daysElapsed = 0; exportDate <= Core::Setup::END_DATE; exportDate.NextDay()) {
                const std::filesystem::path corpusPath = 
                    CORPUS_BASE_PATH / (exportDate.ToString_Date() + ".corpus");
                const std::filesystem::path recordsPath = 
                    RECORDS_BASE_PATH / (exportDate.ToString_Date() + ".rec");

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
                        ExportFullDay(exportDate);
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
                            ExportFullDay(exportDate);
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

                ++daysElapsed;
                status = UpdateStatusBar(daysElapsed);
                UI::Get().Refresh();
            }
        }
        else {
            Logger::Error("Description empty. Terminating process.", true);
        }
    }

}