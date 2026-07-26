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
    const std::string url = "https://quotes-gw.webullfintech.com/api/quote/charts/seconds-mini";
    
    SimpleCurlWrapper scw;
    CurlRequest request(url);

    request.SetCallback(TestingCallbackFunction);

    scw.ExecuteHttpRequest(request);
    
    return 0;
}