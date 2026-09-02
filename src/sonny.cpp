// sonny.cpp
// Main entry point for the sonny project.

#include "dukascopy/dukascopy.hpp"
#include "common/logger.hpp"

int main(int argc, char* argv[]) {
    Logger::Start();

	Dukascopy::ExportFullDay(Common::DateTime(2026, 8, 24));


    Logger::Stop();
    return 0;
}