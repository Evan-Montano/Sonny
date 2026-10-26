// webullstream.hpp
// Keeps Webull environment and stream lifecycle decisions out of the UI.

#pragma once

#include <condition_variable>
#include <mutex>
#include <map>
#include <string>
#include <thread>

namespace Client {
    enum class WebullEnvironment { Live, Sandbox };

    struct WebullEnvironmentConfig {
        WebullEnvironment environment;
        const char* name;
        const char* api_host;
        const char* mqtt_host;
        const char* secret_environment;
    };

    const WebullEnvironmentConfig& GetWebullEnvironmentConfig(WebullEnvironment environment);
    std::string BuildWebullSubscriptionBody(const std::string& session_id);
    std::string GenerateWebullSignature(const std::string& path, const std::string& body,
                                        const std::string& app_key, const std::string& app_secret,
                                        const std::string& host, const std::string& timestamp,
                                        const std::string& nonce,
                                        const std::map<std::string, std::string>& query_parameters = {});

    enum class SubscriptionResultKind { Success, AuthenticationFailure, PermissionFailure, InvalidRequest, TransportFailure, ServerFailure };
    struct SubscriptionResult {
        SubscriptionResultKind kind = SubscriptionResultKind::TransportFailure;
        long http_status = 0;
        std::string diagnostic;
        explicit operator bool() const noexcept { return kind == SubscriptionResultKind::Success; }
    };

    /**
     * @brief Manages one Webull market-data stream and its worker lifetime.
     */
    class WebullStreamingClient {
    public:
        // CONSTRUCTOR

        /**
         * @brief Construct a stopped streaming client.
         */
        WebullStreamingClient() = default;

        /**
         * @brief Stop the stream and wait for its worker to release resources.
         */
        ~WebullStreamingClient();

        /**
         * @brief Prevent copying the worker and its connection ownership.
         */
        WebullStreamingClient(const WebullStreamingClient&) = delete;

        /**
         * @brief Prevent assigning another client's worker and connection ownership.
         */
        WebullStreamingClient& operator=(const WebullStreamingClient&) = delete;

        // METHODS

        /**
         * @brief Start a stream for the selected environment.
         *
         * @param environment Live or sandbox.
         * @return true if a worker was started; false if one is already active.
         */
        bool Start(WebullEnvironment environment);

        /**
         * @brief Request shutdown without waiting on network cleanup.
         */
        void RequestStop();

        /**
         * @brief Request shutdown and wait for the worker to finish cleanup.
         */
        void Stop();

        /**
         * @brief Check whether the streaming worker is active.
         *
         * @return true while the worker is active.
         */
        bool IsRunning() const;

    private:
        // METHODS

        /**
         * @brief Run connection, subscription, and reconnect work on the owned worker.
         *
         * @param environment Environment selected by the caller.
         */
        void Run(WebullEnvironment environment);

        // MEMBERS

        /**
         * @brief Protects worker lifecycle state shared with UI calls.
         */
        mutable std::mutex mutex_;

        /**
         * @brief Interrupts reconnect waits when shutdown is requested.
         */
        std::condition_variable cv_;

        /**
         * @brief Owns the connection and callback lifecycle.
         */
        std::thread worker_;

        /**
         * @brief Signals the worker to leave its loop and clean up.
         */
        bool stop_requested_ = false;

        /**
         * @brief Prevents duplicate stream workers from running concurrently.
         */
        bool running_ = false;
    };
}
