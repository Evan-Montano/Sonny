// runtime.hpp
// Orchestrates the long-running live services (the Webull quote stream and the prediction loop)
// for as long as their UI view is open.

#pragma once

#include "../client/webullstream.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace Runtime {

    /**
     * @brief Owns the live pipeline:
     * quote stream → one-second records → rolling input window → periodic forecasts → evaluation.
     */
    class Orchestrator {
    public:
        // CONSTRUCTOR

        Orchestrator(const Client::WebullEnvironment environment);
        ~Orchestrator();
        Orchestrator(const Orchestrator&) = delete;
        Orchestrator& operator=(const Orchestrator&) = delete;

        // METHODS

        /**
         * @brief Show the quote view, start the stream, and run the prediction loop until Escape is pressed.
         * Blocks the calling (operation) thread for the lifetime of the view.
         */
        void Run();

        /**
         * @brief Ask Run to finish. Safe to call from any thread.
         */
        void RequestStop();

    private:
        // MEMBERS

        /**
         * @brief Time between prediction cycles.
         */
        static constexpr std::chrono::seconds PREDICTION_INTERVAL{30};

        /**
         * @brief Minimum time between quote-panel redraws; quotes arrive far faster than the UI needs.
         */
        static constexpr std::chrono::milliseconds QUOTE_VIEW_REFRESH_INTERVAL{200};

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

        /**
         * @brief Paired with StopSignal so RequestStop can wake the prediction loop immediately.
         */
        std::mutex StopMutex;

        /**
         * @brief Wakes the prediction loop's wait when a stop is requested.
         */
        std::condition_variable StopSignal;

        // METHODS

        /**
         * @brief Wait one PREDICTION_INTERVAL, returning early if a stop is requested.
         *
         * @return true if the loop should run another cycle.
         */
        bool WaitForNextPredictionCycle();
    };

    /**
     * @brief Initializes the sandbox-version orchestrator which will live in the scope of this method.
     *
     */
    void BeginSandboxRuntime();
}
