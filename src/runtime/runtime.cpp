// runtime.cpp
// Implementation of the Orchestrator class and the runtime entry points started from the UI.

#include "runtime.hpp"
#include "client/webullstream.hpp"
#include "../ui/ui.hpp"
#include <ftxui/dom/elements.hpp>

#include <deque>

namespace Runtime {

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

        auto view = UI::Get().PushView(
            ftxui::CatchEvent(
                ftxui::Renderer([] {
                    return ftxui::window(
                        ftxui::text("Sandbox Webull Quote Stream") | ftxui::bold,
                        ftxui::vbox({
                            ftxui::text("Environment: Sandbox"),
                            ftxui::text("Status: Running"),
                            ftxui::text("Press Escape to return.")
                        })
                    );
                }),
                [this](ftxui::Event event) {
                    if (event == ftxui::Event::Escape) {
                        this->Running.store(false);
                        return true;
                    }

                    return false;
                }
            )
        );

        // Big TODO:
        // This section is going to be responsible for the stream and prediction pipeline. Rough draft of the process:
        // In a live runtime loop, the following is occuring continuously:
        // 1. Read, parse, and queue incoming quote ticks (separate thread to update UI quote stream display)
        // 2. Building one-second quote records
        // 3. Normalizing one-second quote records
        // 4. When >= 30 size vector is built and normalized, pass as a query vector into the appropriate HalfHourIndex
        // 5. Get results from the index query and pass into separate function for historical record lookup.
        // 6. Get results from historical lookup and calculate futures.
        // 7. Update futures display on UI.

        // std::deque<std::string> QuoteQueue{};
        

        this->QuoteClient.SetQuoteStreamCallback([&](Client::WebullQuote quote) {
            
        });

        this->QuoteClient.Start(this->EnvironmentType);
        UI::Get().Refresh();

        while (this->Running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        this->QuoteClient.RequestStop();
    }

    // ========== END ORCHESTRATOR IMPLEMENTATION ========== //
    
    void BeginSandboxRuntime() {
        Orchestrator orchestrator(Client::WebullEnvironment::Sandbox);
        orchestrator.Run();
    }   

}