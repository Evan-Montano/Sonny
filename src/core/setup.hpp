// setup.hpp
// Namespace holding references to setup processes.

#pragma once

#include "../common/datetime.hpp"

namespace Core {
    namespace Setup {

        /**
         * @brief Begin date for export data.
         * 
         */
        const Common::DateTime BEGIN_DATE(2026, Common::AUG, 3);

        /**
         * @brief End date for export data.
         * 
         */
        const Common::DateTime END_DATE(2026, Common::AUG, 31);


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
        void ExecuteCreateFaissIndexes();
    }
}