// simplecurlwrapper.hpp
// A simple wrapper around libcurl for making requests.

#pragma once

#include <curl/curl.h>
#include "curlrequest.hpp"

namespace Client {

    class SimpleCurlWrapper {
    public:
        // CONSTRUCTOR
        SimpleCurlWrapper();

        // DESTRUCTOR
        ~SimpleCurlWrapper();

        // METHODS
        CurlRequestEnum ExecuteHttpRequest(CurlRequest &request);

    private:
        // MEMBERS
        CURL *curlHandle;
    };

}