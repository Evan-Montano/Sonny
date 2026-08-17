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

    /**
     * @brief For the current chart object, this method returns a full day's worth of single-second price bars from open-close. 
     * 
     * @param dt DateTime object. The time is ignored and the date only taken into consideration. Using the market open-close.
     * @return ChartDay 
     */
    ChartDay Chart::GetDay(const Common::DateTime &dt) {
        ChartDay res {
            Interval::OneSecond,
            {},
            dt
        };

        if (res.Date.IsWeekday()) {
            // Taking in the dt object and we only care about the date, not the time.
            // Converting to New York time and normalizing the bounds 

            Common::DateTime startDT_NY(dt.GetTimestamp(), true);
            
            startDT_NY.SetHour(Common::AM(9));
            startDT_NY.SetMinute(30);
            startDT_NY.SetSecond(0);
            NormalizeDateTimeToPriceBar(startDT_NY);

            // If the normalized date does not equal the inputted date
            // then the api returned the previous day, which means
            // that the inputted day was not valid, or had a different start time,
            // or was likely a holiday. In each case, we are skipping it.
            if (startDT_NY.GetDay() == Common::DateTime(dt.GetTimestamp(), true).GetDay()) {
                Common::DateTime endDT_NY(
                    startDT_NY.GetYear(),
                    startDT_NY.GetMonth(),
                    startDT_NY.GetDay(),
                    Common::PM(4),
                    0,
                    0,
                    true
                );
                NormalizeDateTimeToPriceBar(endDT_NY); // Now the end date time should be the true end of the market day (New York time).

                uint16_t totalSecondPriceBars = endDT_NY.GetTimestamp() > startDT_NY.GetTimestamp() ?
                    endDT_NY.GetTimestamp() - startDT_NY.GetTimestamp() : 0; // just in case..

                if (totalSecondPriceBars > 0) {
                    // Begin a batched query of all pricebars for the day
                    // I should be able to query 800 at a time reliably. At some point I can test a larget number for speed, however.

                }
            }
        }

        return res;
    }

    /**
     * @brief Takes in a date time and floors it to the nearest pricebar if out of bounds.
     * 
     * @param dt DateTime object
     */
    void Chart::NormalizeDateTimeToPriceBar(Common::DateTime &dt) {
        Client::PriceBarResponse response = Client::WebullClient::GetSinglePriceBar(dt.GetTimestamp(), this->TickerID);
        dt.SetTimestamp(response.Timestamp);
    }

}