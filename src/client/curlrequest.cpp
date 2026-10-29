// curlrequest.cpp
// Implementation of the CurlRequest class.

#include "curlrequest.hpp"

#include <cstring>

namespace Client {

    // CURL REQUEST IMPLEMENTATION //

    CurlRequest::CurlRequest(std::string uri) : Uri(std::move(uri)) {}

    CurlRequest::~CurlRequest() {
        if (this->SensitiveData) {
            this->WipeSensitiveData();
        }
    }

    void CurlRequest::AddHeader(const std::string &key, const std::string &value) {
        this->HeaderDictionary[key] = value;
    }

    void CurlRequest::AddHeader(const std::unordered_map<std::string, std::string> &kvp) {
        for (const auto &[key, value] : kvp) {
            this->AddHeader(key, value);
        }
    }

    void CurlRequest::RemoveHeader(const std::string &key) {
        this->HeaderDictionary.erase(key);
    }

    void CurlRequest::ClearHeaders() {
        if (this->SensitiveData) {
            for (auto &[key, value] : this->HeaderDictionary) {
                WipeString(value);
            }
        }
        this->HeaderDictionary.clear();
    }

    void CurlRequest::AddParam(const std::string &key, const std::string &value) {
        this->ParameterDictionary[key] = value;
        this->FormattedUri.clear();
    }

    void CurlRequest::AddParam(const std::unordered_map<std::string, std::string> &kvp) {
        for (const auto &[key, value] : kvp) {
            this->AddParam(key, value);
        }
    }

    void CurlRequest::RemoveParam(const std::string &key) {
        this->ParameterDictionary.erase(key);
        this->FormattedUri.clear();
    }

    void CurlRequest::ClearParams() {
        this->ParameterDictionary.clear();
        this->FormattedUri.clear();
    }

    void CurlRequest::Reset() {
        this->ClearHeaders();
        this->ClearParams();
        this->RemoveCallback();
        this->FormattedUri.clear();
        if (this->SensitiveData) {
            WipeString(this->Body);
        }
        this->Body.clear();
        this->Timeout = std::chrono::seconds{0};
        this->ConnectTimeout = std::chrono::seconds{0};
    }

    std::string& CurlRequest::GetFormattedUri() {
        // Build the formatted uri and store in class member
        std::string tmp = this->Uri;
        bool first = true;
        for (const auto& [key, value] : this->ParameterDictionary) {
            tmp += first ? "?" : "&";
            tmp += key + "=" + value;
            first = false;
        }
        this->FormattedUri = tmp;

        return this->FormattedUri;
    }

    void CurlRequest::SetCallback(const std::function<void(std::string)> &cb) {
        this->Callback = std::move(cb);
        this->CallbackSet = true;
    }

    void CurlRequest::RemoveCallback() {
        this->Callback = nullptr;
        this->CallbackSet = false;
    }

    void CurlRequest::InvokeCallback(const std::string &data) {
        if (this->Callback) {
            this->Callback(data);
        }
    }

    void CurlRequest::ReassignUri(const std::string &uri) {
        this->Uri = uri;
        this->FormattedUri.clear();
    }

    const std::unordered_map<std::string, std::string>& CurlRequest::GetHeaders() const {
        return this->HeaderDictionary;
    }

    void CurlRequest::SetBody(std::string body) {
        this->Body = std::move(body);
    }

    const std::string& CurlRequest::GetBody() const {
        return this->Body;
    }

    bool CurlRequest::HasBody() const {
        return this->Body.empty() == false;
    }

    void CurlRequest::SetTimeouts(const std::chrono::seconds total, const std::chrono::seconds connect) {
        this->Timeout = total;
        this->ConnectTimeout = connect;
    }

    std::chrono::seconds CurlRequest::GetTimeout() const {
        return this->Timeout;
    }

    std::chrono::seconds CurlRequest::GetConnectTimeout() const {
        return this->ConnectTimeout;
    }

    void CurlRequest::RecordResult(const long responseCode, std::string errorMessage) {
        this->ResponseCode = responseCode;
        this->ErrorMessage = std::move(errorMessage);
    }

    long CurlRequest::GetResponseCode() const {
        return this->ResponseCode;
    }

    const std::string& CurlRequest::GetErrorMessage() const {
        return this->ErrorMessage;
    }

    void CurlRequest::WipeString(std::string &value) {
        // Unlike memset, explicit_bzero is not removed by the optimizer when the string is freed right after.
        explicit_bzero(value.data(), value.size());
    }

    void CurlRequest::WipeSensitiveData() {
        for (auto &[key, value] : this->HeaderDictionary) {
            WipeString(value);
        }
        WipeString(this->Body);
        WipeString(this->_response);
    }

}
