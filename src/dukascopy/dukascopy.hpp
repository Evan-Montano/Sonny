// dukascopy.hpp
// Responsible for exporting historical raw tick data into a binary format used by Sonny's ML architecture.

#pragma once

#include "../common/datetime.hpp"

namespace Dukascopy {

    bool ExportFullDay(const Common::DateTime &dt);
}

/*
Order to call the api
https://jetta.dukascopy.com/v1/instruments/SPY.US-USD
https://jetta.dukascopy.com/v1/ticks/SPY.US-USD/2026/8/24/{13-19}

struct MLRecord {
    uint32_t seconds_since_open;
    float    mid_delta;
    float    spread_delta;
    uint32_t volume;
};


Process creates a file {yyyymmdd.bin} saved under ~/src/storage/records

In each record:
float mid_delta     float spread_delta     uint32_t volume
[...]

*/
