// runtime.hpp
// TODO

#pragma once

#include "../client/webullstream.hpp"
#include <atomic>

namespace Runtime {

    class Orchestrator {
    public:
        // CONSTRUCTOR

        Orchestrator(const Client::WebullEnvironment environment);
        ~Orchestrator();
        Orchestrator(const Orchestrator&) = delete;
        Orchestrator& operator=(const Orchestrator&) = delete;

        // METHODS

        /**
         * @brief Starts the quote stream service.
         * 
         */
        void Run();

    private:
        // MEMBERS

        /**
         * @brief Member to track the environment type.
         * 
         */
        Client::WebullEnvironment EnvironmentType;

        /**
         * @brief Webull tcp client to deliver raw quote data streams.
         * Only one instance can be running per obj.
         */
        Client::WebullStreamingClient QuoteClient;

        /**
         * @brief Atomic wrapper to control the lifetime of the orchestrator in unison with the UI.
         * 
         */
        std::atomic<bool> Running = false;
    };

    /**
     * @brief Initializes the sandbox-version orchestrator which will live in the scope of this method.
     * 
     */
    void BeginSandboxRuntime();
}