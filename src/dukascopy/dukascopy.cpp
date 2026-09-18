// dukascopy.cpp
// Implementation of the Dukascopy namespace for exporting historical raw tick
// data into a binary format used by Sonny's ML architecture.

#include "dukascopy.hpp"

#include "../common/jsonutil.hpp"
#include "../common/logger.hpp"
#include "../client/simplecurlwrapper.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>

namespace Dukascopy {

    void CorpusExporter::BeginCorpusExport() {
        Logger::Info("========================================", true);
        Logger::Info("      Beginning full corpus export", true);
        Logger::Info("========================================", true);

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
                ),
                true
            );

            Common::DateTime exportDate(2026, Common::AUG, 3);

            for (const Common::DateTime endDate(2026, Common::AUG, 31); exportDate <= endDate; exportDate.NextDay()) {
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
        else {
            Logger::Error("Description empty. Terminating process.");
        }
    }

    void CorpusExporter::ExportFullDay(const Common::DateTime& dt) {
        std::vector<OHLCV_BidAsk> candleSticks;

        {
            const std::string ticksUrl = BuildTicksUrl(dt);

            std::vector<TickResponse> tickResponses;

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

                    TickResponse res{};

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
                    ),
                    true
                );
            }
        }
        else {
            Logger::Warning(
                std::format(
                    "Day skipped: {}",
                    dt.ToString_Date()
                )
            );
        }
    }

    std::string CorpusExporter::BuildTicksUrl(
        const Common::DateTime& dt)
    {
        return ticksUrlBase +
            std::to_string(dt.GetYear()) + "/" +
            std::to_string(dt.GetMonth()) + "/" +
            std::to_string(dt.GetDay()) + "/";
    }

    std::vector<CorpusExporter::OHLCV_BidAsk>
    CorpusExporter::ExtractOneSecondCandlesticks(
        const std::vector<TickResponse>& tickResponses,
        const Common::DateTime& dt)
    {
        std::vector<OHLCV_BidAsk> result;

        if (tickResponses.empty() == false) {
            Common::UnixTimestamp timestamp = tickResponses.front().Timestamp;

            for (const TickResponse& response : tickResponses) {
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

    bool CorpusExporter::SaveDayCorpusToDisk(
        const std::vector<OHLCV_BidAsk>& candleSticks,
        const Common::DateTime& dt)
    {
        namespace fs = std::filesystem;

        const fs::path basePath = "storage/corpus/";

        if (fs::exists(basePath) == false &&
            fs::create_directories(basePath) == false) {

            Logger::Error("Could not create corpus directory");
            throw std::runtime_error(
                "Could not create corpus directory"
            );
        }

        const fs::path corpusFile =
            basePath / (dt.ToString_Date() + ".corpus");

        std::ofstream corpusFileStream(
            corpusFile,
            std::ios::trunc
        );

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

    bool CorpusExporter::SaveDayRecordsToDisk(
        const std::vector<OHLCV_BidAsk>& candleSticks,
        const Common::DateTime& dt)
    {
        namespace fs = std::filesystem;

        const fs::path basePath = "storage/records/";

        if (fs::exists(basePath) == false &&
            fs::create_directories(basePath) == false) {

            Logger::Error("Could not create records directory");

            throw std::runtime_error(
                "Could not create records directory"
            );
        }

        const fs::path recFile =
            basePath / (dt.ToString_Date() + ".rec");

        std::ofstream recFileStream(
            recFile,
            std::ios::binary | std::ios::trunc
        );

        if (recFileStream.is_open() == false) {
            Logger::Error(
                std::format(
                    "Could not open record file: {}",
                    recFile.string()
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

}