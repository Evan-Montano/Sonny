// ui.hpp
// Static class API for UI operations, using Meyer's Singleton pattern.

#pragma once

#include "../common/logger.hpp"
#include "../core/setup.hpp"
#include "../runtime/runtime.hpp"

#include <ftxui/dom/direction.hpp>
#include <ftxui/ftxui.hpp>
#include <thread>

using namespace ftxui;

class UI {
private:
    // MEMBERS
    App screen = ScreenInteractive::Fullscreen();

    /**
     * @brief Menu state
     * 
     */
    int current_menu = 0;

    /**
     * @brief Selected item in each menu.
     * 
     */
    int main_selected = 0;
    int sandbox_selected = 0;
    int setup_selected = 0;

    // Main menu
    std::vector<std::string> main_entries = {
        "Sandbox",
        "Setup",
        "Exit"
    };

    // Sandbox menu
    std::vector<std::string> sandbox_entries = {
        "Sandbox Webull SPY Quote Stream",
        "Back"
    };

    // Setup menu
    std::vector<std::string> setup_entries = {
        "Run All Setup Processes (Clears Everything)",
        "Download Historical Records (Clears Existing)",
        "Download Missing Records (Keeps Existing)",
        "Update Indexes",
        "Back"
    };

    MenuOption main_menu_options;
    MenuOption sandbox_menu_options;
    MenuOption setup_menu_options;

    using RenderFunction = std::function<ftxui::Element()>;
    RenderFunction temporary_renderer;
    ftxui::Component temporary_component;
    int temporary_previous_menu = 0;

    // CONSTRUCTOR
    UI() = default;
    UI(const UI&) = delete;
    UI &operator=(const UI&) = delete;
    UI(UI&&) = delete;
    UI &operator=(UI&&) = delete;

    // METHODS

    /**
     * @brief Once an operation thread is finished, this restores the previous menu.
     * 
     */
    void RestorePreviousMenu() {
        this->temporary_renderer = nullptr;
        this->temporary_component.reset();
        this->current_menu = this->temporary_previous_menu;
        this->screen.PostEvent(Event::Custom);
    }

    /**
     * @brief Used to start a separate working thread for all operations.
     * 
     * @param callback 
     */
    void DispatchOperation(std::function<void()> callback) {
        std::thread([cb = std::move(callback)] {
            cb();
        }).detach();
    }

public:
    // CONSTRUCTOR
    static UI &Get() {
        static UI instance;
        return instance;
    }

    class ViewGuard {
    private:
        // MEMBERS
        UI *ui;

    public:
        // CONSTRUCTOR
        explicit ViewGuard(UI *ui)
            : ui(ui) {}
        
        ~ViewGuard() {
            if (ui) {
                ui->RestorePreviousMenu();
            }
        }

        ViewGuard(const ViewGuard&) = delete;
        ViewGuard &operator=(const ViewGuard&) = delete;

        ViewGuard(ViewGuard &&other) noexcept
            : ui(other.ui) {
                other.ui = nullptr;
            }
        
        ViewGuard &operator=(ViewGuard &&other) noexcept {
            if (this != &other) {
                if (ui) {
                    ui->RestorePreviousMenu();
                }

                ui = other.ui;
                other.ui = nullptr;
            }

            return *this;
        }
    };

