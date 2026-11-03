// webullstream.cpp
// Binds signed HTTP subscriptions to the lifetime of a verified MQTT session.

#include "webullstream.hpp"

#include "mqttwrapper.hpp"
#include "simplecurlwrapper.hpp"
#include "../common/datetime.hpp"
#include "../common/jsonutil.hpp"
#include "../common/logger.hpp"
#include "../common/secretstore.hpp"
#include "webull/marketdata.pb.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <nlohmann/json.hpp>

#include <charconv>
#include <cmath>
#include <optional>
#include <utility>
#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

namespace Client {
    namespace {
        // Keep endpoint hosts beside the matching Secret Service scope to prevent mixed-environment requests.
        constexpr WebullEnvironmentConfig LiveConfig{
            WebullEnvironment::Live, "live", "api.webull.com", "data-api.webull.com", "live"
        };
        constexpr WebullEnvironmentConfig SandboxConfig{
            WebullEnvironment::Sandbox, "sandbox", "api.sandbox.webull.com", "data-api.sandbox.webull.com", "sandbox"
        };

        constexpr const char* SubscribePath = "/market-data/streaming/subscribe";
        constexpr const char* UnsubscribePath = "/market-data/streaming/unsubscribe";

        constexpr int MqttPort = 1883;

        // Webull authenticates the MQTT connection by App Key; the password is not checked.
        constexpr const char* MqttPassword = "sonny";

        constexpr std::chrono::seconds MqttKeepAlive{30};
        constexpr std::chrono::seconds ConnectionPollInterval{1};
        constexpr std::chrono::seconds InitialBackoff{1};
        constexpr std::chrono::seconds MaxBackoff{30};
        constexpr std::chrono::seconds HttpTimeout{15};
        constexpr std::chrono::seconds HttpConnectTimeout{8};

        constexpr std::size_t MaxDiagnosticLength = 240;

        /* ========================================================================================================== */

        /**
         * @brief Overwrite and release a string that held credential or signature bytes.
         */
        void Cleanse(std::string& value) {
            OPENSSL_cleanse(value.data(), value.size());
            value.clear();
        }

        /**
         * @brief Cleanses the referenced strings when the enclosing scope exits, on every return path.
         */
        class CleanseOnExit {
        public:
            CleanseOnExit(std::initializer_list<std::string*> values) : values_(values) {}
            ~CleanseOnExit() { for (std::string* value : values_) Cleanse(*value); }

            CleanseOnExit(const CleanseOnExit&) = delete;
            CleanseOnExit& operator=(const CleanseOnExit&) = delete;

        private:
            std::vector<std::string*> values_;
        };

        template <typename T>
        std::optional<T> ParseNumber(std::string_view input) {
            std::optional<T> result;

            if (input.empty() == false) {
                T value{};

                const auto [ptr, ec] = std::from_chars(
                    input.data(),
                    input.data() + input.size(),
                    value
                );

                if (ec == std::errc{} && ptr == input.data() + input.size())
                    result = value;
            }

            return result;
        }

        std::optional<WebullPriceLevel> ParsePriceLevel(const webull::marketdata::AskBid& level) {
            std::optional<WebullPriceLevel> result;

            std::optional<double> price = ParseNumber<double>(level.price());
            std::optional<double> size = ParseNumber<double>(level.size());

            bool valid = price.has_value() != false &&
                        size.has_value() != false;

            if (valid) {
                valid = std::isfinite(*price) != false &&
                        std::isfinite(*size) != false &&
                        *price > 0.0 &&
                        *size >= 0.0;
            }

            if (valid) {
                result = WebullPriceLevel{
                    .Price = *price,
                    .size = *size
                };
            }

            return result;
        }

