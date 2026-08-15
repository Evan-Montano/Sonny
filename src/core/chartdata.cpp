// chartdata.hpp
// Implementation file to chartdata.hpp

#include "chartdata.hpp"
#include <stdexcept>

namespace Core {

    Chart::Chart(const std::string &symbol) : Symbol(symbol) {
        if (this->Symbol.size() > 0) {
            Client::ChartInfoResponse res = Client::WebullClient::GetClosestChartInfo(this->Symbol);
            
            // Assuming if the name comes back that we have 
            // all the data we need
            if (res.Name.size() > 0) {
                this->TickerID = res.TickerID;
                this->ExchangeID = res.ExchangeID;
                this->ExchangeCode = res.ExchangeCode;
                this->Type = res.Type;
                this->RegionID = res.RegionID;
                this->RegionCode = res.RegionCode;
                this->CurrencyID = res.CurrencyID;
                this->CurrencyCode = res.CurrencyCode;
                this->Name = res.Name;
                this->Symbol = res.Symbol;
            }
            else {
                throw std::runtime_error("Chart not found");
            }
        }
        else {
            throw std::runtime_error("No symbol provided");
        }
    }

    void Chart::LoadRange(Common::DateTimeRange &range) {

    }
}