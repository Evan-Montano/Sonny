// sonny.cpp
// Main entry point for the sonny project.

#include "common/logger.hpp"
#include "ui/ui.hpp"

int main(int argc, char* argv[]) {
    Logger::Start();
    UI::Get().RenderMenu();
    Logger::Stop();
    return 0;
}