// setup.cpp
// Implementation for namespace holding references to setup processes.

#include "setup.hpp"
#include "storage.hpp"
#include "../dukascopy/dukascopy.hpp"

namespace Core {
    namespace Setup {

        void ExecuteTotalSetupProcess() {
            
        }

        void ExecuteDownloadAllRecordFiles() {
            Dukascopy::BeginCorpusExport();
        }

        void ExecuteDownloadAllMissingRecordFiles() {

        }

        void ExecuteCreateVectorFiles() {

        }

    }
}