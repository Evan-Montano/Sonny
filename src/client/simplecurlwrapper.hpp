// simplecurlwrapper.hpp
// A simple wrapper around libcurl for making requests.

#pragma once

#include <curl/curl.h>
#include "curlrequest.hpp"

namespace Client {

    class SimpleCurlWrapper {
    public:
        // CONSTRUCTOR
        SimpleCurlWrapper(const bool &executeInBrowser = false);

        // DESTRUCTOR
        ~SimpleCurlWrapper();

        // METHODS
        CurlRequestEnum ExecuteHttpRequest(CurlRequest &request);

        // MEMBERS

        /**
         * @brief Boolean to control whether the incoming requests need to be excuted in a live browser instance.
         * When true, a live browser running via a javascript process is active for the lifecycle of this object.
         * 
         */
        bool ExecuteInBrowser;

    private:
        // MEMBERS
        CURL *curlHandle;
    };

}