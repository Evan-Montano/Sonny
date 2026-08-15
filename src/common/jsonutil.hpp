// jsonutil.hpp
// This class represents a single json structure with helper methods to access the data.

#pragma once

#include "nlohmann/json.hpp"
#include <stdexcept>

namespace Common {

    class JsonUtility {
    public:
        // CONSTRUCTOR
        JsonUtility(const std::string& jsonString) {
            try {
                this->jsonData = nlohmann::json::parse(jsonString);
            }
            catch (const nlohmann::json::parse_error& e) {
                // Handle parsing error
                throw std::invalid_argument("Invalid JSON string: " + std::string(e.what()));
            }
        }

        JsonUtility(const nlohmann::json& jsonData) {
            try {
                this->jsonData = jsonData;
            }
            catch (const nlohmann::json::type_error& e) {
                // Handle type error
                throw std::invalid_argument("Invalid JSON data: " + std::string(e.what()));
            }
        }

        // METHODS
        template<typename T>
        T Get(const std::string &key) const {
            T res;
            if (jsonData.contains(key)) {
                try {
                    res = jsonData.at(key).get<T>();
                }
                catch (const nlohmann::json::type_error& e) {
                    throw std::invalid_argument("Type error for key '" + key + "': " + std::string(e.what()));
                }
            } else {
                throw std::out_of_range("Key '" + key + "' not found in JSON data.");
            }
            return res;
        }

        JsonUtility At(std::size_t index) const {
            if (jsonData.is_array() == false) {
                throw::std::runtime_error("JSON value is not an array.");
            }

            if (index >= jsonData.size()) {
                throw std::out_of_range("Array index out of range.");
            }

            return JsonUtility(jsonData.at(index));
        }

        JsonUtility At(const std::string &key) const {
            if (jsonData.contains(key) == false) {
                throw std::out_of_range("Key '" + key + "' not found in JSON data.");
            }

            return JsonUtility(jsonData.at(key));
        }

        std::string ToString() const {
            return jsonData.dump();
        }

    private:
        // MEMBERS
        nlohmann::json jsonData;
    };

}