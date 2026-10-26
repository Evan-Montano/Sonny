// webullstream.cpp
// Binds signed HTTP subscriptions to the lifetime of a verified MQTT session.

#include "webullstream.hpp"

#include "../common/logger.hpp"
#include "../common/secretstore.hpp"

#include <mqtt/async_client.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <ctime>
#include <format>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

namespace Client {
    namespace {
        // Keep endpoint hosts beside the matching Secret Service scope to prevent mixed-environment requests.
        constexpr WebullEnvironmentConfig LiveConfig{
            WebullEnvironment::Live, "live", "api.webull.com", "data-api.webull.com", "live"
        };
        constexpr WebullEnvironmentConfig SandboxConfig{
            WebullEnvironment::Sandbox, "sandbox", "api.sandbox.webull.com", "data-api.sandbox.webull.com", "sandbox"
        };

        struct Credentials {
            std::string app_key;
            std::string app_secret;
            std::string access_token;
            ~Credentials() {
                // Minimize the time credential bytes remain in process memory.
                auto wipe = [](std::string& value) {
                    OPENSSL_cleanse(value.data(), value.size());
                    value.clear();
                };
                wipe(app_key);
                wipe(app_secret);
                wipe(access_token);
            }
        };

        std::string UtcTimestamp() {
            const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm tm{};
            gmtime_r(&now, &tm);
            std::ostringstream out;
            out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            return out.str();
        }

        std::string NewId() {
            static constexpr char hex[] = "0123456789abcdef";
            std::random_device rd;
            std::string id(32, '0');
            for (char& c : id) c = hex[rd() & 0xf];
            return id;
        }

        std::string UrlEncode(std::string_view value) {
            CURL* curl = curl_easy_init();
            if (!curl) return {};
            char* encoded = curl_easy_escape(curl, value.data(), static_cast<int>(value.size()));
            std::string result = encoded != nullptr ? encoded : "";
            if (encoded) curl_free(encoded);
            curl_easy_cleanup(curl);
            return result;
        }

        std::string HexUpper(const unsigned char* bytes, unsigned length) {
            std::ostringstream out;
            out << std::uppercase << std::hex << std::setfill('0');
            for (unsigned i = 0; i < length; ++i) out << std::setw(2) << static_cast<unsigned>(bytes[i]);
            return out.str();
        }

        std::string SignRequest(const std::string& path, const std::string& body,
                                const std::string& app_key, const std::string& app_secret,
                                const std::string& host, const std::string& timestamp,
                                const std::string& nonce,
                                const std::map<std::string, std::string>& query_parameters = {}) {
            std::array<unsigned char, EVP_MAX_MD_SIZE> md5{};
            unsigned md5_len = 0;
            if (EVP_Digest(body.data(), body.size(), md5.data(), &md5_len, EVP_md5(), nullptr) != 1) return {};
            std::map<std::string, std::string> values = query_parameters;
            values["host"] = host;
            values["x-app-key"] = app_key;
            values["x-signature-algorithm"] = "HMAC-SHA1";
            values["x-signature-nonce"] = nonce;
            values["x-signature-version"] = "1.0";
            values["x-timestamp"] = timestamp;
            std::string signing;
            // Webull signs sorted query and header fields plus the hash of the exact body sent.
            for (const auto& [key, value] : values) {
                if (signing.empty() == false) signing += '&';
                signing += key + "=" + value;
            }
            const std::string to_encode = path + "&" + signing + "&" + HexUpper(md5.data(), md5_len);
            const std::string encoded = UrlEncode(to_encode);
            std::string key = app_secret + "&";
            std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
            unsigned digest_len = 0;
            const auto* signed_result = HMAC(EVP_sha1(), key.data(), static_cast<int>(key.size()),
                     reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size(),
                     digest.data(), &digest_len);
            OPENSSL_cleanse(key.data(), key.size());
            if (signed_result == nullptr) return {};
            std::array<unsigned char, 128> b64{};
            const int b64_len = EVP_EncodeBlock(b64.data(), digest.data(), static_cast<int>(digest_len));
            return b64_len > 0 ? std::string(reinterpret_cast<char*>(b64.data()), static_cast<std::size_t>(b64_len)) : std::string{};
        }

        size_t CollectBody(char* data, size_t size, size_t count, void* context) {
            static_cast<std::string*>(context)->append(data, size * count);
            return size * count;
        }

