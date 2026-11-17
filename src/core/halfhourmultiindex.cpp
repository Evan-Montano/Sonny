// halfhourmultiindex.cpp
// Holds the class implementation for HalfHourMultiIndex and other definitions.

#include "halfhourmultiindex.hpp"
#include "common/logger.hpp"
#include "core/halfhourindex.hpp"
#include "core/structures.hpp"
#include "core/transformer.hpp"
#include <cstdlib>
#include <cmath>
#include <format>
#include <span>
#include <stdexcept>

namespace Core {

    namespace {

        void ApplyDirectionTemporalWeights(
            const std::span<float> vectors,
            const std::size_t vectorSize
        ) {
            if (vectorSize == 0 || vectors.size() % vectorSize != 0) {
                throw std::invalid_argument("Direction vectors do not match their configured dimension.");
            }

            // A single observation has no relative recency, so leave it at the neutral weight of 1.
            // This also avoids dividing by vectorSize - 1.
            std::vector<float> positionScales(vectorSize, 1.0F);

            if (vectorSize > 1) {
                // IndexFlatL2 squares coordinate deltas, so scale each coordinate by sqrt(weight).
                const double newestPosition = static_cast<double>(vectorSize - 1);

                for (std::size_t position = 0; position < vectorSize; ++position) {
                    const double weight = 0.8 + 0.4 * (static_cast<double>(position) / newestPosition);
                    positionScales[position] = static_cast<float>(std::sqrt(weight));
                }
            }

            for (std::size_t position = 0; position < vectors.size(); ++position) {
                vectors[position] *= positionScales[position % vectorSize];
            }
        }

    }

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

    const NormalizationParameters &HalfHourMultiIndex::HalfHourIndex_Magnitude::GetNormalizationParameters() const {
        return this->_normalizationParameters;
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
        std::size_t inputWindowSeconds,
        std::optional<WeekdayGroup> weekdayGroup
    ) : HalfHourIndex(numOfSeconds, inputWindowSeconds, HalfHourIndex::INX_TYPE_DIRECTION, weekdayGroup) {
    }

    void HalfHourMultiIndex::HalfHourIndex_Direction::AddVectorsToIndex(
        const std::span<Core::MLRecord> &segment,
        const std::filesystem::path &recFilePath,
        const std::size_t recordOffset
    ) {
        const std::array<char, 8> tradingDay = ToShortLocationArray(recFilePath);

        // A weekday-group index takes only its own weekdays' sessions. Every window in a .rec file
        // starts in that file's New York session, so the file's date is the window's weekday.
        if (this->_weekdayGroup.has_value() && this->_weekdayGroup->Contains(TradingDayWeekday(tradingDay)) == false) {
            return;
        }

        const std::size_t numVectors = segment.size() - _inputWindowSeconds + 1;

        // Each vector contains _dimension floats.
        std::vector<float> vectors{};
        vectors.reserve(numVectors * static_cast<std::size_t>(this->_dimension));

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

        ApplyDirectionTemporalWeights(vectors, _inputWindowSeconds);

        // Add all generated vectors to FAISS in one operation.
        this->_index.add(
            static_cast<faiss::idx_t>(numVectors),
            vectors.data()
        );

        this->Updated = true;

        Logger::Info(
            std::format(
                "Added {} vectors. Direction index ({}) now contains {} vectors",
                numVectors,
                this->_weekdayGroup.has_value() ? this->_weekdayGroup->name : ALL_DAYS_DIRECTION_LABEL,
                this->_index.ntotal
            ),
            true
        );
    }

    // =================================== //
    // HalfHourMultiIndex Implementation
    // =================================== //

    HalfHourMultiIndex::SubIndexSelection HalfHourMultiIndex::SubIndexSelection::ForSearch(
        const DirectionSearchMode mode,
        const std::optional<WeekdayGroup> &weekdayGroup
    ) {
        SubIndexSelection selection{
            .magnitude = true,
            .all_days_direction = SearchesAllDaysDirection(mode)
        };

        if (SearchesWeekdayDirection(mode)) {
            if (weekdayGroup.has_value() == false) {
                throw std::invalid_argument("This direction search mode needs a weekday group.");
            }

            selection.weekday_groups.push_back(*weekdayGroup);
        }

        return selection;
    }

    HalfHourMultiIndex::SubIndexSelection HalfHourMultiIndex::SubIndexSelection::ToBuild(
        const std::size_t numOfSeconds,
        const std::size_t inputWindowSeconds,
        const DirectionSearchMode mode,
        const std::span<const WeekdayGroup> weekdayGroups,
        const bool reuseExisting
    ) {
        const auto needed = [&](const std::optional<std::string_view> type, const std::optional<WeekdayGroup> &group = std::nullopt) {
            return reuseExisting == false || HalfHourIndex::FilesExist(numOfSeconds, inputWindowSeconds, type, group) == false;
        };

        SubIndexSelection selection{
            .magnitude = needed(HalfHourIndex::INX_TYPE_MAGNITUDE),
            .all_days_direction = SearchesAllDaysDirection(mode) && needed(HalfHourIndex::INX_TYPE_DIRECTION)
        };

        if (SearchesWeekdayDirection(mode)) {
            for (const WeekdayGroup &group : weekdayGroups) {
                if (needed(HalfHourIndex::INX_TYPE_DIRECTION, group)) {
                    selection.weekday_groups.push_back(group);
                }
            }
        }

        return selection;
    }

    bool HalfHourMultiIndex::SubIndexSelection::Empty() const {
        return magnitude == false && all_days_direction == false && weekday_groups.empty();
    }

