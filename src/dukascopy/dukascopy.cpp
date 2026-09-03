// dukascopy.cpp
// Implementation of the Dukascopy namespace for exporting historical raw tick data into a binary format used by Sonny's ML architecture.

#include "dukascopy.hpp"
#include "../common/jsonutil.hpp"
#include "../client/simplecurlwrapper.hpp"
#include "../common/logger.hpp"
#include <vector>
#include <format>
#include <array>
#include <cstdint>
#include <algorithm>

namespace Dukascopy {

	static inline const std::string infoUrl = "https://jetta.dukascopy.com/v1/instruments/SPY.US-USD";

	struct TickResponse {
		Common::UnixTimestamp Timestamp;
		float Multiplier;
		float Ask;
		float Bid;
		
		// These are deltas.
		std::vector<int32_t> Times;
		std::vector<int16_t> Asks;
		std::vector<int16_t> Bids;

		// Actual volume values.
		std::vector<int32_t> AskVolumes;
		std::vector<int32_t> BidVolumes;
	};

	struct MLRecord {
		std::uint32_t SecondsSinceOpen;
		float MidDelta;
		float SpreadDelta;
		std::uint32_t Volume;
	};

	bool ExportFullDay(const Common::DateTime& dt) {
		std::string ticksUrlBase = 
			"https://jetta.dukascopy.com/v1/ticks/SPY.US-USD/" + 
			std::to_string(dt.GetYear()) + "/" +
			std::to_string(dt.GetMonth()) + "/" +
			std::to_string(dt.GetDay()) + "/";
		bool res = false;
		Client::SimpleCurlWrapper curl;
		Common::JsonUtility spyData(std::string("{}"));

		{//Get basic data to ensure that the endpoint is working
			Client::CurlRequest infoRequest(infoUrl);
			infoRequest.SetCallback(
				[&](const std::string& response) {
					if (response.size() > 0) {
						Logger::Info("Response received.", true);
						spyData = Common::JsonUtility(response);
					}
					else {
						Logger::Error("Unable to access info URI. Terminating process.", true);
					}
				}
			);

			Logger::Info("Getting SPY data..", true);
			curl.ExecuteHttpRequest(infoRequest);
		}//end
		
		std::vector<TickResponse> tickResponses;

		if (spyData.Has<std::string>("description")) {
			Logger::Info(std::format("Description: {}", spyData.Get<std::string>("description")), true);
			Client::CurlRequest hourRequest("");
			
			// Setup the callback and store the response in a vector of structs representing the data.
			hourRequest.SetCallback(
				[&](const std::string& response) {
					Common::JsonUtility hourJson(response);
					Logger::Debug(hourJson.ToString());

					tickResponses.push_back(
						{
							hourJson.Get<Common::UnixTimestamp>("timestamp"),
							hourJson.Get<float>("multiplier"),
							hourJson.Get<float>("ask"),
							hourJson.Get<float>("bid"),
							hourJson.Get<std::vector<int32_t>>("times"),
							hourJson.Get<std::vector<int16_t>>("asks"),
							hourJson.Get<std::vector<int16_t>>("bids"),
							hourJson.Get<std::vector<int32_t>>("askVolumes"),
							hourJson.Get<std::vector<int32_t>>("bidVolumes")
						}
					);
				}
			);

			// Iterate through each hour of the trading day (13 - 19)
			for (size_t hour = 13; hour < 20; ++hour) {
				hourRequest.ReassignUri(std::format("{}{}", ticksUrlBase, std::to_string(hour)));
				curl.ExecuteHttpRequest(hourRequest);
			}

			// TODOing..: Consolidate into a single vector of MLRecord(s)
			// Throw them all into a record file.
			// Once that works, then we can set up corpus runs.
			Common::UnixTimestamp timestamp = tickResponses[0].Timestamp;

			for (const TickResponse& response : tickResponses) {
				float multiplier = response.Multiplier;

				// Dukascopy's price deltas are expressed in units of the multiplier.
				// Convert the initial bid into those same integer units so that all
				// reconstruction remains exact and we avoid floating-point drift.
				std::int64_t runningBid = static_cast<std::int64_t>(std::round(response.Bid / multiplier));
				int32_t runningTime = (response.Timestamp + response.Times[0]) % 1000;
				int32_t runningVolume = response.BidVolumes[0];

				std::vector<std::int64_t> currentRow;
				currentRow.push_back(runningBid);

				for (int i = 1; i < response.Bids.size(); ++i) {
					runningTime += response.Times[i];

					if (runningTime >= 1000) {
						// Finish previous candle.
						std::int64_t O = currentRow.front();
						std::int64_t H = *std::max_element(currentRow.begin(), currentRow.end());
						std::int64_t L = *std::min_element(currentRow.begin(), currentRow.end());
						std::int64_t C = currentRow.back();

						Logger::Info(std::format(
							"{}\t{}\t{}\t{}\t{}\t{}",
							Common::DateTime(++timestamp, true).ToString_Time(),
							O * multiplier,
							H * multiplier,
							L * multiplier,
							C * multiplier,
							runningVolume
						));

						currentRow.clear();
						runningVolume = 0;
						runningTime -= 1000;
					}

					// Bids are already expressed in multiplier units, so this is
					// exact integer arithmetic.
					runningBid += response.Bids[i];

					runningVolume += response.BidVolumes[i];
					currentRow.push_back(runningBid);
				}
			}
			
			res = true;
		}

		return res;
	}
}

/*
Order to call the api
https://jetta.dukascopy.com/v1/instruments/SPY.US-USD
https://jetta.dukascopy.com/v1/ticks/SPY.US-USD/2026/8/24/{13-19}

struct MLRecord {
	float    mid_delta;
	float    spread_delta;
	uint32_t volume;
};


Process creates a file {yyyy-mm-dd.bin} saved under ~/src/storage/records

In each record:
float mid_delta     float spread_delta     uint32_t volume
[...]

*/