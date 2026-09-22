// setup.cpp
// Implementation for namespace holding references to setup processes.

#include "setup.hpp"
#include "../dukascopy/dukascopy.hpp"

namespace Core {
    namespace Setup {

        void ExecuteTotalSetupProcess() {
            
        }

        void ExecuteDownloadAllRecordFiles() {
            Dukascopy::BeginCorpusExport(true);
        }

        void ExecuteDownloadAllMissingRecordFiles() {
            Dukascopy::BeginCorpusExport();
        }

        void ExecuteCreateVectorFiles() {

        }

    }
}