    HalfHourMultiIndex::HalfHourMultiIndex(
        const std::size_t &numOfSeconds, 
        std::size_t inputWindowSeconds) 
        : HalfHourMultiIndex(numOfSeconds, inputWindowSeconds, SubIndexSelection{}) {
    }

    HalfHourMultiIndex::HalfHourMultiIndex(
        const std::size_t &numOfSeconds,
        const std::size_t inputWindowSeconds,
        const SubIndexSelection &subIndexes)
        : _inputWindowSeconds(inputWindowSeconds) {
        if (subIndexes.magnitude) {
            _index_magnitude.emplace(numOfSeconds, inputWindowSeconds);
        }

        if (subIndexes.all_days_direction) {
            _index_direction.emplace(numOfSeconds, inputWindowSeconds);
        }

        for (const WeekdayGroup &group : subIndexes.weekday_groups) {
            _weekday_direction_indexes.push_back(
                std::make_unique<HalfHourIndex_Direction>(numOfSeconds, inputWindowSeconds, group)
            );
        }
    }

    void HalfHourMultiIndex::SetNormalizationParameters(const NormalizationParameters &parameters) {
        if (this->_index_magnitude.has_value() == false) {
            throw std::logic_error("This index does not hold a magnitude sub-index.");
        }

        this->_index_magnitude->SetNormalizationParameters(parameters);
        // The direction index does not contain normalization parameters due to the nature of the data.
    }

    const NormalizationParameters &HalfHourMultiIndex::GetNormalizationParameters() const {
        if (this->_index_magnitude.has_value() == false) {
            throw std::logic_error("This index does not hold a magnitude sub-index.");
        }

        return this->_index_magnitude->GetNormalizationParameters();
    }

    void HalfHourMultiIndex::AddVectorsToIndex(
        const std::span<Core::MLRecord> &segment,
        const std::filesystem::path &recFilePath,
        const std::size_t recordOffset
    ) {
        if (segment.size() >= this->_inputWindowSeconds) {
            if (this->_index_magnitude.has_value()) {
                this->_index_magnitude->AddVectorsToIndex(
                    segment,
                    recFilePath,
                    recordOffset
                );
            }

            if (this->_index_direction.has_value()) {
                this->_index_direction->AddVectorsToIndex(
                    segment,
                    recFilePath,
                    recordOffset
                );
            }

            // Each weekday-group index keeps only the days of its own group.
            for (const std::unique_ptr<HalfHourIndex_Direction> &weekdayIndex : this->_weekday_direction_indexes) {
                weekdayIndex->AddVectorsToIndex(
                    segment,
                    recFilePath,
                    recordOffset
                );
            }
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
        return FilesExist(numOfSeconds, inputWindowSeconds, SubIndexSelection{});
    }

    bool HalfHourMultiIndex::FilesExist(
        const std::size_t numOfSeconds,
        const std::size_t inputWindowSeconds,
        const SubIndexSelection &subIndexes
    ) {
        bool exist =
            (subIndexes.magnitude == false ||
                HalfHourIndex::FilesExist(numOfSeconds, inputWindowSeconds, HalfHourIndex::INX_TYPE_MAGNITUDE)) &&
            (subIndexes.all_days_direction == false ||
                HalfHourIndex::FilesExist(numOfSeconds, inputWindowSeconds, HalfHourIndex::INX_TYPE_DIRECTION));

        for (const WeekdayGroup &group : subIndexes.weekday_groups) {
            exist = exist && HalfHourIndex::FilesExist(numOfSeconds, inputWindowSeconds, HalfHourIndex::INX_TYPE_DIRECTION, group);
        }

        return exist;
    }

    void HalfHourMultiIndex::DeleteFiles(
        const std::size_t numOfSeconds,
        const std::size_t inputWindowSeconds,
        const SubIndexSelection &subIndexes
    ) {
        if (subIndexes.magnitude) {
            HalfHourIndex::DeleteFiles(numOfSeconds, inputWindowSeconds, HalfHourIndex::INX_TYPE_MAGNITUDE);
        }

        if (subIndexes.all_days_direction) {
            HalfHourIndex::DeleteFiles(numOfSeconds, inputWindowSeconds, HalfHourIndex::INX_TYPE_DIRECTION);
        }

        for (const WeekdayGroup &group : subIndexes.weekday_groups) {
            HalfHourIndex::DeleteFiles(numOfSeconds, inputWindowSeconds, HalfHourIndex::INX_TYPE_DIRECTION, group);
        }
    }

    HalfHourMultiIndex::MultiQueryResult HalfHourMultiIndex::Query(std::span<const MLRecord> query, std::size_t maxResults) const {
        if (this->_index_magnitude.has_value() == false) {
            throw std::logic_error("Cannot query a segment without its magnitude sub-index.");
        }

        if (this->_weekday_direction_indexes.size() > 1) {
            throw std::logic_error("A segment can be queried with at most one weekday group loaded.");
        }

        MultiQueryResult res{};
        MultiQuery multiQuery = this->CreateMultiQuery(query);

        res.MagnitudeResults = this->_index_magnitude->Query(multiQuery.MagnitudeQuery, maxResults);

        // Weighted once; every direction index of this window uses the same coordinate weights.
        ApplyDirectionTemporalWeights(multiQuery.DirectionQuery, _inputWindowSeconds);

        if (this->_index_direction.has_value()) {
            res.DirectionResults = this->_index_direction->Query(multiQuery.DirectionQuery, maxResults);
        }

        if (this->_weekday_direction_indexes.empty() == false) {
            res.WeekdayDirectionResults = this->_weekday_direction_indexes.front()->Query(multiQuery.DirectionQuery, maxResults);
        }

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

        res.MagnitudeQuery = this->_index_magnitude->NormalizeQuery(magnitudeQuery);    

        return res;
    }


}