        std::string SafeDiagnostic(std::string_view response, const Credentials& credentials,
                                   const std::string& signature, const std::string& nonce,
                                   const std::string& session) {
            try {
                // Error bodies can echo request credentials, so expose only selected fields after redaction.
                const auto json = nlohmann::json::parse(response);
                std::string code;
                std::string message;
                for (const char* key : {"code", "error_code"}) {
                    if (json.contains(key) && (json[key].is_number() || json[key].is_boolean())) code = json[key].dump();
                }
                for (const char* key : {"message", "msg", "error"}) {
                    if (json.contains(key) && json[key].is_string()) { message = json[key].get<std::string>(); break; }
                }
                std::string lower = message;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find("token") != std::string::npos || lower.find("secret") != std::string::npos ||
                    lower.find("signature") != std::string::npos || lower.find("authorization") != std::string::npos) {
                    message = "Sensitive authentication detail omitted.";
                }
                for (const std::string* sensitive : {&credentials.app_key, &credentials.app_secret,
                                                       &credentials.access_token, &signature, &nonce, &session}) {
                    if (!sensitive->empty()) {
                        std::size_t position = 0;
                        while ((position = message.find(*sensitive, position)) != std::string::npos) {
                            message.replace(position, sensitive->size(), "[redacted]");
                            position += 10;
                        }
                    }
                }
                if (message.size() > 240) message.resize(240);
                if (code.empty() && message.empty()) return "Response body omitted (no safe diagnostic fields).";
                return (code.empty() ? "" : "code=" + code + "; ") + message;
            } catch (...) {
                return "Response body omitted (not recognized as a safe JSON error).";
            }
        }

        /**
         * @brief 
         * 
         * @param config 
         * @param credentials 
         * @param session 
         * @param unsubscribe 
         * @return SubscriptionResult 
         */
        SubscriptionResult PostSubscription(const WebullEnvironmentConfig& config, const Credentials& credentials,
                                             const std::string& session, bool unsubscribe) {
            static const bool curl_initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
            if (curl_initialized == false) return {SubscriptionResultKind::TransportFailure, 0, "libcurl initialization failed"};
            const std::string path = unsubscribe ? "/market-data/streaming/unsubscribe" : "/market-data/streaming/subscribe";
            // Compact serialization keeps the signed bytes identical to the transmitted JSON body.
            const std::string body = BuildWebullSubscriptionBody(session);
            const std::string timestamp = UtcTimestamp();
            const std::string nonce = NewId();
            std::string signature = SignRequest(path, body, credentials.app_key, credentials.app_secret,
                                                config.api_host, timestamp, nonce);
            if (signature.empty()) return {SubscriptionResultKind::TransportFailure, 0, "Request signing failed"};

            struct curl_slist* headers = nullptr;
            auto add = [&](const std::string& value) { headers = curl_slist_append(headers, value.c_str()); };
            add("Content-Type: application/json");
            add("x-app-key: " + credentials.app_key);
            add("x-timestamp: " + timestamp);
            add("x-signature: " + signature);
            add("x-signature-algorithm: HMAC-SHA1");
            add("x-signature-version: 1.0");
            add("x-signature-nonce: " + nonce);
            add("x-version: v3");
            add("reqid: " + NewId());
            if (credentials.access_token.empty() == false) add("x-access-token: " + credentials.access_token);

            CURL* curl = curl_easy_init();
            if (!curl) { 
                curl_slist_free_all(headers); 
                return {SubscriptionResultKind::TransportFailure, 0, "Unable to create HTTP request"}; 
            }

            std::string response;
            const std::string url = "https://" + std::string(config.api_host) + path;
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CollectBody);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
            const CURLcode curl_result = curl_easy_perform(curl);
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            for (struct curl_slist* header = headers; header != nullptr; header = header->next) {
                if (header->data != nullptr) OPENSSL_cleanse(header->data, std::strlen(header->data));
            }
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            if (curl_result != CURLE_OK) {
                OPENSSL_cleanse(signature.data(), signature.size());
                OPENSSL_cleanse(response.data(), response.size());
                return {SubscriptionResultKind::TransportFailure, status,
                        std::string("HTTP transport failure: ") + curl_easy_strerror(curl_result)};
            }
            if (status >= 200 && status < 300) {
                OPENSSL_cleanse(signature.data(), signature.size());
                OPENSSL_cleanse(response.data(), response.size());
                return {SubscriptionResultKind::Success, status, ""};
            }