        std::optional<WebullQuote> ParseWebullQuote(const std::string& payload) {
            std::optional<WebullQuote> result;

            webull::marketdata::Quote message;

            bool valid = payload.empty() == false;

            if (valid) valid = message.ParseFromString(payload);

            if (valid) valid = message.has_basic();

            WebullQuote quote{};

            if (valid) {
                const ::webull::marketdata::Basic& basic = message.basic();

                std::optional<long> timestamp = ParseNumber<std::int64_t>(basic.timestamp());

                valid = basic.symbol().empty() == false &&
                        basic.instrument_id().empty() == false &&
                        timestamp.has_value() != false &&
                        *timestamp > 0;

                if (valid) {
                    quote.symbol = basic.symbol();
                    quote.instrument_id = basic.instrument_id();
                    quote.timestamp_ms = *timestamp;
                    quote.session = basic.session();
                }
            }

            if (valid) {
                for (const webull::marketdata::AskBid& ask : message.asks()) {
                    std::optional<WebullPriceLevel> level = ParsePriceLevel(ask);

                    if (level.has_value() != false) quote.asks.push_back(*level);
                    else valid = false;
                }
            }

            if (valid) {
                for (const webull::marketdata::AskBid& bid : message.bids()) {
                    std::optional<WebullPriceLevel> level = ParsePriceLevel(bid);

                    if (level.has_value() != false) quote.bids.push_back(*level);
                    else valid = false;
                }
            }

            if (valid) result = std::move(quote);

            return result;
        }

    }

    /* ============================================================================================================== */

    struct WebullCredentials {
        std::string app_key;
        std::string app_secret;
        std::string access_token;

        // Minimize the time credential bytes remain in process memory.
        ~WebullCredentials() {
            Cleanse(app_key);
            Cleanse(app_secret);
            Cleanse(access_token);
        }
    };

    /* ============================================================================================================== */

    namespace {
        enum class SubscriptionAction { Subscribe, Unsubscribe };

        /* ========================================================================================================== */

        std::string UtcTimestamp() {
            return std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                               std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        }

        /* ========================================================================================================== */

        std::string NewId() {
            static constexpr char hex[] = "0123456789abcdef";
            std::random_device rd;
            std::string id(32, '0');
            for (char& c : id) c = hex[rd() & 0xf];
            return id;
        }

        /* ========================================================================================================== */

        std::string HexUpper(const unsigned char* bytes, unsigned length) {
            std::string hex;
            for (unsigned i = 0; i < length; ++i) hex += std::format("{:02X}", bytes[i]);
            return hex;
        }

        /* ========================================================================================================== */

