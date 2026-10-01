// jsonutil.hpp
// This class represents a single json structure with helper methods to access the data.

#pragma once

#include "nlohmann/json.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace Common {

    /**
     * @brief Utility class for accessing and manipulating a JSON structure.
     */
    class JsonUtility {
    public:
        // CONSTRUCTORS

        JsonUtility() : jsonData(nlohmann::json::object()) {}

        /**
         * @brief Construct from a JSON string.
         *
         * @param jsonString JSON data represented as a string.
         *
         * @throws std::invalid_argument If the string contains invalid JSON.
         */
        explicit JsonUtility(const std::string& jsonString) {
            try {
                jsonData = nlohmann::json::parse(jsonString);
            }
            catch (const nlohmann::json::parse_error& e) {
                throw std::invalid_argument(
                    "Invalid JSON string: " + std::string(e.what())
                );
            }
        }

        /**
         * @brief Construct from an existing JSON object.
         *
         * @param jsonData Existing JSON data.
         *
         * @return A JsonUtility containing the provided JSON data.
         */
        static JsonUtility FromJson(const nlohmann::json& jsonData) {
            return JsonUtility(jsonData, JsonTag{});
        }

        /**
         * @brief Construct by reading JSON from a file.
         *
         * @param path Path to the JSON file.
         *
         * @return A JsonUtility containing the JSON data from the file.
         *
         * @throws std::runtime_error If the file cannot be opened.
         * @throws std::invalid_argument If the file contains invalid JSON.
         */
        static JsonUtility FromFile(const std::filesystem::path& path) {
            std::ifstream file(path);

            if (file.is_open() == false) {
                throw std::runtime_error(
                    "Unable to open JSON file: " + path.string()
                );
            }

            try {
                nlohmann::json jsonData = nlohmann::json::parse(file);
                return JsonUtility(jsonData, JsonTag{});
            }
            catch (const nlohmann::json::parse_error& e) {
                throw std::invalid_argument(
                    "Invalid JSON file '" + path.string() + "': "
                    + std::string(e.what())
                );
            }
        }

        // METHODS

        /**
         * @brief Set a value associated with a key.
         *
         * @tparam K Type of the JSON key.
         * @tparam V Type of the JSON value.
         *
         * @param key JSON key.
         * @param value JSON value.
         *
         * @throws std::invalid_argument If the key or value cannot be stored.
         */
        template<typename K, typename V>
        void Set(const K& key, const V& value) {
            try {
                jsonData[key] = value;
            }
            catch (const nlohmann::json::type_error& e) {
                throw std::invalid_argument(
                    "Invalid JSON key or value: " + std::string(e.what())
                );
            }
        }

        /**
         * @brief Get a value associated with a key.
         *
         * @tparam T Type to convert the JSON value to.
         *
         * @param key JSON key.
         *
         * @return The value associated with the key.
         *
         * @throws std::out_of_range If the key does not exist.
         * @throws std::invalid_argument If the JSON value cannot be converted to T.
         */
        template<typename T>
        T Get(const std::string& key) const {
            if (jsonData.contains(key)) {
                try {
                    return jsonData.at(key).get<T>();
                }
                catch (const nlohmann::json::type_error& e) {
                    throw std::invalid_argument(
                        "Type error for key '" + key + "': "
                        + std::string(e.what())
                    );
                }
            }

            throw std::out_of_range(
                "Key '" + key + "' not found in JSON data."
            );
        }

        /**
         * @brief Get the root JSON value.
         *
         * @tparam T Type to convert the JSON value to.
         *
         * @return The root JSON value converted to T.
         *
         * @throws std::invalid_argument If the JSON value cannot be converted to T.
         */
        template<typename T>
        T Get() const {
            try {
                return jsonData.get<T>();
            }
            catch (const nlohmann::json::type_error& e) {
                throw std::invalid_argument(
                    "JSON type error: " + std::string(e.what())
                );
            }
        }

        /**
         * @brief Attempt to get a value associated with a key.
         *
         * @tparam T Type to convert the JSON value to.
         *
         * @param key JSON key.
         * @param out Output variable to receive the value.
         *
         * @return true if the key exists and the value can be converted to T,
         *         otherwise false.
         */
        template<typename T>
        bool TryGet(const std::string& key, T& out) const {
            if (jsonData.contains(key) == false) {
                return false;
            }

            try {
                out = jsonData.at(key).get<T>();
                return true;
            }
            catch (const nlohmann::json::type_error&) {
                return false;
            }
        }

        /**
         * @brief Access an element of a JSON array by index.
         *
         * @param index Array index.
         *
         * @return JsonUtility containing the selected array element.
         *
         * @throws std::runtime_error If the JSON value is not an array.
         * @throws std::out_of_range If the index is outside the array bounds.
         */
        JsonUtility At(std::size_t index) const {
            if (jsonData.is_array() == false) {
                throw std::runtime_error(
                    "JSON Error: Value is not an array."
                );
            }

            if (index >= jsonData.size()) {
                throw std::out_of_range(
                    "JSON Error: Array index out of range."
                );
            }

            return JsonUtility(jsonData.at(index), JsonTag{});
        }

        /**
         * @brief Access a JSON value by key.
         *
         * @param key JSON key.
         *
         * @return JsonUtility containing the selected JSON value.
         *
         * @throws std::out_of_range If the key does not exist.
         */
        JsonUtility At(const std::string& key) const {
            if (jsonData.contains(key) == false) {
                throw std::out_of_range(
                    "JSON Error: Key '" + key + "' not found in JSON data."
                );
            }

            return JsonUtility(jsonData.at(key), JsonTag{});
        }

        /**
        * @brief Remove a value associated with a key.
        *
        * @param key JSON key.
        * @return true if the key was removed.
        * @return false if the key did not exist.
        */
        bool Erase(const std::string& key) {
            if (jsonData.contains(key) == false) {
                return false;
            }

            jsonData.erase(key);
            return true;
        }

        /**
         * @brief Serialize the JSON data to a string.
         *
         * @return JSON data represented as a string.
         */
        std::string ToString() const {
            return jsonData.dump();
        }

    private:

        /**
         * @brief Tag type used to construct JsonUtility from existing JSON data.
         */
        struct JsonTag {};

        /**
         * @brief Construct from an existing JSON object internally.
         *
         * @param jsonData Existing JSON data.
         * @param tag Internal constructor tag.
         */
        JsonUtility(const nlohmann::json& jsonData, JsonTag)
            : jsonData(jsonData) {}

        // MEMBERS

        nlohmann::json jsonData;
    };

}