// sonny.cpp
// Main entry point for the sonny project.

#include "common/logger.hpp"
#include <ftxui/ftxui.hpp>

int main(int argc, char* argv[]) {
    Logger::Start();

    std::vector<std::string> entries = {
        "Download Data",
        "Build Corpus",
        "Train Model",
        "Run Prediction",
        "Exit"
    };

    int selected = 0;

    auto menu = ftxui::Menu(&entries, &selected);
    
    auto renderer = Renderer(menu, [&] {
        return ftxui::vbox({
            ftxui::text("Project Sonny") | ftxui::bold | ftxui::center,
            ftxui::separator(),
            menu->Render(),
        });
    });

    auto screen = ftxui::ScreenInteractive::TerminalOutput();
    screen.Loop(renderer);

    Logger::Stop();
    return 0;
}