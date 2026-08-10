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


class WebullClient {
public:
	// METHODS
};