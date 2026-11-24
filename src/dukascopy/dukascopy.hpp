// dukascopy.hpp
// Procedure responsible for exporting historical raw tick data into a binary
// format used by Sonny's ML architecture.

#pragma once

namespace Dukascopy {
    /**
     * @brief Entry to corpus export procedure.
     * Missing-only exports skip previously failed record files recorded on disk.
     * @param replaceExisting Retry all configured weekdays, including previously failed ones.
     */
    void BeginCorpusExport(bool replaceExisting = false);
}
