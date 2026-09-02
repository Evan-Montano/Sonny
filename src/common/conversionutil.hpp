// conversionutil.hpp
// Static class to help with data conversions.

#pragma once

#include <cstdint>
#include <stdexcept>

namespace Common {

    class Convert {
        public:
        // METHODS
        static std::int64_t DecStrToInt(std::string_view str, int decimal_places) {
            std::int64_t result = 0;
            bool after_decimal = false;
            int decimals = 0;

            for (char c : str) {
                if (c == '.') {
                    after_decimal = true;
                    continue;
                }

                if (c < '0' || c > '9')
                    throw std::invalid_argument("Invalid number");

                result = result * 10 + (c - '0');

                if (after_decimal && ++decimals == decimal_places)
                    break;
            }

            while (decimals < decimal_places) {
                result *= 10;
                ++decimals;
            }

            return result;
        }
    };
}