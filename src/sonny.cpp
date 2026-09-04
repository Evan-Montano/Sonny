// sonny.cpp
// Main entry point for the sonny project.

#include "dukascopy/dukascopy.hpp"
#include "common/logger.hpp"

int main(int argc, char* argv[]) {
    Logger::Start();
    Dukascopy::BeginCorpusExport();
    Logger::Stop();
    return 0;
}