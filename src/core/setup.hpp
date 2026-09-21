// setup.hpp
// Namespace holding references to setup processes.

#pragma once

namespace Core {
    namespace Setup {

        /**
         * @brief Process including all setup steps: Corpus download, create vector files.
         * 
         */
        void ExecuteTotalSetupProcess();

        /**
         * @brief Process to download all record files from Dukascopy endpoint.
         * 
         */
        void ExecuteDownloadAllRecordFiles();

        /**
         * @brief Process to download all missing record files from dukascopy endpoint.
         * 
         */
        void ExecuteDownloadAllMissingRecordFiles();

        /**
         * @brief Process to create faiss vector files from records.
         * 
         */
        void ExecuteCreateVectorFiles();
    }
}