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

namespace Dukascopy {

	static inline const std::string infoUrl = "https://jetta.dukascopy.com/v1/instruments/SPY.US-USD";

	struct TickResponse {
		Common::UnixTimestamp Timestamp;
		float Multiplier;
		float Ask;
		float Bid;
		std::vector<int16_t> Times;
		std::vector<int16_t> Asks;
		std::vector<int16_t> Bids;
		std::vector<int16_t> AskVolumes;
		std::vector<int16_t> BidVolumes;
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
					Logger::Debug(hourJson.ToString(), true);

					tickResponses.push_back(
						{
							hourJson.Get<Common::UnixTimestamp>("timestamp"),
							hourJson.Get<float>("multiplier"),
							hourJson.Get<float>("ask"),
							hourJson.Get<float>("bid"),
							hourJson.Get<std::vector<int16_t>>("times"),
							hourJson.Get<std::vector<int16_t>>("asks"),
							hourJson.Get<std::vector<int16_t>>("bids"),
							hourJson.Get<std::vector<int16_t>>("askVolumes"),
							hourJson.Get<std::vector<int16_t>>("bidVolumes")
						}
					);
				}
			);

			// Iterate through each hour of the trading day (13 - 19)
			for (size_t hour = 13; hour < 20; ++hour) {
				hourRequest.ReassignUri(std::format("{}{}", ticksUrlBase, std::to_string(hour)));
				curl.ExecuteHttpRequest(hourRequest);
			}

			// TODO: Consolidate into a single vector of MLRecord(s)
			// Throw them all into a record file.
			// Once that works, then we can set up corpus runs.

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
	uint32_t seconds_since_open;
	float    mid_delta;
	float    spread_delta;
	uint32_t volume;
};


Process creates a file {yyyymmdd.bin} saved under ~/src/storage/records

In each record:
float mid_delta     float spread_delta     uint32_t volume
[...]

*/