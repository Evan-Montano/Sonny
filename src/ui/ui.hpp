// ui.hpp
// Static class API for UI operations, using Meyer's Singleton pattern.

#pragma once

#include "../common/logger.hpp"
#include "../core/setup.hpp"
#include "../runtime/runtime.hpp"

#include <ftxui/dom/direction.hpp>
#include <ftxui/ftxui.hpp>
#include <format>
#include <mutex>
#include <thread>

using namespace ftxui;

/**
 * @brief Owns the terminal menus and synchronizes temporary operation views.
 */
class UI {
private:
    // MEMBERS
    App Screen = ScreenInteractive::Fullscreen();

    /**
     * @brief Menu state
     * 
     */
    int CurrentMenu = 0;

    /**
     * @brief Selected item in each menu.
     * 
     */
    int MainSelected = 0;
    int SandboxSelected = 0;
    int SetupSelected = 0;
    bool ClearLogConfirmationVisible = false;
    int ClearLogConfirmationSelected = 1;
    std::string ClearLogStatus;

    // Main menu
    std::vector<std::string> MainEntries = {
        "Sandbox",
        "Setup",
        "Exit"
    };

    // Sandbox menu
    std::vector<std::string> SandboxEntries = {
        "Sandbox Webull SPY Quote Stream",
        "Back"
    };

    // Setup menu
    std::vector<std::string> SetupEntries = {
        "Run All Setup Processes (Clears Everything)",
        "Download Historical Records (Clears Existing)",
        "Download Missing Records (Keeps Existing)",
        "Update Indexes",
        "Clear Log File",
        "Back"
    };

    MenuOption MainMenuOptions;
    MenuOption SandboxMenuOptions;
    MenuOption SetupMenuOptions;

    using RenderFunction = std::function<ftxui::Element()>;
    RenderFunction TemporaryRenderer;
    ftxui::Component TemporaryComponent;
    int TemporaryPreviousMenu = 0;

    /**
     * @brief Guards all view state shared between the UI thread and operation threads.
     * The UI thread holds it while rendering and handling events. Operation threads take it
     * through PushView, RestorePreviousMenu, and Update. Recursive so UI-thread callbacks
     * can call these methods while the UI thread already holds it.
     */
    std::recursive_mutex ViewMutex;

    /**
     * @brief Root component that holds the view lock while the UI thread renders or handles events,
     * so it never observes view state that an operation thread is halfway through changing.
     */
    class LockedRoot : public ComponentBase {
    public:
        // CONSTRUCTOR

        LockedRoot(Component child, std::recursive_mutex &mutex)
            : Mutex(mutex) {
            Add(std::move(child));
        }

        // METHODS

        Element OnRender() override {
            std::lock_guard lock(this->Mutex);
            return ChildAt(0)->Render();
        }

        bool OnEvent(Event event) override {
            std::lock_guard lock(this->Mutex);
            // Forward directly: the child is not focusable while a temporary view is shown.
            return ChildAt(0)->OnEvent(event);
        }

    private:
        // MEMBERS

        std::recursive_mutex &Mutex;
    };

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
        {
            // Cleared synchronously so the renderer cannot run after the caller's captured state is destroyed.
            std::lock_guard lock(this->ViewMutex);
            this->TemporaryRenderer = nullptr;
            this->TemporaryComponent.reset();
            this->CurrentMenu = this->TemporaryPreviousMenu;
        }
        this->Screen.PostEvent(Event::Custom);
    }

    /**
     * @brief Used to start a separate working thread for all operations.
     * 
     * @param callback 
     */
    void DispatchOperation(std::function<void()> callback) {
        std::thread([cb = std::move(callback)] {
            // An exception escaping a detached thread terminates the whole application.
            try {
                cb();
            }
            catch (const std::exception &error) {
                Logger::Error(std::format("Operation failed: {}", error.what()), true);
            }
            catch (...) {
                Logger::Error("Operation failed with an unknown error.", true);
            }
        }).detach();
    }