        std::string ToLower(std::string value) {
            std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        bool ContainsAny(std::string_view text, std::initializer_list<std::string_view> needles) {
            return std::ranges::any_of(needles, [text](std::string_view needle) { return text.find(needle) != std::string_view::npos; });
        }

        void Redact(std::string& text, const std::string& secret) {
            if (secret.empty()) return;
            static constexpr std::string_view Placeholder = "[redacted]";
            for (std::size_t position = text.find(secret); position != std::string::npos;
                 position = text.find(secret, position + Placeholder.size())) {
                text.replace(position, secret.size(), Placeholder);
            }
        }

        /* ========================================================================================================== */

        /**
         * @brief Reduce an error response to fields that are safe to log.
         * Error bodies can echo request credentials, so only selected fields are exposed, after redaction.
         *
         * @param response Raw HTTP response body.
         * @param secrets Values that must never appear in the returned text.
         */
        std::string SafeDiagnostic(const std::string& response, std::initializer_list<const std::string*> secrets) {
            try {
                const Common::JsonUtility json(response);

                std::string code;
                for (const char* key : {"code", "error_code"}) {
                    nlohmann::json value;
                    if (json.TryGet(key, value) && (value.is_number() || value.is_boolean())) code = value.dump();
                }

                std::string message;
                for (const char* key : {"message", "msg", "error"}) {
                    if (json.TryGet(key, message)) break;
                }

                if (ContainsAny(ToLower(message), {"token", "secret", "signature", "authorization"})) {
                    message = "Sensitive authentication detail omitted.";
                }
                for (const std::string* secret : secrets) Redact(message, *secret);
                if (message.size() > MaxDiagnosticLength) message.resize(MaxDiagnosticLength);

                if (code.empty() && message.empty()) return "Response body omitted (no safe diagnostic fields).";
                return (code.empty() ? "" : "code=" + code + "; ") + message;
            } catch (...) {
                return "Response body omitted (not recognized as a safe JSON error).";
            }
        }

        /* ========================================================================================================== */

        SubscriptionResultKind ClassifyFailure(long status, const std::string& diagnostic) {
            const std::string lower = ToLower(diagnostic);
            if (status == 401 || ContainsAny(lower, {"authentication", "invalid app key"})) {
                return SubscriptionResultKind::AuthenticationFailure;
            }
            // A bare 403 cannot identify the cause; treat it as an entitlement issue unless auth detail is present.
            if (status == 403 || ContainsAny(lower, {"permission", "entitlement"})) {
                return SubscriptionResultKind::PermissionFailure;
            }
            if (status == 400 || status == 422) {
                return SubscriptionResultKind::InvalidRequest;
            }
            return SubscriptionResultKind::ServerFailure;
        }

        /**
         * @brief Failures that need a configuration or entitlement change rather than another attempt.
         */
        bool IsPermanentFailure(SubscriptionResultKind kind) {
            return kind == SubscriptionResultKind::AuthenticationFailure ||
                   kind == SubscriptionResultKind::PermissionFailure ||
                   kind == SubscriptionResultKind::InvalidRequest;
        }

        /**
         * @brief These CONNACK failures need configuration changes, not another retry.
         */
        bool IsPermanentMqttFailure(int reason_code) {
            static constexpr std::array<int, 6> PermanentReasons{1, 2, 3, 5, 103, 104};
            return std::ranges::find(PermanentReasons, reason_code) != PermanentReasons.end();
        }

        /* ========================================================================================================== */

        /**
         * @brief Send a signed subscribe or unsubscribe request for the given MQTT session.
         *
         * @param config Endpoints for the selected environment.
         * @param credentials Signing credentials for the selected environment.
         * @param session MQTT client ID the subscription is bound to.
         * @param action Subscribe or unsubscribe.
         * @return The classified HTTP outcome with a log-safe diagnostic.
         */
        SubscriptionResult PostSubscription(const WebullEnvironmentConfig& config, const WebullCredentials& credentials,
                                            const std::string& session, SubscriptionAction action) {
            const std::string path = action == SubscriptionAction::Subscribe ? SubscribePath : UnsubscribePath;
            // Compact serialization keeps the signed bytes identical to the transmitted JSON body.
            const std::string body = BuildWebullSubscriptionBody(session);
            const std::string timestamp = UtcTimestamp();
            const std::string nonce = NewId();

            std::string signature = GenerateWebullSignature(path, body, credentials.app_key, credentials.app_secret,
                                                            config.api_host, timestamp, nonce);
            std::string response;
            const CleanseOnExit cleanse{&signature, &response};

            if (signature.empty()) return {SubscriptionResultKind::TransportFailure, 0, "Request signing failed"};

            std::unique_ptr<SimpleCurlWrapper> http;
            try {
                http = std::make_unique<SimpleCurlWrapper>();
            } catch (const std::exception&) {
                return {SubscriptionResultKind::TransportFailure, 0, "libcurl initialization failed"};
            }

            CurlRequest request("https://" + std::string(config.api_host) + path);
            request.SensitiveData = true;
            request.SetBody(body);
            request.SetTimeouts(HttpTimeout, HttpConnectTimeout);
            request.AddHeader("Content-Type", "application/json");
            request.AddHeader("x-app-key", credentials.app_key);
            request.AddHeader("x-timestamp", timestamp);
            request.AddHeader("x-signature", signature);
            request.AddHeader("x-signature-algorithm", "HMAC-SHA1");
            request.AddHeader("x-signature-version", "1.0");
            request.AddHeader("x-signature-nonce", nonce);
            request.AddHeader("x-version", "v3");
            request.AddHeader("reqid", NewId());
            
            if (credentials.access_token.empty() == false) request.AddHeader("x-access-token", credentials.access_token);
            request.SetCallback([&response](std::string data) { response = std::move(data); });

            const bool sent = http->ExecuteHttpRequest(request) == CurlRequestEnum::SUCCESS;
            const long status = request.GetResponseCode();

            if (sent == false) {
                return {SubscriptionResultKind::TransportFailure, status, "HTTP transport failure: " + request.GetErrorMessage()};
            }
            if (status >= 200 && status < 300) {
                return {SubscriptionResultKind::Success, status, ""};
            }

            const std::string diagnostic = SafeDiagnostic(response, {&credentials.app_key, &credentials.app_secret,
                                                                     &credentials.access_token, &signature, &nonce, &session});
            return {ClassifyFailure(status, diagnostic), status, diagnostic};
        }

        /* ========================================================================================================== */

        const char* DescribeFailure(SubscriptionResultKind kind) {
            switch (kind) {
                case SubscriptionResultKind::AuthenticationFailure: return "authentication failure";
                case SubscriptionResultKind::PermissionFailure: return "market-data permission failure";
                case SubscriptionResultKind::InvalidRequest: return "invalid request";
                case SubscriptionResultKind::TransportFailure: return "transport failure";
                case SubscriptionResultKind::ServerFailure: break;
                case SubscriptionResultKind::Success: break;
            }
            return "server failure";
        }

        void LogSubscription(const char* verb, const SubscriptionResult& result) {
            const std::string prefix = std::format("Webull HTTP {} status={}", verb, result.http_status);
            if (result) {
                Logger::Info(prefix + " succeeded", true);
            } else {
                Logger::Error(std::format("{} failed ({}): {}", prefix, DescribeFailure(result.kind), result.diagnostic), true);
            }
        }

        /* ========================================================================================================== */

        std::string GetCredential(Common::SecretEnvironment environment, const std::string& name, bool required = true) {
            Common::SecretResult result = Common::SecretStore::GetWebullSecret(environment, name);

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

        /* ========================================================================================================== */

        /**
         * @brief Read the selected environment's credentials, stopping at the first missing required one.
         *
         * @return true if the App Key and App Secret were both found.
         */
        bool LoadCredentials(const WebullEnvironmentConfig& config, WebullCredentials& credentials) {
            const Common::SecretEnvironment secret_environment = config.environment == WebullEnvironment::Live
                ? Common::SecretEnvironment::Live : Common::SecretEnvironment::Sandbox;

            credentials.app_key = GetCredential(secret_environment, "app_key");
            if (credentials.app_key.empty()) return false;

            credentials.app_secret = GetCredential(secret_environment, "app_secret");
            if (credentials.app_secret.empty()) return false;

            // Token creation needs Webull's interactive verification, so only read a provisioned token.
            credentials.access_token = GetCredential(secret_environment, "access_token", false);
            return true;
        }
    }

    /* ============================================================================================================== */

    const WebullEnvironmentConfig& GetWebullEnvironmentConfig(WebullEnvironment environment) {
        return environment == WebullEnvironment::Live ? LiveConfig : SandboxConfig;
    }

    /* ============================================================================================================== */

    std::string BuildWebullSubscriptionBody(const std::string& session_id) {
        return nlohmann::json({
            {"session_id", session_id}, {"symbols", {"SPY"}}, {"category", "US_ETF"},
            {"sub_types", {"QUOTE"}}, {"grab", "true"}, {"overnight_required", false}
        }).dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
    }

    /* ============================================================================================================== */

    std::string GenerateWebullSignature(const std::string& path, const std::string& body,
                                        const std::string& app_key, const std::string& app_secret,
                                        const std::string& host, const std::string& timestamp,
                                        const std::string& nonce,
                                        const std::map<std::string, std::string>& query_parameters) {
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

        // Webull signs sorted query and header fields plus the hash of the exact body sent.
        std::string signing;
        for (const auto& [key, value] : values) {
            if (signing.empty() == false) signing += '&';
            signing += key + "=" + value;
        }
        const std::string encoded = SimpleCurlWrapper::EncodeUrl(path + "&" + signing + "&" + HexUpper(md5.data(), md5_len));

        std::string key = app_secret + "&";
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned digest_len = 0;
        const unsigned char* signed_result = HMAC(EVP_sha1(), key.data(), static_cast<int>(key.size()),
                                         reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size(),
                                         digest.data(), &digest_len);
        Cleanse(key);
        if (signed_result == nullptr) return {};

        std::array<unsigned char, 128> b64{};
        const int b64_len = EVP_EncodeBlock(b64.data(), digest.data(), static_cast<int>(digest_len));
        return b64_len > 0 ? std::string(reinterpret_cast<char*>(b64.data()), static_cast<std::size_t>(b64_len)) : std::string{};
    }

    /* ============================================================================================================== */

    WebullStreamingClient::~WebullStreamingClient() { Stop(); }

    /* ============================================================================================================== */

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

    /* ============================================================================================================== */

    void WebullStreamingClient::Stop() {
        RequestStop();
        if (worker_.joinable()) worker_.join();
    }

    /* ============================================================================================================== */

    void WebullStreamingClient::RequestStop() {
        {
            std::lock_guard lock(mutex_);
            if (running_ == false && worker_.joinable() == false) return;
            stop_requested_ = true;
        }
        cv_.notify_all();
    }

    /* ============================================================================================================== */

    bool WebullStreamingClient::IsRunning() const {
        std::lock_guard lock(mutex_);
        return running_;
    }

    /* ============================================================================================================== */

    bool WebullStreamingClient::WaitForStop(std::chrono::seconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return stop_requested_; });
    }

