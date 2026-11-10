// halfhourmultiindex.cpp
// Holds the class implementation for HalfHourMultiIndex and other definitions.

#include "halfhourmultiindex.hpp"
#include "common/logger.hpp"
#include "core/halfhourindex.hpp"
#include "core/structures.hpp"
#include "core/transformer.hpp"
#include <cstdlib>
#include <format>

namespace Core {

    // =================================== //
    // HalfHourIndex_Magnitude Implementation
    // =================================== //

    HalfHourMultiIndex::HalfHourIndex_Magnitude::HalfHourIndex_Magnitude(
        const std::size_t &numOfSeconds,
        std::size_t inputWindowSeconds
    ) : HalfHourIndex(numOfSeconds, inputWindowSeconds, HalfHourIndex::INX_TYPE_MAGNITUDE) {
    }

    bool HalfHourMultiIndex::HalfHourIndex_Magnitude::HasNormalizationParameters() {
        return this->_hasNormalizationParameters;
    }

    void HalfHourMultiIndex::HalfHourIndex_Magnitude::AddVectorsToIndex(
        const std::span<Core::MLRecord> &segment,
        const std::filesystem::path &recFilePath,
        const std::size_t recordOffset
    ) {
        if (this->_hasNormalizationParameters == false) {
            throw std::logic_error(
                "Cannot add vectors before normalization parameters are set."
            );
        }

        // IMPORTANT: Before we can add the vector(s) to this index, the data has to be
        // the absolute magnitude AND normalized.
        std::vector<Core::MLRecord> normalizedSegment(
            segment.begin(),
            segment.end()
        );

        for (Core::MLRecord &rec : normalizedSegment) {
            rec.mid_delta = std::abs(rec.mid_delta);
        }

        NormalizeRecordsVector(normalizedSegment, this->_normalizationParameters);

        // Num vectors should typically resolve to 1800, except for the final half-hour.
        const std::size_t numVectors = normalizedSegment.size() - _inputWindowSeconds + 1;

        // Each vector we're passing in contains _dimension * numVectors floats.
        // Basically it's a contiguous vector containing many vectors of _dimension size.
        std::vector<float> vectors{};
        vectors.reserve(numVectors * static_cast<std::size_t>(this->_dimension));

        const std::array<char, 8> tradingDay = ToShortLocationArray(recFilePath);

        for (std::size_t offset = 0; offset < numVectors; ++offset) {
            for (std::size_t windowIndex = 0; windowIndex < this->_inputWindowSeconds; ++windowIndex) {
                const MLRecord &record = normalizedSegment[offset + windowIndex];

                // MAGNITUDE SPECIFIC CODE
                // As we parse the incoming historical span, we only care about the absolute
                // magnitude of the quote delta between records. So this index is just tracking
                // absolute magnitudes in changes throughout an order book stream.
                vectors.push_back(record.mid_delta);
            }

            WindowLocation location{};
            location.recordIndex = static_cast<uint32_t>(recordOffset + offset);
            location.tradingDay = tradingDay;

            this->_windowLocations.push_back(location);
        }

        // Add all generated vectors to FAISS in one operation.
        this->_index.add(
            static_cast<faiss::idx_t>(numVectors),
            vectors.data()
        );

        this->Updated = true;

        Logger::Info(
            std::format(
                "Added {} vectors. Magnitude index now contains {} vectors",
                numVectors,
                this->_index.ntotal
            ),
            true
        );
    }

    // =================================== //
    // HalfHourIndex_Direction Implementation
    // =================================== //

    HalfHourMultiIndex::HalfHourIndex_Direction::HalfHourIndex_Direction(
        const std::size_t &numOfSeconds,
        std::size_t inputWindowSeconds
    ) : HalfHourIndex(numOfSeconds, inputWindowSeconds, HalfHourIndex::INX_TYPE_DIRECTION) {
    }

