// mqttwrapper.hpp
// A simple wrapper around the Paho MQTT client that keeps broker details out of feature code.

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace Client {

    /**
     * @brief Thrown when the broker or transport rejects an MQTT operation.
     * Carries the broker/library reason code so callers can decide whether a retry can help.
     */
    class MqttException : public std::runtime_error {
    public:
        // CONSTRUCTOR
        MqttException(const std::string& message, int reasonCode)
            : std::runtime_error(message), ReasonCode(reasonCode) {}

        // MEMBERS
        const int ReasonCode;
    };

    /**
     * @brief Connection settings applied when a client connects to the broker.
     */
    struct MqttConnectOptions {
        std::string UserName;
        std::string Password;
        std::chrono::seconds KeepAlive{30};
        bool CleanSession = true;

        /**
         * @brief Verify the broker certificate to reject untrusted endpoints.
         */
        bool VerifyServerCertificate = true;
    };

    using MqttMessageHandler = std::function<void(const std::string& topic, const std::string& payload)>;
    using MqttConnectionLostHandler = std::function<void()>;

    /**
     * @brief Owns one MQTT 3.1.1 client connection and forwards broker events to the registered handlers.
     * Handlers run on the Paho callback thread, so they should be set before Connect() and stay cheap.
     */
    class MqttWrapper {
    public:
        // CONSTRUCTOR
        MqttWrapper(std::string serverUri, std::string clientId);

        // DESTRUCTOR
        ~MqttWrapper();

        MqttWrapper(const MqttWrapper&) = delete;
        MqttWrapper& operator=(const MqttWrapper&) = delete;

        // METHODS

        /**
         * @brief Set the handler invoked for every message received from the broker.
         */
        void SetMessageHandler(MqttMessageHandler handler);

        /**
         * @brief Set the handler invoked when an established connection drops unexpectedly.
         */
        void SetConnectionLostHandler(MqttConnectionLostHandler handler);

        /**
         * @brief Connect to the broker and block until it acknowledges the connection.
         *
         * @throws MqttException If the broker rejects the connection or the transport fails.
         */
        void Connect(const MqttConnectOptions& options);

        /**
         * @brief Disconnect from the broker and block until the disconnect completes.
         *
         * @throws MqttException If the disconnect fails.
         */
        void Disconnect();

        /**
         * @brief Check whether the connection is established and has not been reported lost.
         */
        bool IsConnected() const;

        const std::string& GetServerUri() const;
        const std::string& GetClientId() const;

    private:
        class Impl;

        // MEMBERS
        std::string ServerUri;
        std::string ClientId;
        std::unique_ptr<Impl> impl;
    };

}
