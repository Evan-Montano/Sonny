// ui.hpp
// Static class API for UI operations, using Meyer's Singleton pattern.

#pragma once

#include "../core/setup.hpp"

#include <ftxui/ftxui.hpp>

using namespace ftxui;

class UI {
private:
    // MEMBERS
    App screen = ScreenInteractive::TerminalOutput();

    // Menu state
    int current_menu = 0;

    // Selected item in each menu.
    int main_selected = 0;
    int setup_selected = 0;

    // Main menu
    std::vector<std::string> main_entries = {
        "Setup",
        "Exit"
    };

    // Setup menu
    std::vector<std::string> setup_entries = {
        "Run All Setup Processes (Clears Everything)",
        "Download Historical Records (Clears Existing)",
        "Download Missing Records (Keeps Existing)",
        "Create Vector Files (Clears Existing)",
        "Back"
    };

    MenuOption main_menu_options;
    MenuOption setup_menu_options;

    using RenderFunction = std::function<ftxui::Element()>;
    RenderFunction temporary_renderer;
    int temporary_previous_menu = 0;

    // CONSTRUCTOR
    UI() = default;
    UI(const UI&) = delete;
    UI &operator=(const UI&) = delete;
    UI(UI&&) = delete;
    UI &operator=(UI&&) = delete;

    // METHODS
    void RestorePreviousMenu() {
        this->temporary_renderer = nullptr;
        this->current_menu = this->temporary_previous_menu;
        this->screen.PostEvent(Event::Custom);
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
                    // Setup
                    this->current_menu = 1;
                    break;

                case 1:
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

        this->setup_menu_options.on_enter = [&] {
            switch (this->setup_selected) {
                case 0:
                    // Total setup process
                    Core::Setup::ExecuteTotalSetupProcess();
                    break;

                case 1:
                    // Download all record files
                    Core::Setup::ExecuteDownloadAllRecordFiles();
                    break;

                case 2:
                    // Download missing records, keep existing
                    Core::Setup::ExecuteDownloadAllMissingRecordFiles();
                    break;

                case 3:
                    // Create vector files
                    Core::Setup::ExecuteCreateVectorFiles();
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
                setup_menu,
                temporary_view
            },
            &this->current_menu
        );

        // UI
        auto renderer = Renderer(menus, [&] {
            if (this->current_menu == 2) {
                return temporary_view->Render();
            }

            return vbox({
                text(this->current_menu == 0 ? "Menu" : "Setup")
                    | bold
                    | center,

                separator(),

                menus->Render()
            });
        });

        // Global keyboard handling
        auto app = CatchEvent(renderer, [&](Event event) {
            if (event == Event::Escape) {
                if (this->current_menu == 1) {
                    // Escape from Setup -> Main Menu
                    this->current_menu = 0;
                    return true;
                }
            }

            return false;
        });

        // Run
        this->screen.Loop(app);
    }

    ViewGuard PushView(RenderFunction renderer) {
        this->temporary_previous_menu = this->current_menu;
        this->temporary_renderer = std::move(renderer);

        // 0 = Main
        // 1 = Setup
        // 2 = Temporary view
        this->current_menu = 2;

        this->screen.PostEvent(Event::Custom);
        return ViewGuard(this);
    }

    void Refresh() {
        this->screen.PostEvent(Event::Custom);
    }

};

// Sample usage
// void ExecuteDownloadAllRecordFiles() {

//     std::string status = "Starting download...";

//     auto view = UI::Get().PushView([&] {
//         return ftxui::vbox({
//             ftxui::text("Download Historical Records") | ftxui::bold,
//             ftxui::separator(),
//             ftxui::text(status)
//         });
//     });

//     UI::Get().Refresh();

//     status = "Connecting to Dukascopy...";
//     UI::Get().Refresh();

//     // Actual download work...
    
//     status = "Downloading records...";
//     UI::Get().Refresh();

//     // More work...

//     status = "Download complete.";
//     UI::Get().Refresh();

//     // `view` goes out of scope when this function returns.
//     // The Setup menu automatically comes back.
// }
