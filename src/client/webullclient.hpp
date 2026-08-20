// webullclient.hpp
// WebullClient class definition for interacting with the Webull API.

/*

webullclient is going to have the following tasks:

1. Query all historical chart data for the S&P using the seconds-mini fetch endpoint in batches.
	a. Forward the chart data to the engine for processing & building of the custom database.

2. Maintain an open websocket connection to Webull.
	a. Get live S&P chart data and forward to the prediction engine.
	b. Execute live trades.

*/

#pragma once

#include <string>
#include <vector>
#include "curlrequest.hpp"
#include "../common/pricebar.hpp"

namespace Client {
	struct ChartInfoResponse {
		std::string TickerID;
		std::string ExchangeID;
		std::string ExchangeCode;
		std::string Type;
		std::string RegionID;
		std::string RegionCode;
		std::string CurrencyID;
		std::string CurrencyCode;
		std::string Name;
		std::string Symbol;
	};

	class WebullClient {
	public:
		// METHODS
		static ChartInfoResponse GetClosestChartInfo(const std::string &symbol);
		static Common::PriceBar GetSinglePriceBar(const std::string &tickerId, const Common::UnixTimestamp &timestamp);
		static void PopulatePriceBarRange(const std::string &tickerId, std::vector<Common::PriceBar> &bars, const Common::UnixTimestamp &timestamp, const uint16_t &count);
	
	private:
		// MEMBERS
		static ChartInfoResponse ResolveInfoResponse(const std::string &response);
		static const HeaderMap GetDefaultHeaders();

		// STATIC MEMBERS
		inline static const std::string SECONDS_MINI_URI = "https://quotes-gw.webullfintech.com/api/quote/charts/seconds-mini";
		inline static const std::string TICKERS_SEARCH_URI = "https://quotes-gw.webullfintech.com/api/search/pc/tickers";

		inline static constexpr uint8_t PRICE_SCALE = 100;
	};
	
}