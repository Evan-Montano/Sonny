// dukascopy.hpp
// Responsible for exporting historical raw tick data into a binary format used by Sonny's ML architecture.

#pragma once

#include "../common/datetime.hpp"

namespace Dukascopy {

    bool ExportFullDay(const Common::DateTime &dt);
}
