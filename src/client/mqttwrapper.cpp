// mqttwrapper.cpp
// Implementation of the MqttWrapper class.

#include "mqttwrapper.hpp"

#include <mqtt/async_client.h>

#include <atomic>
#include <utility>

namespace Client {

    namespace {
        /**
         * @brief Forwards Paho callback events to the handlers registered on the wrapper.
         */
        class CallbackBridge final : public virtual mqtt::callback {
        public:
            // METHODS
            void connected(const std::string&) override {
                Connected.store(true);
            }

            void connection_lost(const std::string&) override {
                Connected.store(false);
                if (OnConnectionLost) {
                    OnConnectionLost();
                }
            }

            void message_arrived(mqtt::const_message_ptr message) override {
                if (OnMessage) {
                    OnMessage(message->get_topic(), message->get_payload());
                }
            }

            // MEMBERS
            std::atomic<bool> Connected{false};
            MqttMessageHandler OnMessage;
            MqttConnectionLostHandler OnConnectionLost;
        };

        MqttException Translate(const mqtt::exception& error) {
            return MqttException(error.what(), error.get_reason_code());
        }
    }

    class MqttWrapper::Impl {
    public:
        // CONSTRUCTOR
        Impl(const std::string& serverUri, const std::string& clientId)
            : AsyncClient(serverUri, clientId) {
            AsyncClient.set_callback(Callback);
        }

        // MEMBERS
        // The callback is declared first so it outlives the client that references it.
        CallbackBridge Callback;
        mqtt::async_client AsyncClient;
    };

    MqttWrapper::MqttWrapper(std::string serverUri, std::string clientId)
        : ServerUri(std::move(serverUri)), ClientId(std::move(clientId)) {
        try {
            this->impl = std::make_unique<Impl>(this->ServerUri, this->ClientId);
        }
        catch (const mqtt::exception& error) {
            throw Translate(error);
        }
    }

    MqttWrapper::~MqttWrapper() = default;

    void MqttWrapper::SetMessageHandler(MqttMessageHandler handler) {
        this->impl->Callback.OnMessage = std::move(handler);
    }

    void MqttWrapper::SetConnectionLostHandler(MqttConnectionLostHandler handler) {
        this->impl->Callback.OnConnectionLost = std::move(handler);
    }

    void MqttWrapper::Connect(const MqttConnectOptions& options) {
        try {
            const mqtt::ssl_options ssl = mqtt::ssl_options_builder()
                .enable_server_cert_auth(options.VerifyServerCertificate)
                .finalize();

            const mqtt::connect_options connectOptions = mqtt::connect_options_builder()
                .mqtt_version(MQTTVERSION_3_1_1)
                .keep_alive_interval(options.KeepAlive)
                .clean_session(options.CleanSession)
                .user_name(options.UserName)
                .password(options.Password)
                .ssl(ssl)
                .finalize();

            this->impl->AsyncClient.connect(connectOptions)->wait();
        }
        catch (const mqtt::exception& error) {
            throw Translate(error);
        }
    }

    void MqttWrapper::Disconnect() {
        try {
            this->impl->AsyncClient.disconnect()->wait();
        }
        catch (const mqtt::exception& error) {
            throw Translate(error);
        }
    }

    bool MqttWrapper::IsConnected() const {
        return this->impl->AsyncClient.is_connected() && this->impl->Callback.Connected.load();
    }

    const std::string& MqttWrapper::GetServerUri() const {
        return this->ServerUri;
    }

    const std::string& MqttWrapper::GetClientId() const {
        return this->ClientId;
    }

}
