// curlrequest.hpp
// Class to be used while making requests using the simple curl wrapper.

#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <functional>

namespace Client {

    enum class CurlRequestEnum {
        SUCCESS,
        UNKNOWN
    };

    using ParamMap = std::unordered_map<std::string, std::string>;
    using HeaderMap = std::unordered_map<std::string, std::string>;

    class CurlRequest {
    public:
        // CONSTRUCTOR
        CurlRequest(std::string uri);

        // DESTRUCTOR
        ~CurlRequest();

        // METHODS
        void AddHeader(const std::string &key, const std::string &value);
        void AddHeader(const HeaderMap &kvp);
        void RemoveHeader(const std::string &key);
        void ClearHeaders();
        void AddParam(const std::string &key, const std::string &value);
        void AddParam(const ParamMap &kvp);
        void RemoveParam(const std::string &key);
        void ClearParams();
        void Reset();
        void SetCallback(const std::function<void(std::string)> &cb);
        void RemoveCallback();
        void InvokeCallback(const std::string &data);
        void ReassignUri(const std::string &uri);
        std::string& GetFormattedUri();
        const HeaderMap& GetHeaders() const;

        /**
         * @brief Set a request body. A request with a body is sent as a POST.
         * Not supported when the wrapper executes requests in a browser.
         */
        void SetBody(std::string body);
        const std::string& GetBody() const;
        bool HasBody() const;

        /**
         * @brief Limit how long the request may take. Zero leaves the libcurl default (no limit).
         *
         * @param total Limit for the entire transfer.
         * @param connect Limit for the connection phase only.
         */
        void SetTimeouts(std::chrono::seconds total, std::chrono::seconds connect);
        std::chrono::seconds GetTimeout() const;
        std::chrono::seconds GetConnectTimeout() const;

        /**
         * @brief Store the outcome of the last execution. Called by the wrapper after each request.
         *
         * @param responseCode HTTP status returned by the server, or 0 if none was received.
         * @param errorMessage Transport error description, or empty on success.
         */
        void RecordResult(long responseCode, std::string errorMessage);
        long GetResponseCode() const;
        const std::string& GetErrorMessage() const;

        void AppendResponse(std::string_view chunk) {
            this->_response += chunk;
        }

        void FinishResponse() {
            if (this->CallbackSet && this->Callback) {
                this->InvokeCallback(this->_response);
            }
            if (this->SensitiveData) {
                WipeString(this->_response);
            }
            this->_response.clear();
        }

        /**
         * @brief Overwrite a string's bytes so they do not linger in freed memory.
         */
        static void WipeString(std::string &value);

        // MEMBERS
        bool ClearSettingsAfterUse = false;
        bool CallbackSet = false;

        /**
         * @brief When true, header values, the body, and buffered responses are wiped from memory
         * once they are no longer needed. Use for requests that carry credentials.
         */
        bool SensitiveData = false;

    private:
        // METHODS
        void WipeSensitiveData();

        // MEMBERS
        std::string Uri;
        std::string FormattedUri;
        std::string _response;
        std::string Body;
        HeaderMap HeaderDictionary;
        ParamMap ParameterDictionary;
        std::function<void(std::string)> Callback = nullptr;
        std::chrono::seconds Timeout{0};
        std::chrono::seconds ConnectTimeout{0};
        long ResponseCode = 0;
        std::string ErrorMessage;
    };

}
