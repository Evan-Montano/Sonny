// webullclient.cpp
// Implementation of the WebullClient class for interacting with the Webull API.

#include "../common/secretstore.hpp"
#include "../common/jsonutil.hpp"
#include "simplecurlwrapper.hpp"
#include "webullclient.hpp"

namespace Client {

    const HeaderMap WebullClient::GetDefaultHeaders() {
        HeaderMap res{};
        const std::string deviceID = Common::SecretStore::Get("Did");
        if (deviceID.size() > 0) {
            res = {
                { "appid", "wb_web_app" },
                { "sec-ch-ua-platform", "\"Linux\"" },
                { "device-type", "Web" },
                { "hl", "en" },
                { "sec-ch-ua", "\"Not;A=Brand\";v=\"8\", \"Chromium\";v=\"150\", \"Brave\";v=\"150\"" },
                { "sec-ch-ua-mobile", "?0" },
                { "app", "global" },
                { "os", "web" },
                { "platform", "web" },
                { "Referer", "https://app.webull.com/" },
                { "osv", "i9zh" },
                { "did", deviceID },
                { "ph", "UNIX Chrome" },
                { "User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36" },
                { "ver", "6.5.3" },
                { "app-group", "broker" },
            };
        }
        return res;
    }

    ChartInfoResponse WebullClient::GetClosestChartInfo(const std::string &symbol) {
        ChartInfoResponse result;
        static const std::string url = "https://quotes-gw.webullfintech.com/api/search/pc/tickers";
        const std::string deviceID = Common::SecretStore::Get("Did");
        const ParamMap params = {
            { "keyword", symbol },
            { "pageIndex", "1" },
            { "pageSize", "1" },
            { "brokerId", "8" }
        };
        
        CurlRequest request(url);
        request.AddParam(params);
        request.AddHeader(GetDefaultHeaders());
        request.SetCallback(
            [&](const std::string& response)
            {
                result = ResolveInfoResponse(response);
            }
        );

        SimpleCurlWrapper scw;
        scw.ExecuteHttpRequest(request);

        return result;
    }
    
    ChartInfoResponse WebullClient::ResolveInfoResponse(const std::string &response) {
        ChartInfoResponse res;
        if (response.size() > 0) {
            Common::JsonUtility data(response);
            data = data.At("data").At(0);

            res.TickerID = std::to_string(data.Get<unsigned int>("tickerId"));
            res.ExchangeID = std::to_string(data.Get<unsigned int>("exchangeId"));
            res.ExchangeCode = data.Get<std::string>("exchangeCode");
            res.Type = std::to_string(data.Get<unsigned int>("type"));
            res.RegionID = std::to_string(data.Get<unsigned int>("regionId"));
            res.RegionCode = data.Get<std::string>("regionCode");
            res.Name = data.Get<std::string>("name");
            res.Symbol = data.Get<std::string>("symbol");
        }
        return res;
    }

// Data returned by GetClosestChartInfo fetch:
// {
//   "data": [
//     {
//       "assetType": 1,
//       "currencyCode": "USD",
//       "currencyId": 247,
//       "derivativeSupport": 1,
//       "disExchangeCode": "NYSEARCA",
//       "disSymbol": "SPY",
//       "exchangeCode": "PSE",
//       "exchangeId": 34,
//       "exchangeTrade": true,
//       "isAdr": 0,
//       "isLeveraged": 0,
//       "isPTP": 0,
//       "leverageFactor": "0",
//       "listStatus": 1,
//       "name": "State Street® SPDR® S&P 500® ETF Trust",
//       "nightTradeSession": 0,
//       "oddLotSupport": true,
//       "overnightTradeFlag": 1,
//       "regionCode": "US",
//       "regionId": 6,
//       "secType": [
//         1,
//         34
//       ],
//       "secType2": 1,
//       "securitySubType": 701,
//       "securityType": 7,
//       "shariahFlag": 0,
//       "specialType": 34,
//       "symbol": "SPY",
//       "template": "etf",
//       "tickerId": 913243251,
//       "tinyName": "State Street® SPDR® S&P 500® ETF Trust",
//       "type": 3
//     }
//   ],
//   "hasMore": true
// }
}