public:
    // MEMBERS

    /**
     * @brief Restores the previous UI menu when a temporary view leaves scope.
     */
    class ViewGuard {
    private:
        // MEMBERS
        UI *Ui;

    public:
        // CONSTRUCTOR
        explicit ViewGuard(UI *ui)
            : Ui(ui) {}
        
        ViewGuard(const ViewGuard&) = delete;
        ViewGuard &operator=(const ViewGuard&) = delete;

        ViewGuard(ViewGuard &&other) noexcept
            : Ui(other.Ui) {
                other.Ui = nullptr;
            }
        
        // DESTRUCTOR

        ~ViewGuard() {
            if (Ui) {
                Ui->RestorePreviousMenu();
            }
        }

        // OPERATORS

        ViewGuard &operator=(ViewGuard &&other) noexcept {
            if (this != &other) {
                if (Ui) {
                    Ui->RestorePreviousMenu();
                }

                Ui = other.Ui;
                other.Ui = nullptr;
            }

            return *this;
        }
    };

    // METHODS

    static UI &Get() {
        static UI instance;
        return instance;
    }

    void RenderMenu() {
        this->MainMenuOptions.on_enter = [&] {
            switch (this->MainSelected) {
                case 0:
                    // Sandbox
                    this->CurrentMenu = 1;
                    break;
                case 1:
                    // Setup
                    this->CurrentMenu = 2;
                    break;

                case 2:
                    // Exit
                    this->Screen.ExitLoopClosure()();
                    break;

                default:
                    break;
            }
        };

        auto main_menu =
        Menu(
            &this->MainEntries,
            &this->MainSelected,
            this->MainMenuOptions
        );

        this->SandboxMenuOptions.on_enter = [&] {
            switch(this->SandboxSelected) {
                case 0:
                    // Start sandbox spy quote stream.
                    DispatchOperation(&Runtime::BeginSandboxRuntime);
                    break;
                case 1:
                    // Back to main menu
                    this->CurrentMenu = 0;
                    break;
                default:
                    break;
            }
        };

        auto sandbox_menu = 
        Menu(
            &this->SandboxEntries,
            &this->SandboxSelected,
            this->SandboxMenuOptions
        );


        this->SetupMenuOptions.on_enter = [&] {
            switch (this->SetupSelected) {
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
                    this->ClearLogConfirmationSelected = 1;
                    this->ClearLogStatus.clear();
                    this->ClearLogConfirmationVisible = true;
                    break;

                case 5:
                    // Back to main menu
                    this->CurrentMenu = 0;
                    break;

                default:
                    break;
            }
        };

        auto setup_menu =
        Menu(
            &this->SetupEntries,
            &this->SetupSelected,
            this->SetupMenuOptions
        );

        // Temporary view
        auto temporary_view = Renderer([&] {
            if (this->TemporaryRenderer) {
                return this->TemporaryRenderer();
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
            &this->CurrentMenu
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

            if (this->CurrentMenu == 3) {
                main_view = temporary_view->Render();
            }
            else if (this->CurrentMenu == 2 && this->ClearLogConfirmationVisible) {
                main_view = window(
                    text("Clear Current Log File?") | bold,
                    vbox({
                        text("This will erase today's Sonny log file."),
                        text("The action cannot be undone."),
                        text(this->ClearLogConfirmationSelected == 0
                            ? "> Clear log file"
                            : "  Clear log file"),
                        text(this->ClearLogConfirmationSelected == 1
                            ? "> Cancel"
                            : "  Cancel"),
                        text("Use Up/Down to choose, Enter to confirm, Esc to cancel.")
                    })
                ) | center;
            }
            else {
                std::string title;
                switch (this->CurrentMenu) {
                    case 0: title = "Menu"; break;
                    case 1: title = "Sandbox"; break;
                    case 2: title = "Setup"; break;
                    default: title = "Sonny"; break;
                }
                Elements menu_content{menus->Render()};
                if (this->CurrentMenu == 2 && this->ClearLogStatus.empty() == false) {
                    menu_content.push_back(text(this->ClearLogStatus));
                }
                main_view = window(text(title) | bold, vbox(std::move(menu_content)));
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
            if (this->CurrentMenu == 2 && this->ClearLogConfirmationVisible) {
                if (event == Event::Escape) {
                    this->ClearLogConfirmationVisible = false;
                } else if (event == Event::ArrowUp || event == Event::ArrowDown) {
                    this->ClearLogConfirmationSelected =
                        1 - this->ClearLogConfirmationSelected;
                } else if (event == Event::Return) {
                    if (this->ClearLogConfirmationSelected == 0) {
                        this->ClearLogStatus = Logger::ClearCurrentLogFile()
                            ? "Today's log file was cleared."
                            : "Could not clear today's log file.";
                    }
                    this->ClearLogConfirmationVisible = false;
                }
                return true;
            }

            // Forward events to the active temporary component first.
            if (this->CurrentMenu == 3 && this->TemporaryComponent) {
                if (this->TemporaryComponent->OnEvent(event)) {
                    return true;
                }
            }

            if (event == Event::Escape) {
                switch (this->CurrentMenu) {
                    case 1: // Sandbox menu
                    case 2: // Setup menu
                        this->CurrentMenu = 0;
                        return true;

                    default:
                        break;
                }
            }

            return false;
        });

        // Run
        this->Screen.Loop(Make<LockedRoot>(app, this->ViewMutex));
    }

    /**
     * @brief Show a temporary view until the returned guard is destroyed.
     * The renderer runs on the UI thread, so any state it reads must be changed through Update.
     *
     * @param renderer 
     * @return ViewGuard 
     */
    ViewGuard PushView(RenderFunction renderer) {
        {
            std::lock_guard lock(this->ViewMutex);
            this->TemporaryComponent.reset();
            this->TemporaryPreviousMenu = this->CurrentMenu;
            this->TemporaryRenderer = std::move(renderer);

            // 0 = Main
            // 1 = Sandbox
            // 2 = Setup
            // 3 = Temporary view
            this->CurrentMenu = 3;
        }
        this->Screen.PostEvent(Event::Custom);

        return ViewGuard(this);
    }

    /**
     * @brief Show a temporary interactive view until the returned guard is destroyed.
     * 
     * @param component 
     * @return ViewGuard 
     */
    ViewGuard PushView(ftxui::Component component) {
        {
            std::lock_guard lock(this->ViewMutex);
            this->TemporaryPreviousMenu = this->CurrentMenu;

            // Retain the component so its event handler remains accessible.
            this->TemporaryComponent = component;

            // Also provide its rendering function to the existing view system.
            this->TemporaryRenderer = [component = std::move(component)] {
                return component->Render();
            };

            this->CurrentMenu = 3;
        }
        this->Screen.PostEvent(Event::Custom);

        return ViewGuard(this);
    }

    void Refresh() {
        this->Screen.PostEvent(Event::Custom);
    }

    /**
     * @brief Change state read by a temporary view's renderer, then redraw.
     * Operation threads must use this instead of writing that state directly.
     *
     * @param mutation Changes to apply while the view lock is held. Keep it short.
     */
    void Update(const std::function<void()> &mutation) {
        {
            std::lock_guard lock(this->ViewMutex);
            mutation();
        }
        this->Refresh();
    }

};
