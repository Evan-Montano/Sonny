// sonny.cpp
// Main entry point for the sonny project.

#include <iostream>
#include "client/curlrequest.hpp"
#include "client/simplecurlwrapper.hpp"

void TestingCallbackFunction(std::string s) {
    std::cout << "Callback invoked" << std::endl;
    std::cout << s << std::endl;
}

int main() {
    const std::string url = "https://quotes-gw.webullfintech.com/api/quote/charts/seconds-mini?type=s1&count=800&timestamp=1784574544&restorationType=0&tickerId=913243251";
    
    SimpleCurlWrapper scw;
    CurlRequest request(url);
    request.SetCallback(TestingCallbackFunction);

    scw.ExecuteHttpRequest(request);
    
    return 0;
}