    /* ============================================================================================================== */

    bool WebullStreamingClient::StopRequested() const {
        std::lock_guard lock(mutex_);
        return stop_requested_;
    }

    /* ============================================================================================================== */

    void WebullStreamingClient::Run(WebullEnvironment environment) {
        const WebullEnvironmentConfig& config = GetWebullEnvironmentConfig(environment);
        Logger::Info(std::string("Webull environment selected: ") + config.name, true);

        WebullCredentials credentials;
        if (LoadCredentials(config, credentials)) {
            std::chrono::seconds backoff = InitialBackoff;

            while (StopRequested() == false) {
                if (RunSession(config, credentials, backoff) == SessionOutcome::GiveUp || StopRequested()) break;

                // Webull does not restore subscriptions after reconnect; the next session uses a fresh session ID.
                Logger::Warning(std::format("Webull reconnect attempt scheduled in {} seconds", backoff.count()), true);
                if (WaitForStop(backoff)) break;

                backoff = std::min(backoff * 2, MaxBackoff);
            }
        }

        {
            std::lock_guard lock(mutex_);
            running_ = false;
            stop_requested_ = false;
        }

        Logger::Info("Webull stream stopped; resources cleaned up", true);
    }

    /* ============================================================================================================== */

    WebullStreamingClient::SessionOutcome WebullStreamingClient::RunSession(const WebullEnvironmentConfig& config,
                                                                            const WebullCredentials& credentials,
                                                                            std::chrono::seconds& backoff) {
        const std::string session = NewId();
        const std::string uri = std::format("ssl://{}:{}", config.mqtt_host, MqttPort);
        Logger::Info("Webull MQTT connection attempt to " + uri, true);

        try {
            MqttWrapper mqtt(uri, session);
            mqtt.SetMessageHandler(
                [this](const std::string &topic, const std::string &payload) {
                    const std::string stamp = Common::DateTime::GetCurrentDateTime().ToString_DT();
                    Logger::Info(std::format("Webull message received at {} topic={} payload_bytes={}", stamp, topic, payload.size()), true);

                    if (topic == "quote" && this->QuoteStreamCallback) {
                        std::optional<WebullQuote> quote = ParseWebullQuote(payload);
                        if (quote) {
                            // Transfer ownership to the callback.
                            this->QuoteStreamCallback(std::move(*quote));
                        }
                        else {
                            Logger::Info("Failed to parse Webull quote payload.", true);
                        }
                    }
                }
            );
            mqtt.SetConnectionLostHandler([] { Logger::Warning("Webull MQTT connection lost", true); });
            mqtt.Connect({.UserName = credentials.app_key, .Password = MqttPassword, .KeepAlive = MqttKeepAlive});

            // Webull binds the HTTP subscription to this already-connected MQTT client ID.
            Logger::Info("Webull MQTT connected; awaiting HTTP subscription", true);

            const SubscriptionResult subscribed = PostSubscription(config, credentials, session, SubscriptionAction::Subscribe);
            LogSubscription("subscribe", subscribed);

            if (!subscribed) {
                mqtt.Disconnect();
                return IsPermanentFailure(subscribed.kind) ? SessionOutcome::GiveUp : SessionOutcome::Reconnect;
            }

            Logger::Info("Webull subscription accepted; waiting for market-data messages", true);
            backoff = InitialBackoff;

            // Poll the connection so a dropped broker link ends the session even without a stop request.
            while (WaitForStop(ConnectionPollInterval) == false && mqtt.IsConnected()) {}

            if (mqtt.IsConnected()) {
                LogSubscription("unsubscribe", PostSubscription(config, credentials, session, SubscriptionAction::Unsubscribe));
                mqtt.Disconnect();
            }
        } catch (const MqttException& error) {
            Logger::Error("Webull MQTT connection/stream failure; broker reason=" + std::to_string(error.ReasonCode), true);
            if (IsPermanentMqttFailure(error.ReasonCode)) {
                Logger::Error("Webull MQTT rejected the protocol or application credentials; stopping retries", true);
                return SessionOutcome::GiveUp;
            }
        } catch (const std::exception&) {
            Logger::Error("Webull streaming operation failed", true);
        }

        return SessionOutcome::Reconnect;
    }

    /* ============================================================================================================== */

    void WebullStreamingClient::SetQuoteStreamCallback(const std::function<void(WebullQuote)> &callback) {
        this->QuoteStreamCallback = std::move(callback);
    }

    /* ============================================================================================================== */
}
