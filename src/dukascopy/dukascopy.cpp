// dukascopy.cpp
// Implementation of the Dukascopy namespace for exporting historical raw tick data into a binary format used by Sonny's ML architecture.

#include "dukascopy.hpp"
#include "../common/jsonutil.hpp"
#include "../common/datetime.hpp"
#include "../client/simplecurlwrapper.hpp"
#include "../common/logger.hpp"

#include <vector>
#include <format>
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace Dukascopy {

	static inline const std::string infoUrl = "https://jetta.dukascopy.com/v1/instruments/SPY.US-USD";

	namespace {
		struct TickResponse {
			Common::UnixTimestamp Timestamp;
			float Multiplier;
			float Ask;
			float Bid;
		
			// Deltas.
			std::vector<int32_t> Times;
			std::vector<int16_t> Asks;
			std::vector<int16_t> Bids;

			// Actual volume values.
			std::vector<int32_t> AskVolumes;
			std::vector<int32_t> BidVolumes;
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
			float multiplier;
		};
	}

	//struct MLRecord {
	//	std::uint32_t SecondsSinceOpen;
	//	float MidDelta;
	//	float SpreadDelta;
	//	std::uint32_t Volume;
	//};

	static std::vector<OHLCV_BidAsk>  ExtractOneSecondCandlesticks(const std::vector<TickResponse> &tickResponses, const Common::DateTime &dt) {
		std::vector<OHLCV_BidAsk> res{};
		if (tickResponses.empty() == false) {
			Common::UnixTimestamp timestamp = tickResponses[0].Timestamp;

			for (const TickResponse& response : tickResponses) {
				const float multiplier = response.Multiplier;

				// Dukascopy's price deltas are expressed in units of the multiplier.
				// Converting the initial bid into those same integer units so that all
				// reconstruction remains exact and we avoid floating-point drift.
				std::int64_t runningBid = static_cast<std::int64_t>(std::round(response.Bid / multiplier));
				std::int64_t runningAsk = static_cast<std::int64_t>(std::round(response.Ask / multiplier));

				int32_t runningTime = (response.Timestamp + response.Times[0]) % 1000;
				uint32_t runningBidVolume = response.BidVolumes[0];
				uint32_t runningAskVolume = response.AskVolumes[0];

				std::vector<std::int64_t> currentBidRow;
				std::vector<std::int64_t> currentAskRow;
				currentBidRow.push_back(runningBid);
				currentAskRow.push_back(runningAsk);

				// Bids and asks vectors should be the same size, so we're looping over both at the same time
				for (size_t i = 1; i < response.Bids.size(); ++i) {
					runningTime += response.Times[i];

					if (runningTime >= 1000) {
						res.push_back(OHLCV_BidAsk {
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
							.multiplier = multiplier
						});

						currentBidRow.clear();
						currentAskRow.clear();
						runningBidVolume = 0;
						runningAskVolume = 0;
						runningTime -= 1000;
					}

					// Bids and asks are already expressed in multiplier units, so this is
					// exact integer arithmetic.
					runningBid += response.Bids[i];
					runningAsk += response.Asks[i];
					runningBidVolume += response.BidVolumes[i];
					runningAskVolume += response.AskVolumes[i];

					currentBidRow.push_back(runningBid);
					currentAskRow.push_back(runningAsk);
				}
			}

			Logger::Info(std::format("Day processed: {}", dt.ToString_Date()));
		}
		else {
			Logger::Warning(std::format("Day skipped: {}", dt.ToString_Date()));
		}

		return res;
	}

	static void ExportFullDay(const Common::DateTime& dt) {
		std::string ticksUrlBase = "https://jetta.dukascopy.com/v1/ticks/SPY.US-USD/" + 
			std::to_string(dt.GetYear()) + "/" +
			std::to_string(dt.GetMonth()) + "/" +
			std::to_string(dt.GetDay()) + "/";
		std::vector<TickResponse> tickResponses;
		Client::SimpleCurlWrapper curl;
		Client::CurlRequest hourRequest("");
			
		// Setup the callback and store the response in a vector of structs representing the data.
		hourRequest.SetCallback(
			[&](const std::string& response) {
				const Common::JsonUtility hourJson(response);
				TickResponse res{};
				hourJson.TryGet<Common::UnixTimestamp>("timestamp", res.Timestamp);
				hourJson.TryGet<float>("multiplier", res.Multiplier);
				hourJson.TryGet<float>("bid", res.Bid);
				hourJson.TryGet<float>("ask", res.Ask);
				hourJson.TryGet<std::vector<int32_t>>("times", res.Times);
				hourJson.TryGet<std::vector<int16_t>>("asks", res.Asks);
				hourJson.TryGet<std::vector<int16_t>>("bids", res.Bids);
				hourJson.TryGet<std::vector<int32_t>>("askVolumes", res.AskVolumes);
				hourJson.TryGet<std::vector<int32_t>>("bidVolumes", res.BidVolumes);

				if (res.Timestamp > 0 &&
					res.Multiplier > 0 &&
					res.Bid > 0 &&
					res.Ask > 0 &&
					res.Times.empty() == false &&
					res.Asks.empty() == false &&
					res.Bids.empty() == false &&
					res.AskVolumes.empty() == false &&
					res.BidVolumes.empty() == false) {
					tickResponses.push_back(res);
				}
			}
		);

		// Iterate through each hour of the trading day (13 - 19)
		for (size_t hour = 13; hour < 20; ++hour) {
			hourRequest.ReassignUri(std::format("{}{}", ticksUrlBase, std::to_string(hour)));
			curl.ExecuteHttpRequest(hourRequest);
		}

		std::vector<OHLCV_BidAsk> candleSticks = ExtractOneSecondCandlesticks(tickResponses, dt);

		// TODO: Create two different record files:
		// 1. Corpus OHLCV
		// 2. Delta Record
	}

	void BeginCorpusExport() {
		Logger::Info("========================================", true);
		Logger::Info("      Beginning full corpus export", true);
		Logger::Info("========================================", true);
		
		Common::JsonUtility spyData(std::string("{}"));

		{	
			//Get basic data to ensure that the endpoint is working
			Client::SimpleCurlWrapper curl;
			Client::CurlRequest infoRequest(infoUrl);
			infoRequest.SetCallback(
				[&](const std::string& response) {
					if (response.empty() == false) {
						spyData = Common::JsonUtility(response);
					}
					else {
						Logger::Error("Unable to access info URI. Terminating process.", true);
					}
				}
			);

			curl.ExecuteHttpRequest(infoRequest);
		}//end

		Logger::Info(std::format("Description: {}", spyData.Get<std::string>("description")), true);

		if (const Common::DateTime day(2026, Common::AUG, 24); day.IsWeekday()) {
				ExportFullDay(day);
		}
		else {
			Logger::Warning(std::format("Weekend skipped: {}", day.ToString_Date()));
		}
	}
}

/*
Process creates a file {yyyy-mm-dd.bin} saved under ~/src/storage/records
In each record:
float mid_delta     float spread_delta     uint32_t volume
[...]
*/