    void HalfHourMultiIndex::HalfHourIndex_Direction::AddVectorsToIndex(
        const std::span<Core::MLRecord> &segment,
        const std::filesystem::path &recFilePath,
        const std::size_t recordOffset
    ) {
        const std::size_t numVectors = segment.size() - _inputWindowSeconds + 1;

        // Each vector contains _dimension floats.
        std::vector<float> vectors{};
        vectors.reserve(numVectors * static_cast<std::size_t>(this->_dimension));

        const std::array<char, 8> tradingDay = ToShortLocationArray(recFilePath);

        for (std::size_t offset = 0; offset < numVectors; ++offset) {
            for (std::size_t windowIndex = 0; windowIndex < this->_inputWindowSeconds; ++windowIndex) {
                const MLRecord &record = segment[offset + windowIndex];
                
                // DIRECTION SPECIFIC
                // As we parse the incoming historical span, we only care about the actual direction that the
                // quote delta moved, either positive, negative, or zero(no change)
                float direction = 0.0F;

                if (record.mid_delta > 0.0F) {
                    direction = 1.0F;
                }
                else if (record.mid_delta < 0.0F) {
                    direction = -1.0F;
                }

                vectors.push_back(direction);
            }

            WindowLocation location{};
            location.recordIndex = static_cast<uint32_t>(recordOffset + offset);
            location.tradingDay = tradingDay;

            this->_windowLocations.push_back(location);
        }

        // Add all generated vectors to FAISS in one operation.
        this->_index.add(
            static_cast<faiss::idx_t>(numVectors),
            vectors.data()
        );

        this->Updated = true;

        Logger::Info(
            std::format(
                "Added {} vectors. Direction index now contains {} vectors",
                numVectors,
                this->_index.ntotal
            ),
            true
        );
    }

    // =================================== //
    // HalfHourMultiIndex Implementation
    // =================================== //

    HalfHourMultiIndex::HalfHourMultiIndex(
        const std::size_t &numOfSeconds, 
        std::size_t inputWindowSeconds) 
        : _inputWindowSeconds(inputWindowSeconds),
          _index_magnitude(numOfSeconds, inputWindowSeconds), 
          _index_direction(numOfSeconds, inputWindowSeconds) {
        
    }

    void HalfHourMultiIndex::SetNormalizationParameters(const NormalizationParameters &parameters) {
        this->_index_magnitude.SetNormalizationParameters(parameters);
        // The direction index does not contain normalization parameters due to the nature of the data.
    }

    void HalfHourMultiIndex::AddVectorsToIndex(
        const std::span<Core::MLRecord> &segment,
        const std::filesystem::path &recFilePath,
        const std::size_t recordOffset
    ) {
        if (segment.size() >= this->_inputWindowSeconds) {
            this->_index_magnitude.AddVectorsToIndex(
                segment,
                recFilePath,
                recordOffset
            );

            this->_index_direction.AddVectorsToIndex(
                segment,
                recFilePath,
                recordOffset
            );
        }
        else {
            Logger::Warning(
                std::format(
                    "Not enough records to create a vector: {} records provided",
                    segment.size()
                ),
                true
            );
        }
    }

    

    bool HalfHourMultiIndex::FilesExist(const std::size_t numOfSeconds, const std::size_t inputWindowSeconds) {
        return HalfHourIndex::FilesExist(numOfSeconds, inputWindowSeconds, HalfHourIndex::INX_TYPE_MAGNITUDE) &&
            HalfHourIndex::FilesExist(numOfSeconds, inputWindowSeconds, HalfHourIndex::INX_TYPE_DIRECTION);
    }

    HalfHourMultiIndex::MultiQueryResult HalfHourMultiIndex::Query(std::span<const MLRecord> query, std::size_t maxResults) const {
        MultiQueryResult res{};
        MultiQuery multiQuery = this->CreateMultiQuery(query);

        res.MagnitudeResults = this->_index_magnitude.Query(multiQuery.MagnitudeQuery, maxResults);
        res.DirectionResults = this->_index_direction.Query(multiQuery.DirectionQuery, maxResults);

        return res;
    }

    HalfHourMultiIndex::MultiQuery HalfHourMultiIndex::CreateMultiQuery(std::span<const MLRecord> records) const {
        MultiQuery res{};
        std::vector<MLRecord> magnitudeQuery{};
        magnitudeQuery.reserve(records.size());

        for (const MLRecord &rec : records) {
            float direction = 0.0F;
            if (rec.mid_delta > 0.0F) {
                direction = 1.0F;
            }
            else if (rec.mid_delta < 0.0F) {
                direction = -1.0F;
            }

            res.DirectionQuery.push_back(direction);
            magnitudeQuery.push_back({std::abs(rec.mid_delta)});
        }

        res.MagnitudeQuery = this->_index_magnitude.NormalizeQuery(magnitudeQuery);    

        return res;
    }


}