// runtime.cpp
// TODO

#include "runtime.hpp"
#include "client/webullstream.hpp"
#include "../ui/ui.hpp"
#include <ftxui/dom/elements.hpp>

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