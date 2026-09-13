// dukascopy.hpp
// Procedure responsible for exporting historical raw tick data into a binary
// format used by Sonny's ML architecture.

#pragma once

#include "../common/datetime.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Dukascopy {

    class CorpusExporter {
    public:
        // CONSTRUCTOR
        CorpusExporter() = default;

        // METHODS
        void BeginCorpusExport();

    private:
        // MEMBERS
        inline static const std::string infoUrl =
            "https://jetta.dukascopy.com/v1/instruments/SPY.US-USD";

        inline static const std::string ticksUrlBase =
            "https://jetta.dukascopy.com/v1/ticks/SPY.US-USD/";

        // STRUCTS
        struct TickResponse {
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

        // METHODS
        void ExportFullDay(const Common::DateTime& dt);

        [[nodiscard]]
        static std::string BuildTicksUrl(const Common::DateTime& dt);

        [[nodiscard]]
        static std::vector<OHLCV_BidAsk> ExtractOneSecondCandlesticks(
            const std::vector<TickResponse>& tickResponses,
            const Common::DateTime& dt
        );

        static void SaveDayCorpusToDisk(
            const std::vector<OHLCV_BidAsk>& candleSticks,
            const Common::DateTime& dt
        );

        static void SaveDayRecordsToDisk(
            const std::vector<OHLCV_BidAsk>& candleSticks,
            const Common::DateTime& dt
        );
    };

}