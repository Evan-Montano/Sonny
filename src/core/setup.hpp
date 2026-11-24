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
        const Common::DateTime BEGIN_DATE(2017, Common::FEB, 17);

        /**
         * @brief End date for export data.
         * 
         */
        const Common::DateTime END_DATE = Common::DateTime::GetCurrentDateTime();

        /**
        * @brief Total number of calendar days in the configured setup range.
        *
        * The +1 accounts for the inclusive BEGIN_DATE and END_DATE
        * used by the setup loops.
        */
        const long TOTAL_DAYS = END_DATE - BEGIN_DATE + 1;

        /**
         * @brief When true, "Update Indexes" reuses complete magnitude/direction pairs and rebuilds
         * missing or incomplete pairs. Set to false to rebuild every active input window from scratch
         * after changing the source records, date range, vector construction, or temporal weights.
         * Completeness checks do not track what the indexes were built from.
         */
        inline constexpr bool REUSE_EXISTING_INDEXES = false;


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
        void ExecuteUpdateIndexes();
    }
}