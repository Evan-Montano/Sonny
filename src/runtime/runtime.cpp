// runtime.cpp
// Implementation of the Orchestrator class and the runtime entry points started from the UI.
//
// Data flow (each stage lives in its own file under src/runtime/):
//   Webull quote stream            (stream thread)
//     → OneSecondAggregator        one gap-free MLRecord per second, same rules as the .rec export
//     → LiveInputWindow            input records + held-out horizon, shared with the loop below
//   Prediction loop                (this thread, every PREDICTION_INTERVAL)
//     → PredictionExperiment       queries inputs and immediately scores against the captured held-out future

#include "runtime.hpp"
#include "../ui/ui.hpp"
#include "core/inputconfig.hpp"
#include "common/datetime.hpp"
#include "liveinputwindow.hpp"
#include "onesecondaggregator.hpp"
#include "predictionexperiment.hpp"
#include "quoteview.hpp"

#include <ftxui/component/component.hpp>

#include <utility>

namespace Runtime {

    namespace {

        /**
         * @brief Joins the stream worker when Run's scope ends, including during exception unwinding.
         * The quote callback captures Run's locals, so the worker must stop before they are destroyed.
         */
        class StreamStopGuard {
        public:
            // CONSTRUCTOR

            explicit StreamStopGuard(Client::WebullStreamingClient& client) : StreamClient(client) {}
            StreamStopGuard(const StreamStopGuard&) = delete;
            StreamStopGuard& operator=(const StreamStopGuard&) = delete;

            // DESTRUCTOR

            ~StreamStopGuard() { StreamClient.Stop(); }

        private:
            // MEMBERS

            Client::WebullStreamingClient& StreamClient;
        };

    }

    // ========== ORCHESTRATOR IMPLEMENTATION ========== //

    // === CONSTRUCTOR === //
    Orchestrator::Orchestrator(const Client::WebullEnvironment environment)
    : EnvironmentType(environment) {}

    Orchestrator::~Orchestrator() {
        this->QuoteClient.RequestStop();
    }
    // === END CONSTRUCTOR === //

    // === METHODS === //
    void Orchestrator::Run() {
        this->Running.store(true);

        // --- View --- //
        // Declared first so it is torn down last, after the stream has stopped writing to it.
        QuoteViewState viewState{
            .Environment = this->EnvironmentType == Client::WebullEnvironment::Sandbox ? "SANDBOX" : "LIVE"
        };

        auto view = UI::Get().PushView(
            ftxui::CatchEvent(
                ftxui::Renderer([&viewState] { return RenderQuoteView(viewState); }),
                [this](const ftxui::Event& event) {
                    if (event == ftxui::Event::Escape) {
                        this->RequestStop();
                        return true;
                    }
                    return false;
                }
            )
        );

        // --- Pipeline --- //
        LiveInputWindow inputWindow;
        PredictionExperiment experiment;

        OneSecondAggregator aggregator(
            [&](const LiveRecord& record) {
                const std::size_t windowSize = inputWindow.Push(record);
                UI::Get().Update([&] { viewState.OnRecord(record, windowSize); });
            },
            [&] {
                inputWindow.Clear();
                UI::Get().Update([&] { viewState.OnWindowReset(); });
            }
        );

        // --- Quote stream (runs on the stream worker thread) --- //
        std::uint64_t quoteCount = 0;
        auto lastViewRefresh = std::chrono::steady_clock::time_point{};

        this->QuoteClient.SetQuoteStreamCallback([&](Client::WebullQuote quote) {
            ++quoteCount;

            const auto now = std::chrono::steady_clock::now();
            if (now - lastViewRefresh >= QUOTE_VIEW_REFRESH_INTERVAL) {
                lastViewRefresh = now;
                UI::Get().Update([&] { viewState.OnQuote(quote, quoteCount); });
            }

            aggregator.AddQuote(std::move(quote));
        });

        const StreamStopGuard streamStopGuard(this->QuoteClient);
        this->QuoteClient.Start(this->EnvironmentType);
        UI::Get().Refresh();

        // --- Index preload --- //
        // Single-window mode only: read the window's indexes now, while the stream is still filling
        // the first input window, so the first prediction does not have to wait for them.
        // With every window active they load on first use instead, as before.
        if (Core::USE_ALL_INPUT_WINDOWS == false) {
            experiment.Preload(Common::DateTime::GetCurrentTimestampMilliseconds());
        }

        // --- Prediction loop --- //
        // Each cycle queries only the input portion, then scores its already captured future.
        while (this->WaitForNextPredictionCycle()) {
            if (std::optional<LiveInputWindow::Snapshot> snapshot = inputWindow.TakeSnapshotIfUpdated()) {
                experiment.RunSnapshot(*snapshot);
            }
        }

        this->QuoteClient.Stop();
    }

    void Orchestrator::RequestStop() {
        {
            std::lock_guard lock(this->StopMutex);
            this->Running.store(false);
        }
        this->StopSignal.notify_all();
    }

    bool Orchestrator::WaitForNextPredictionCycle() {
        std::unique_lock lock(this->StopMutex);
        this->StopSignal.wait_for(lock, PREDICTION_INTERVAL, [this] { return this->Running.load() == false; });
        return this->Running.load();
    }
    // === END METHODS === //

    // ========== END ORCHESTRATOR IMPLEMENTATION ========== //

    void BeginSandboxRuntime() {
        Orchestrator orchestrator(Client::WebullEnvironment::Sandbox);
        orchestrator.Run();
    }

}