            const std::string diagnostic = SafeDiagnostic(response, credentials, signature, nonce, session);
            OPENSSL_cleanse(signature.data(), signature.size());
            OPENSSL_cleanse(response.data(), response.size());
            std::string lower = diagnostic;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            SubscriptionResultKind kind = SubscriptionResultKind::ServerFailure;
            if (status == 401 || lower.find("authentication") != std::string::npos || lower.find("invalid app key") != std::string::npos) {
                kind = SubscriptionResultKind::AuthenticationFailure;
            }
            // A bare 403 cannot identify the cause; treat it as an entitlement issue unless auth detail is present.
            else if (status == 403 || lower.find("permission") != std::string::npos || lower.find("entitlement") != std::string::npos) {
                kind = SubscriptionResultKind::PermissionFailure;
            }
            else if (status == 400 || status == 422) kind = SubscriptionResultKind::InvalidRequest;
            return {kind, status, diagnostic};
        }

        void LogSubscription(const char* verb, const SubscriptionResult& result) {
            const std::string prefix = std::string("Webull HTTP ") + verb + " status=" + std::to_string(result.http_status);
            if (result) Logger::Info(prefix + " succeeded", true);
            else {
                const char* kind = "server failure";
                switch (result.kind) {
                    case SubscriptionResultKind::AuthenticationFailure: kind = "authentication failure"; break;
                    case SubscriptionResultKind::PermissionFailure: kind = "market-data permission failure"; break;
                    case SubscriptionResultKind::InvalidRequest: kind = "invalid request"; break;
                    case SubscriptionResultKind::TransportFailure: kind = "transport failure"; break;
                    case SubscriptionResultKind::ServerFailure: break;
                    case SubscriptionResultKind::Success: break;
                }
                Logger::Error(prefix + " failed (" + kind + "): " + result.diagnostic, true);
            }
        }

        /**
         * @brief Callback class inheriting mqtt callback interface for handling quote messages.
         * 
         */
        class StreamCallback final : public virtual mqtt::callback {
        public:
            void connected(const std::string&) override { connected_.store(true); }
            void connection_lost(const std::string&) override {
                connected_.store(false);
                Logger::Warning("Webull MQTT connection lost", true);
            }
            void message_arrived(mqtt::const_message_ptr message) override {
                const auto now = std::chrono::system_clock::now();
                const auto time = std::chrono::system_clock::to_time_t(now);
                std::tm local{};
                localtime_r(&time, &local);
                char stamp[32]{};
                std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %Z", &local);
                // Quote payloads are protobuf; logging bytes avoids presenting undecoded data as quote fields.
                Logger::Info(std::format("Webull message received at {} topic={} payload_bytes={}", stamp,
                                         message->get_topic(), message->get_payload().size()), true);
            }
            bool Connected() const { return connected_.load(); }
        private:
            std::atomic<bool> connected_{false};
        };

        std::string GetCredential(Common::SecretEnvironment environment, const std::string& name, bool required = true) {
            auto result = Common::SecretStore::GetWebullSecret(environment, name);
            if (!result) {
                if (result.error == Common::SecretError::Missing && required) {
                    Logger::Error("Webull credential missing: " + name + ". Provision it with secret-tool; see docs/webull-streaming.md.", true);
                } else if (result.error != Common::SecretError::Missing) {
                    Logger::Error("Webull credential lookup failed for " + name + ": " + result.detail, true);
                }
                return {};
            }
            return std::move(result.value);
        }
    }

    const WebullEnvironmentConfig& GetWebullEnvironmentConfig(WebullEnvironment environment) {
        return environment == WebullEnvironment::Live ? LiveConfig : SandboxConfig;
    }

    std::string BuildWebullSubscriptionBody(const std::string& session_id) {
        return nlohmann::json({
            {"session_id", session_id}, {"symbols", {"SPY"}}, {"category", "US_ETF"},
            {"sub_types", {"QUOTE"}}, {"grab", "true"}, {"overnight_required", false}
        }).dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
    }

    std::string GenerateWebullSignature(const std::string& path, const std::string& body,
                                        const std::string& app_key, const std::string& app_secret,
                                        const std::string& host, const std::string& timestamp,
                                        const std::string& nonce,
                                        const std::map<std::string, std::string>& query_parameters) {
        return SignRequest(path, body, app_key, app_secret, host, timestamp, nonce, query_parameters);
    }

    WebullStreamingClient::~WebullStreamingClient() { Stop(); }

    bool WebullStreamingClient::Start(WebullEnvironment environment) {
        std::lock_guard lock(mutex_);
        if (running_) {
            Logger::Warning("A Webull streaming session is already active", true);
            return false;
        }
        if (worker_.joinable()) worker_.join();
        stop_requested_ = false;
        running_ = true;
        Logger::Info(std::string("Webull streaming operation requested; environment=") + GetWebullEnvironmentConfig(environment).name, true);
        worker_ = std::thread(&WebullStreamingClient::Run, this, environment);
        return true;
    }

    void WebullStreamingClient::Stop() {
        RequestStop();
        if (worker_.joinable()) worker_.join();
    }

    void WebullStreamingClient::RequestStop() {
        {
            std::lock_guard lock(mutex_);
            if (!running_ && !worker_.joinable()) return;
            stop_requested_ = true;
        }
        cv_.notify_all();
    }

    bool WebullStreamingClient::IsRunning() const {
        std::lock_guard lock(mutex_);
        return running_;
    }

    void WebullStreamingClient::Run(WebullEnvironment environment) {
        const auto& config = GetWebullEnvironmentConfig(environment);

        Logger::Info(std::string("Webull environment selected: ") + config.name, true);

        const auto secret_environment = environment == WebullEnvironment::Live
            ? Common::SecretEnvironment::Live : Common::SecretEnvironment::Sandbox;

        Credentials credentials;
        credentials.app_key = GetCredential(secret_environment, "app_key");
        if (credentials.app_key.empty()) goto done;
        credentials.app_secret = GetCredential(secret_environment, "app_secret");
        if (credentials.app_secret.empty()) goto done;

        // Token creation needs Webull's interactive verification, so only read a provisioned token.
        credentials.access_token = GetCredential(secret_environment, "access_token", false);

        {
            std::chrono::seconds backoff{1};
            bool permanently_failed = false;
            while (permanently_failed == false) {
                {
                    std::lock_guard lock(mutex_);
                    if (stop_requested_) break;
                }
                const std::string session = NewId();
                const std::string uri = "ssl://" + std::string(config.mqtt_host) + ":1883";
                Logger::Info("Webull MQTT connection attempt to " + uri, true);

                try {
                    StreamCallback callback;
                    mqtt::async_client client(uri, session);
                    client.set_callback(callback);

                    mqtt::ssl_options ssl = mqtt::ssl_options_builder()
                        // Verify the broker certificate to reject untrusted endpoints.
                        .enable_server_cert_auth(true)
                        .finalize();
                    mqtt::connect_options options = mqtt::connect_options_builder()
                        .mqtt_version(MQTTVERSION_3_1_1)
                        .keep_alive_interval(std::chrono::seconds(30))
                        .clean_session(true)
                        .user_name(credentials.app_key)
                        .password("sonny")
                        .ssl(ssl)
                        .finalize();
                    
                    client.connect(options)->wait();

                    // Webull binds the HTTP subscription to this already-connected MQTT client ID.
                    Logger::Info("Webull MQTT connected; awaiting HTTP subscription", true);

                    const auto subscribed = PostSubscription(config, credentials, session, false);
                    LogSubscription("subscribe", subscribed);
                    
                    if (!subscribed) {
                        permanently_failed = subscribed.kind == SubscriptionResultKind::AuthenticationFailure ||
                            subscribed.kind == SubscriptionResultKind::PermissionFailure ||
                            subscribed.kind == SubscriptionResultKind::InvalidRequest;
                        client.disconnect()->wait();
                        if (permanently_failed) break;
                    } else {
                        Logger::Info("Webull subscription accepted; waiting for market-data messages", true);
                        backoff = std::chrono::seconds(1);

                        while (true) {
                            std::unique_lock lock(mutex_);
                            if (cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_requested_; })) break;
                            lock.unlock();
                            if (client.is_connected() == false || callback.Connected() == false) break;
                        }

                        if (client.is_connected()) {
                            const auto unsubscribed = PostSubscription(config, credentials, session, true);
                            LogSubscription("unsubscribe", unsubscribed);
                            client.disconnect()->wait();
                        }
                    }
                } catch (const mqtt::exception& error) {
                    const int reason = error.get_reason_code();
                    Logger::Error("Webull MQTT connection/stream failure; broker reason=" + std::to_string(reason), true);
                    // These CONNACK failures need configuration changes, not another retry.
                    permanently_failed = reason == 1 || reason == 2 || reason == 3 || reason == 5 ||
                        reason == 103 || reason == 104;
                    if (permanently_failed) Logger::Error("Webull MQTT rejected the protocol or application credentials; stopping retries", true);
                } catch (const std::exception& error) {
                    (void)error;
                    Logger::Error("Webull streaming operation failed", true);
                }

                // Webull does not restore subscriptions after reconnect; the next loop uses a fresh session ID.
                {
                    std::unique_lock lock(mutex_);
                    if (stop_requested_ || permanently_failed) break;
                    Logger::Warning("Webull reconnect attempt scheduled in " + std::to_string(backoff.count()) + " seconds", true);
                    if (cv_.wait_for(lock, backoff, [this] { return stop_requested_; })) break;
                }
                
                backoff = std::min(backoff * 2, std::chrono::seconds(30));
            }
        }

    done:
        {
            std::lock_guard lock(mutex_);
            running_ = false;
            stop_requested_ = false;
        }
        Logger::Info("Webull stream stopped; resources cleaned up", true);
    }
}
