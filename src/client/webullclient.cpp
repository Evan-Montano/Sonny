// webullclient.cpp
// Implementation of the WebullClient class for interacting with the Webull API.

#include "../common/secretstore.hpp"
#include "../common/jsonutil.hpp"
#include "simplecurlwrapper.hpp"
#include "webullclient.hpp"
#include <vector>

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
        const ParamMap params = {
            { "keyword", symbol },
            { "pageIndex", "1" },
            { "pageSize", "1" },
            { "brokerId", "8" }
        };
        
        CurlRequest request(TICKERS_SEARCH_URI);
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
    
    PriceBarResponse WebullClient::GetSinglePriceBar(const Common::UnixTimestamp &timestamp, const std::string &tickerId) {
        PriceBarResponse res{};
        const ParamMap params = {
            { "type", "s1" },
            { "count", "1" },
            { "timestamp", std::to_string(timestamp) },
            { "restorationType", "0" },
            { "tickerId", tickerId }
        };

        CurlRequest request(SECONDS_MINI_URI);
        request.AddParam(params);
        request.AddHeader(GetDefaultHeaders());

        request.SetCallback(
            [&](const std::string &response) {
                Common::JsonUtility jsonResponse(response);
                std::string data = jsonResponse.At(0).At("data").At(0).Get<std::string>();

                std::stringstream ss(data);
                std::string field;
                std::vector<std::string> fields;
                while (std::getline(ss, field, ',')) {
                    fields.push_back(field);
                }

                res.Timestamp = std::stoi(fields[0]);
            }
        );

        SimpleCurlWrapper scw;
        scw.ExecuteHttpRequest(request);

        return res;
    }
// [
//   {
//     "tickerId": 913243251,
//     "timeZone": "America/New_York",
//     "realPreClose": "776.34",
//     "hasMore": 1,
//     "exchangeStatus": false,
//     "dates": [
//       {
//         "type": "T",
//         "start": "09:30:01",
//         "end": "16:00:00",
//         "avgShow": 0
//       }
//     ],
//     "specialExchangeTimes": [],
//     "data": [
//       "1786651200,777.79,777.74,777.82,777.74,null,16343,null"
//     ],
//     "version": 1781756680044
//   }
// ]
}