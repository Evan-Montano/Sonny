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
        // const Common::DateTime BEGIN_DATE(2026, Common::NOV, 2);

        /**
         * @brief End date for export data.
         * 
         */
        const Common::DateTime END_DATE(2026, Common::NOV, 10);

        /**
        * @brief Total number of calendar days in the configured setup range.
        *
        * The +1 accounts for the inclusive BEGIN_DATE and END_DATE
        * used by the setup loops.
        */
        const long TOTAL_DAYS = END_DATE - BEGIN_DATE + 1;

        /**
         * @brief When true, "Update Indexes" keeps index segments whose files are already complete
         * and builds only the missing ones (and weekday-group indexes whose manifest no longer
         * matches their group). Set to false to rebuild every selected index from scratch, e.g. after
         * changing BEGIN_DATE/END_DATE, the vector construction, or the direction temporal weights:
         * reuse checks that the files are complete, not what they were built from.
         *
         * Either way, only the active input windows (Core::ActiveInputWindows) and the sub-indexes the
         * direction search mode needs (Core::DIRECTION_SEARCH_MODE) are touched.
         */
        inline constexpr bool REUSE_EXISTING_INDEXES = true;


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