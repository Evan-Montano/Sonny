// setup.cpp
// Implementation for namespace holding references to setup processes.

#include "setup.hpp"
#include "../common/storage.hpp"
#include "../dukascopy/dukascopy.hpp"
#include "../common/logger.hpp"

namespace Core {
    namespace Setup {

        /**
         * @brief Helper to delete the corpus and records directories.
         * 
         */
        void DeleteAllStorageDirectories() {
            Logger::Info("Deleting all records..", true);
            Common::Storage::DeleteDirectory("storage/corpus");
            Common::Storage::DeleteDirectory("storage/records");
            Common::Storage::DeleteDirectory("storage/indexes");
        }

        void ExecuteTotalSetupProcess() {
            DeleteAllStorageDirectories();
            ExecuteDownloadAllRecordFiles();
            ExecuteCreateFaissIndexes();
        }

        void ExecuteDownloadAllRecordFiles() {
            Dukascopy::BeginCorpusExport(true);
        }

        void ExecuteDownloadAllMissingRecordFiles() {
            Dukascopy::BeginCorpusExport();
        }

        void ExecuteCreateFaissIndexes() {
            
        }

    }
}