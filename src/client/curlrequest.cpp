// curlrequest.cpp
// Implementation of the CurlRequest class.

#include "curlrequest.hpp"
#include <algorithm>

// CURL REQUEST IMPLEMENTATION //

CurlRequest::CurlRequest(std::string uri) : Uri(std::move(uri)) {}

void CurlRequest::AddHeader(const std::string &key, const std::string &value) {
    this->HeaderDictionary[key] = value;
}

void CurlRequest::RemoveHeader(const std::string &key) {
    this->HeaderDictionary.erase(key);
}

void CurlRequest::ClearHeaders() {
    this->HeaderDictionary.clear();
}

void CurlRequest::AddParam(const std::string &key, const std::string &value) {
    this->ParameterDictionary[key] = value;
    this->FormattedUri.clear();
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

void CurlRequest::SetCallback(std::function<void(std::string)> cb) {
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

const std::unordered_map<std::string, std::string>& CurlRequest::GetHeaders() const {
    return this->HeaderDictionary;
}