    // METHODS
    void RenderMenu() {
        this->main_menu_options.on_enter = [&] {
            switch (this->main_selected) {
                case 0:
                    // Sandbox
                    this->current_menu = 1;
                    break;
                case 1:
                    // Setup
                    this->current_menu = 2;
                    break;

                case 2:
                    // Exit
                    this->screen.ExitLoopClosure()();
                    break;

                default:
                    break;
            }
        };

        auto main_menu =
        Menu(
            &this->main_entries,
            &this->main_selected,
            this->main_menu_options
        );

        this->sandbox_menu_options.on_enter = [&] {
            switch(this->sandbox_selected) {
                case 0:
                    // Start sandbox spy quote stream.
                    DispatchOperation(&Runtime::BeginSandboxRuntime);
                    break;
                case 1:
                    // Back to main menu
                    this->current_menu = 0;
                    break;
                default:
                    break;
            }
        };

        auto sandbox_menu = 
        Menu(
            &this->sandbox_entries,
            &this->sandbox_selected,
            this->sandbox_menu_options
        );


        this->setup_menu_options.on_enter = [&] {
            switch (this->setup_selected) {
                case 0:
                    // Total setup process
                    DispatchOperation(&Core::Setup::ExecuteTotalSetupProcess);
                    break;

                case 1:
                    // Download all record files
                    DispatchOperation(&Core::Setup::ExecuteDownloadAllRecordFiles);
                    break;

                case 2:
                    // Download missing records, keep existing
                    DispatchOperation(&Core::Setup::ExecuteDownloadAllMissingRecordFiles);
                    break;

                case 3:
                    // Create vector files
                    DispatchOperation(&Core::Setup::ExecuteUpdateIndexes);
                    break;

                case 4:
                    // Back to main menu
                    this->current_menu = 0;
                    break;

                default:
                    break;
            }
        };

        auto setup_menu =
        Menu(
            &this->setup_entries,
            &this->setup_selected,
            this->setup_menu_options
        );

        // Temporary view
        auto temporary_view = Renderer([&] {
            if (this->temporary_renderer) {
                return this->temporary_renderer();
            }

            return text("No temporary view.");
        });

        // Menu container
        auto menus = Container::Tab(
            {
                main_menu,
                sandbox_menu,
                setup_menu,
                temporary_view
            },
            &this->current_menu
        );

        // Console window
        auto console_view = Renderer([&] {
            const auto messages = Logger::GetConsoleMessages();

            Elements lines;

            for (const auto& message : messages) {
                lines.push_back(text(message));
            }

            if (lines.empty()) {
                lines.push_back(text("Console is empty."));
            }

            return window(
                text("Console") | bold,
                vbox(std::move(lines))
            );
        });

        // UI
        auto renderer = Renderer(menus, [&] {
            Element main_view;

            if (this->current_menu == 3) {
                main_view = temporary_view->Render();
            }
            else {
                std::string title;
                switch (this->current_menu) {
                    case 0: title = "Menu"; break;
                    case 1: title = "Sandbox"; break;
                    case 2: title = "Setup"; break;
                    default: title = "Sonny"; break;
                }
                main_view = window(
                    text(title) | bold,
                    vbox({
                        menus->Render()
                    })
                );
            }

            return vbox({
                text("Sonny") | bold | center,
                main_view | flex,
                // Console: exactly 12 terminal rows tall.
                console_view->Render()
                    | size(HEIGHT, EQUAL, 12)
            }) | flex;
        });

        // Global keyboard handling
        auto app = CatchEvent(renderer, [&](Event event) {
            // Forward events to the active temporary component first.
            if (this->current_menu == 3 && this->temporary_component) {
                if (this->temporary_component->OnEvent(event)) {
                    return true;
                }
            }

            if (event == Event::Escape) {
                switch (this->current_menu) {
                    case 1: // Sandbox menu
                    case 2: // Setup menu
                        this->current_menu = 0;
                        return true;

                    default:
                        break;
                }
            }

            return false;
        });

        // Run
        this->screen.Loop(app);
    }

    ViewGuard PushView(RenderFunction renderer) {
        this->temporary_component.reset();
        this->temporary_previous_menu = this->current_menu;
        this->temporary_renderer = std::move(renderer);

        // 0 = Main
        // 1 = Sandbox
        // 2 = Setup
        // 3 = Temporary view
        this->current_menu = 3;
        this->screen.PostEvent(Event::Custom);

        return ViewGuard(this);
    }

    ViewGuard PushView(ftxui::Component component) {
        this->temporary_previous_menu = this->current_menu;

        // Retain the component so its event handler remains accessible.
        this->temporary_component = component;

        // Also provide its rendering function to the existing view system.
        this->temporary_renderer = [component = std::move(component)] {
            return component->Render();
        };

        this->current_menu = 3;
        this->screen.PostEvent(Event::Custom);

        return ViewGuard(this);
    }

    void Refresh() {
        this->screen.PostEvent(Event::Custom);
    }

};
