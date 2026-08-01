// simplecurlwrapper.cpp
// Implementation of the SimpleCurlWrapper class.

#include <stdexcept>
#include "simplecurlwrapper.hpp"

namespace {
    class CurlGlobal {
    public:
        // CONSTRUCTOR
        CurlGlobal() {
            CURLcode result = curl_global_init(CURL_GLOBAL_ALL);
            if (result != CURLE_OK) {
                throw std::runtime_error("Curl global initialization failed.");
            }
        }

        // DESTRUSTOR
        ~CurlGlobal() {
            curl_global_cleanup();
        }

        static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
            CurlRequest *request = static_cast<CurlRequest*>(userdata);
            size_t bytes = size * nmemb;
            
            if (request) {
                request->AppendResponse(std::string_view(ptr, bytes));
            }
            return bytes;
        }
    };
};

// SIMPLE CURL WRAPPER IMPLEMENTATION //

SimpleCurlWrapper::~SimpleCurlWrapper() {
    curl_easy_cleanup(this->curlHandle);
}

SimpleCurlWrapper::SimpleCurlWrapper() {
    static CurlGlobal curlGlobal;
    this->curlHandle = curl_easy_init();
    if (this->curlHandle == nullptr) {
        throw std::runtime_error("Curl handle initialization failed.");
    }
}

// Basic process:
// 1. Validate CurlRequest object.
// 2. Ensure the formatted uri is set up.
// 3. Reset the curl handle.
// 4. Configure the curl handle: url, headers, callback function.
// 5. Call curl_easy_perform.
// 6. Cleanup request object if specified.
// 7. Handle any errors and return appropriate enum.
CurlRequestEnum SimpleCurlWrapper::ExecuteHttpRequest(CurlRequest &request) {
    // 1. TODO: Validate the CurlRequest object where necessary. Right now nothing really needs to be done.
    
    // 2. Get reference to the formatted uri.
    const std::string& uriWithParams = request.GetFormattedUri();

    // 3. Reset curl handle.
    curl_easy_reset(this->curlHandle);

    // 4. Configure the curl handle.
    curl_easy_setopt(this->curlHandle, CURLOPT_URL, uriWithParams.data()); // set url
    curl_easy_setopt(this->curlHandle, CURLOPT_CA_CACHE_TIMEOUT, 604800L);

    // Set headers
    struct curl_slist *list = NULL;
    for (const auto& [key, value] : request.GetHeaders()) {
        std::string headerStr = key + ": " + value;
        list = curl_slist_append(list, headerStr.data());
    }
    curl_easy_setopt(this->curlHandle, CURLOPT_HTTPHEADER, list);

    // set callback function
    if (request.CallbackSet) {
        curl_easy_setopt(this->curlHandle, CURLOPT_WRITEFUNCTION, CurlGlobal::write_callback);
        curl_easy_setopt(this->curlHandle, CURLOPT_WRITEDATA, &request);
    }

    // 5. Call easy perform.
    CURLcode result = curl_easy_perform(this->curlHandle);

    if (result == CURLE_OK) {
        request.FinishResponse();
    }

    // 6. Cleanup request object.
    if (request.ClearSettingsAfterUse) {
        request.Reset();
    }
    curl_slist_free_all(list);

    // 7. Handle errors
    CurlRequestEnum res;
    switch (result) {
        case CURLcode::CURLE_OK:
            res = CurlRequestEnum::SUCCESS;
            break;
        default:
            res = CurlRequestEnum::UNKNOWN;
            break;
    }

    return res;
}
