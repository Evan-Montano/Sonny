// curlrequest.hpp
// Class to be used while making requests using the simple curl wrapper.

#pragma once

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

        void AppendResponse(std::string_view chunk) {
            this->_response += chunk;
        }

        void FinishResponse() {
            if (this->CallbackSet && this->Callback) {
                this->InvokeCallback(this->_response);
            }
        }

        // MEMBERS
        bool ClearSettingsAfterUse = false;
        bool CallbackSet = false;

    private:
        // MEMBERS
        std::string Uri;
        std::string FormattedUri;
        std::string _response;
        HeaderMap HeaderDictionary;
        ParamMap ParameterDictionary;
        std::function<void(std::string)> Callback = nullptr;
    };

}