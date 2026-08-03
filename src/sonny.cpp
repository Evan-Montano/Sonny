// sonny.cpp
// Main entry point for the sonny project.

#include <iostream>
#include "client/curlrequest.hpp"
#include "client/simplecurlwrapper.hpp"
#include "common/jsonutil.hpp"
#include "common/dtutil.hpp"
#include "common/secretstore.hpp"

void TestingCallbackFunction(std::string s) {
    JsonUtility json(s);
    json = json.At(0);
    std::vector<std::string> data = json.Get<std::vector<std::string>>("data");
    std::cout << "Data array dump: " << std::endl;
    for (const std::string &item : data) {
        std::cout << "Data item: " << item << std::endl;
    }

    DateTime dt = DateTime::FromUnixTimestamp(1785528000);
    std::cout << std::endl << "DateTime for timestamp 1785528000 is: " << dt.ToString() << std::endl;
}

int main() {
    const std::string url = "https://quotes-gw.webullfintech.com/api/quote/charts/seconds-mini";

    DateTime dt = {
        2026,
        JUL,
        31,
        PM(8), // 8 PM UTC, 4PM EST
        0,
        0
    };

    std::string ts = dt.ToUnixTimestampStr();
    std::cout << "Unix timestamp for " << dt.ToString() << " is: " << ts << std::endl;

    std::string deviceID = SecretStore::Get("did");

    const std::unordered_map<std::string, std::string> urlParams = {
        { "type", "s1" },
        { "count", "10" },
        { "timestamp", ts },
        { "restorationType", "0" },
        { "tickerId", "913243251" }
    };

    const std::unordered_map<std::string, std::string> requestHeaders = {
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
        { "did", deviceID },
        { "ph", "UNIX Chrome" },
        { "User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36" },
        { "ver", "6.5.3" },
        { "app-group", "broker" },
    };

    SimpleCurlWrapper scw;
    CurlRequest request(url);
    request.AddParam(urlParams);
    request.AddHeader(requestHeaders);
    request.SetCallback(TestingCallbackFunction);

    scw.ExecuteHttpRequest(request);
    
    return 0;
}

// Data array legend:
// 1785527998   746.56    746.61      746.67      746.47      null    3144    null
// VOLUME       OPEN      CLOSE       HIGH        LOW         TBD     VOL     TBD