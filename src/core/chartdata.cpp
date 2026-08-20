// chartdata.hpp
// Implementation file to chartdata.hpp

#include "chartdata.hpp"
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <iostream>

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

                std::cout << "Normalized end: " + endDT_NY.ToString_DT() + " | " + endDT_NY.ToString_TS() << std::endl;
                std::cout << "Normalized start: " + startDT_NY.ToString_DT() + " | " + startDT_NY.ToString_TS() << std::endl << std::endl;

                uint16_t totalSecondPriceBars = endDT_NY.GetTimestamp() > startDT_NY.GetTimestamp() ?
                    endDT_NY.GetTimestamp() - startDT_NY.GetTimestamp() : 0; // just in case..

                if (totalSecondPriceBars > 0) {
                    // Begin a batched query of all pricebars for the day
                    // DISCOVERY: If I ask for 800 bars, it might give me 800. It might give me 812, or 897..
                    // Going to have to query incrementally, checking the last bar's timestamp to see if we went over the line.
                    // then finally trim the list in the end so I don't get data from the previous day.

                    std::cout << "Total bars expected: " + std::to_string(totalSecondPriceBars) << std::endl;

                    // If I query ten bars with the timestamp @ exactly 4:00PM, I get the ten bars LEADING
                    // UP TO AND INCLUDING 4pm, so: range = 15:59:51 - 16:00:00
                    // So for the returning vector, I have to push price bars to the front
                    std::vector<Common::PriceBar> region;
                    region.reserve(this->MAX_QUERY_SIZE);
                    res.PriceBars.reserve(totalSecondPriceBars);
                    Common::UnixTimestamp tmpTimestamp = endDT_NY.GetTimestamp();

                    do {
                        // Populate the partial end -> start price bars region
                        Client::WebullClient::PopulatePriceBarRange(
                            this->TickerID,
                            region,
                            tmpTimestamp,
                            this->MAX_QUERY_SIZE
                        );

                        if (region.size() > 0) {

                            // Remove any bars not in our total range
                            region.erase(
                                std::remove_if(
                                    region.begin(),
                                    region.end(),
                                    [&](const Common::PriceBar& bar) {
                                        return bar.Timestamp < startDT_NY.GetTimestamp();
                                    }
                                ),
                                region.end()
                            );

                            // Insert into main vector
                            res.PriceBars.insert(res.PriceBars.end(), region.begin(), region.end());

                            // The new timestamp to query should be the regionEnd - 1
                            tmpTimestamp = region[region.size()-1].Timestamp-1;

                            Common::PriceBar bar;
                            bar = region[0];
                            std::cout <<
                                std::format(
                                    "{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
                                    Common::DateTime(bar.Timestamp, true).ToString_DT(),
                                    bar.Timestamp,
                                    bar.Open,
                                    bar.Close,
                                    bar.High,
                                    bar.Low,
                                    bar.Volume
                                );
                            Common::UnixTimestamp ts_End = bar.Timestamp;

                            bar = region[region.size()-1];
                            std::cout <<
                                std::format(
                                    "{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
                                    Common::DateTime(bar.Timestamp, true).ToString_DT(),
                                    bar.Timestamp,
                                    bar.Open,
                                    bar.Close,
                                    bar.High,
                                    bar.Low,
                                    bar.Volume
                                );
                            
                            std::cout << "Time difference: " + std::to_string(region[0].Timestamp - region[region.size()-1].Timestamp) << std::endl;
                            std::cout << "Total in batch: " + std::to_string(region.size()) << std::endl << std::endl;
                        }
                        else {
                            break;
                        }

                        region.clear();
                    } while (res.PriceBars[res.PriceBars.size()-1].Timestamp > startDT_NY.GetTimestamp());

                    // TODO: Create a process to run 1-3 times over res.PriceBars to fill in any missing gaps
                    // Webull is a frickin' stinker and isn't delivering perfect, contiguous data..
                    

                    // reverse the returning vector so now the price bars should be in chronological order
                    std::reverse(res.PriceBars.begin(), res.PriceBars.end());
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
        Common::PriceBar response = Client::WebullClient::GetSinglePriceBar(this->TickerID, dt.GetTimestamp());
        dt.SetTimestamp(response.Timestamp);
    }

}