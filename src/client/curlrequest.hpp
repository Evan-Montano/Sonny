// curlrequest.hpp
// Class to be used while making requests using the simple curl wrapper.

#pragma once

#include <string>
#include <unordered_map>
#include <functional>

enum class CurlRequestEnum {
    SUCCESS,
    UNKNOWN
};

class CurlRequest {
public:
    // CONSTRUCTOR
    CurlRequest(std::string uri);

    // METHODS
    void AddHeader(const std::string &key, const std::string &value);
    void AddHeader(const std::unordered_map<std::string, std::string> &kvp);
    void RemoveHeader(const std::string &key);
    void ClearHeaders();
    void AddParam(const std::string &key, const std::string &value);
    void AddParam(const std::unordered_map<std::string, std::string> &kvp);
    void RemoveParam(const std::string &key);
    void ClearParams();
    void Reset();
    void SetCallback(const std::function<void(std::string)> &cb);
    void RemoveCallback();
    void InvokeCallback(const std::string &data);
    void ReassignUri(const std::string &uri);
    std::string& GetFormattedUri();
    const std::unordered_map<std::string, std::string>& GetHeaders() const;

    // MEMBERS
    bool ClearSettingsAfterUse = false;
    bool CallbackSet = false;

private:
    // MEMBERS
    std::string Uri;
    std::string FormattedUri;
    std::unordered_map<std::string, std::string> HeaderDictionary;
    std::unordered_map<std::string, std::string> ParameterDictionary;
    std::function<void(std::string)> Callback = nullptr;
};