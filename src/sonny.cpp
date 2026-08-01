// sonny.cpp
// Main entry point for the sonny project.

#include <iostream>
#include "client/curlrequest.hpp"
#include "client/simplecurlwrapper.hpp"
#include "common/jsonutil.hpp"

void TestingCallbackFunction(std::string s) {
    std::cout << "Raw json: " << std::endl << s << std::endl;
    // response, we need to get the data array:
    //[{"tickerId":913243251,"timeZone":"America/New_York","realPreClose":"747.03","hasMore":0,"exchangeStatus":true,"dates":[{"type":"T","start":"09:30:01","end":"16:00:00","avgShow":0}],"specialExchangeTimes":[],"data":[],"cleanTime":1785504600000,"cleanDuration":5,"version":1781756680044}]
    JsonUtility json(s);
    std::cout << "Parsed json: " << std::endl << json.ToString() << std::endl << std::endl;

    json = json.At(0);
    std::cout << "Parsed json at index 0: " << std::endl << json.ToString() << std::endl << std::endl;

    std::vector<std::string> data = json.Get<std::vector<std::string>>("data");
    std::cout << "Data array dump: " << std::endl;
    for (const std::string &item : data) {
        std::cout << "Data item: " << item << std::endl;
    }
}

int main() {
    const std::string url = "https://quotes-gw.webullfintech.com/api/quote/charts/seconds-mini";
    
    SimpleCurlWrapper scw;
    CurlRequest request(url);
    // request.AddParam(urlParams);
    // request.AddHeader(requestHeaders);

    request.SetCallback(TestingCallbackFunction);

    scw.ExecuteHttpRequest(request);
    
    return 0;
}