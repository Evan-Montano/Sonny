// simplecurlwrapper.hpp
// A simple wrapper around libcurl for making requests.

#pragma once

#include "curlrequest.hpp"

#include <curl/curl.h>
#include <memory>
#include <mutex>
#include <string_view>

namespace Client {

    /**
     * @brief Executes HTTP requests through curl or the owned browser worker.
     */
    class SimpleCurlWrapper {
    public:
        // CONSTRUCTOR
        SimpleCurlWrapper(bool executeInBrowser = false);

        // DESTRUCTOR
        ~SimpleCurlWrapper();

        // METHODS
        CurlRequestEnum ExecuteHttpRequest(CurlRequest &request);

        static std::string EncodeUrl(std::string_view value);

        // MEMBERS

        /**
         * @brief Boolean to control whether the incoming requests need to be excuted in a live browser instance.
         * When true, a live browser running via a javascript process is active for the lifecycle of this object.
         * 
         */
        const bool ExecuteInBrowser;        

    private:
        // MEMBERS

        class BrowserProcess;

        CURL *CurlHandle;
        std::unique_ptr<BrowserProcess> BrowserWorker;
        std::mutex ExecutionMutex;
    };

}
