// halfhourindex.cpp
// Class and method implementations for all things pertaining to the building and querying of market futures.

#include "halfhourindex.hpp"
#include "../common/jsonutil.hpp"
#include "../common/storage.hpp"
#include "../common/logger.hpp"
#include "transformer.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>


namespace Core {

    std::string HalfHourIndex::GetHalfHourName(const std::size_t numOfSeconds) {
        const std::size_t bucket = numOfSeconds / SECONDS_PER_HALF_HOUR;

        if (bucket >= TOTAL_HALF_HOURS) {
            throw std::out_of_range(
                std::format("Invalid half-hour offset: {}", numOfSeconds)
            );
        }

        const std::size_t startMinutes = MARKET_OPEN_MINUTES + (bucket * 30);
        const std::size_t endMinutes = startMinutes + 30;

        const std::size_t startHour = startMinutes / 60;
        const std::size_t startMinute = startMinutes % 60;

        const std::size_t endHour = endMinutes / 60;
        const std::size_t endMinute = endMinutes % 60;

        return std::format(
            "{:02}{:02}-{:02}{:02}",
            startHour,
            startMinute,
            endHour,
            endMinute
        );
    }

    namespace {

        /**
         * @brief Appends the window length and optional sub-index type to a base path: <base>/<N>s[/<type>].
         */
        std::filesystem::path SubIndexDirectory(
            const std::filesystem::path &basePath,
            const std::size_t inputWindowSeconds,
            const std::optional<std::string_view> indexType
        ) {
            std::filesystem::path directory = basePath / std::format("{}s", inputWindowSeconds);

            if (indexType.has_value()) {
                directory /= *indexType;
            }

            return directory;
        }

    }

    std::filesystem::path HalfHourIndex::GetIndexPath(
        const std::size_t numOfSeconds,
        const std::size_t inputWindowSeconds,
        std::optional<std::string_view> indexType
    ) {
        return SubIndexDirectory(Common::Storage::INDEXES_BASE_PATH, inputWindowSeconds, indexType) /
            std::format(
                "{}.index",
                GetHalfHourName(numOfSeconds)
            );
    }

    std::filesystem::path HalfHourIndex::GetMetadataPath(
        const std::size_t numOfSeconds,
        const std::size_t inputWindowSeconds,
        std::optional<std::string_view> indexType
    ) {
        return SubIndexDirectory(Common::Storage::METADATA_BASE_PATH, inputWindowSeconds, indexType) /
            std::format(
                "{}.meta",
                GetHalfHourName(numOfSeconds)
            );
    }

    std::filesystem::path HalfHourIndex::GetNormalizationPath(
        const std::size_t numOfSeconds,
        const std::size_t inputWindowSeconds,
        std::optional<std::string_view> indexType
    ) {
        const std::filesystem::path metadataPath = GetMetadataPath(numOfSeconds, inputWindowSeconds, indexType);
        return metadataPath.parent_path() / (metadataPath.stem().string() + ".normalization.json");
    }

    bool HalfHourIndex::FilesExist(
        const std::size_t numOfSeconds,
        const std::size_t inputWindowSeconds,
        std::optional<std::string_view> indexType
    ) {
        // The direction index does not use normalization-parameter files.
        const bool requiresNormalization = (indexType.has_value() && *indexType == INX_TYPE_DIRECTION) == false;

        return Common::Storage::FileExists(GetIndexPath(numOfSeconds, inputWindowSeconds, indexType)) &&
            Common::Storage::FileExists(GetMetadataPath(numOfSeconds, inputWindowSeconds, indexType)) &&
            (requiresNormalization == false ||
                Common::Storage::FileExists(GetNormalizationPath(numOfSeconds, inputWindowSeconds, indexType)));
    }

    void HalfHourIndex::DeleteFiles(
        const std::size_t numOfSeconds,
        const std::size_t inputWindowSeconds,
        std::optional<std::string_view> indexType
    ) {
        for (const auto& path : {
            GetIndexPath(numOfSeconds, inputWindowSeconds, indexType),
            GetMetadataPath(numOfSeconds, inputWindowSeconds, indexType),
            GetNormalizationPath(numOfSeconds, inputWindowSeconds, indexType)
        }) {
            std::error_code error;
            Common::Storage::DeleteFile(path, error);
            if (error) {
                throw std::filesystem::filesystem_error("Cannot clear index files before rebuilding", path, error);
            }
        }
    }

    std::filesystem::path HalfHourIndex::GetRecordFilePathFromWindowLocation(const std::array<char,8> &yyyymmdd) {
        const std::string date(yyyymmdd.data(), yyyymmdd.size());
        const std::string formattedDate =
            date.substr(0, 4) + "-" +
            date.substr(4, 2) + "-" +
            date.substr(6, 2);

        return Common::Storage::RECORDS_BASE_PATH / (formattedDate + ".rec");
    }

    std::array<char,8> HalfHourIndex::ToShortLocationArray(const std::filesystem::path &recordFilePath) {
        const std::string date = recordFilePath.stem().string();
        std::array<char, 8> shortDate{};
        std::size_t outputIndex = 0;

        for (const char c : date) {
            if (c != '-') {
                shortDate[outputIndex++] = c;
            }
        }

        return shortDate;
    }

    HalfHourIndex::HalfHourIndex(
        const std::filesystem::path indexPath,
        const std::filesystem::path metadataPath,
        const std::size_t inputWindowSeconds,
        std::optional<std::string_view> indexType
    )
        : _inputWindowSeconds(inputWindowSeconds),
        _indexType(indexType),
        _dimension(static_cast<int>(InputVectorDimension(inputWindowSeconds))),
        _index(_dimension),
        _indexPath(indexPath),
        _metadataPath(metadataPath)
    {
        if (IsSupportedInputWindow(inputWindowSeconds) == false) {
            throw std::invalid_argument(
                std::format("Unsupported input window length: {}", inputWindowSeconds)
            );
        }

        if (_indexPath.empty()) {
            _indexPath = GetIndexPath(1, _inputWindowSeconds, indexType);
        }

        if (_metadataPath.empty()) {
            _metadataPath = GetMetadataPath(1, _inputWindowSeconds, indexType);
        }

        _normalizationPath = _metadataPath.parent_path() / (_metadataPath.stem().string() + ".normalization.json");

        Initialize();
    }

    HalfHourIndex::HalfHourIndex(
        const std::size_t& numOfSeconds,
        const std::size_t inputWindowSeconds,
        std::optional<std::string_view> indexType
    )
        : _inputWindowSeconds(inputWindowSeconds),
        _indexType(indexType),
        _dimension(static_cast<int>(InputVectorDimension(inputWindowSeconds))),
        _index(_dimension),
        _indexPath(GetIndexPath(numOfSeconds, inputWindowSeconds, indexType)),
        _metadataPath(GetMetadataPath(numOfSeconds, inputWindowSeconds, indexType)),
        _normalizationPath(GetNormalizationPath(numOfSeconds, inputWindowSeconds, indexType))
    {
        if (IsSupportedInputWindow(inputWindowSeconds) == false) {
            throw std::invalid_argument(
                std::format("Unsupported input window length: {}", inputWindowSeconds)
            );
        }

        Initialize();
    }

    void HalfHourIndex::Initialize() {
        if (Common::Storage::FileExists(this->_indexPath)) {
            Logger::Info(
                std::format(
                    "Loading FAISS index: {}",
                    this->_indexPath.string()
                ),
                true
            );

            // Using faiss api to load the (.)index file from path
            faiss::Index *loadedIndex = faiss::read_index(this->_indexPath.string().c_str());

            if (loadedIndex == nullptr) {
                throw std::runtime_error(
                    std::format(
                        "FAISS returned a null index for: {}",
                        this->_indexPath.string()
                    )
                );
            }

            faiss::IndexFlatL2 *flatIndex = dynamic_cast<faiss::IndexFlatL2 *>(loadedIndex);
            if (flatIndex == nullptr) {
                delete loadedIndex;

                throw std::runtime_error(
                    std::format(
                        "Index at {} is not an IndexFlatL2",
                        this->_indexPath.string()
                    )
                );
            }

            if (flatIndex->d != _dimension) {
                const int actualDimension = flatIndex->d;

                delete loadedIndex;

                throw std::runtime_error(
                    std::format(
                        "Index at {} has dimension {}, expected {}",
                        this->_indexPath.string(),
                        actualDimension,
                        _dimension
                    )
                );
            }

            // Take the loaded vectors rather than copying them: a copy briefly holds the whole index
            // twice, which is about a gigabyte for a 60s segment. The dimension was checked above and
            // both are IndexFlatL2, so the vectors and their count are all that differ.
            this->_index.codes = std::move(flatIndex->codes);
            this->_index.ntotal = flatIndex->ntotal;
            delete loadedIndex;
            this->Updated = false;

            Logger::Info(
                std::format(
                    "Loaded {} vectors from {}",
                    this->_index.ntotal,
                    this->_indexPath.string()
                ),
                true
            );

            // Load metadata from file
            if (Common::Storage::FileExists(this->_metadataPath)) {
                LoadMetadata();
            }

            // Laod the normalization parameters associated with this index.
            if (this->_indexType.has_value() && *this->_indexType == INX_TYPE_MAGNITUDE) {
                LoadNormalizationParameters();
            }
        }
        else {
            Logger::Info(
                std::format(
                    "Creating new FAISS index: {}",
                    this->_indexPath.string()
                ),
                true
            );

            this->Updated = true;
        }
    }

    HalfHourIndex::~HalfHourIndex() {
        if (this->Updated) {
            try {
                if (_hasNormalizationParameters == false && this->_indexType.has_value() && *this->_indexType == INX_TYPE_MAGNITUDE) {
                    throw std::runtime_error(
                        "Cannot persist HalfHourIndex without normalization parameters."
                    );
                }

                Logger::Info(
                    std::format(
                        "Saving FAISS index: {}",
                        this->_indexPath.string()
                    ),
                    true
                );

                if (Common::Storage::FileExists(
                        this->_indexPath.parent_path()) == false) {
                    Common::Storage::CreateDirectory(
                        this->_indexPath.parent_path()
                    );
                }

                faiss::write_index(
                    &this->_index,
                    this->_indexPath.string().c_str()
                );

                SaveMetadataToDisk();

                if (this->_indexType.has_value() && *this->_indexType == INX_TYPE_MAGNITUDE) {
                    SaveNormalizationParametersToDisk();
                }
            }
            catch (const std::exception& e) {
                Logger::Error(
                    std::format(
                        "Failed to save HalfHourIndex: {}",
                        e.what()
                    ),
                    true
                );
            }
        }
    }

    void HalfHourIndex::LoadMetadata() {
        Logger::Info(std::format("Loading metadata: {}", _metadataPath.string()), true);
        _windowLocations = Common::Storage::ReadBinaryRecords<WindowLocation>(_metadataPath);
        Logger::Info("Successfully loaded metadata", true);
    }

    void HalfHourIndex::SaveMetadataToDisk() {
        Common::Storage::CreateDirectory(_metadataPath.parent_path());
        Common::Storage::WriteBinaryRecords<WindowLocation>(_metadataPath, _windowLocations);
        Logger::Info(std::format("Successfully saved {} metadata entries", _windowLocations.size()), true);
    }

    void HalfHourIndex::LoadNormalizationParameters() {
        if (Common::Storage::FileExists(_normalizationPath) == false) {
            throw std::runtime_error(
                std::format(
                    "Normalization parameters missing: {}. "
                    "The historical indexes must be rebuilt.",
                    _normalizationPath.string()
                )
            );
        }

        Common::JsonUtility json = Common::JsonUtility::FromFile(_normalizationPath);

        const int schemaVersion = json.Get<int>("schema_version");

        if (schemaVersion != 3) {
            throw std::runtime_error(
                std::format(
                    "Unsupported normalization schema version: {}. "
                    "The indexes for the configured input window must be rebuilt.",
                    schemaVersion
                )
            );
        }

        const std::vector<std::string> expectedFeatureOrder{
            "mid_delta"
        };

        const auto featureOrder = json.Get<std::vector<std::string>>("feature_order");

        if (featureOrder != expectedFeatureOrder) {
            throw std::runtime_error(
                "Normalization feature order does not match Sonny's MLRecord layout."
            );
        }

        const std::size_t inputWindowSeconds =
            json.Get<std::size_t>("input_window_seconds");

        if (inputWindowSeconds != _inputWindowSeconds) {
            throw std::runtime_error(
                std::format(
                    "Normalization parameters use a {}-second input window; "
                    "the configured input window is {} seconds.",
                    inputWindowSeconds,
                    _inputWindowSeconds
                )
            );
        }

        if (json.Get<std::string>("standard_deviation") != "population") {
            throw std::runtime_error(
                "Unsupported normalization standard deviation convention."
            );
        }

        NormalizationParameters parameters{
            .SampleCount = json.Get<std::uint64_t>("sample_count"),

            .MidDeltaMean = json.Get<double>("mid_delta_mean"),
            .MidDeltaStddev = json.Get<double>("mid_delta_stddev")
        };

        ValidateNormalizationParameters(parameters);

        _normalizationParameters = parameters;
        _hasNormalizationParameters = true;

        Logger::Info(
            std::format(
                "Loaded normalization parameters: {} ({} samples)",
                _normalizationPath.string(),
                parameters.SampleCount
            ),
            true
        );
    }

    void HalfHourIndex::SaveNormalizationParametersToDisk() const {
        if (_hasNormalizationParameters == false && this->_indexType.has_value() && *this->_indexType == INX_TYPE_MAGNITUDE) {
            throw std::runtime_error(
                "Cannot save an index without normalization parameters."
            );
        }

        Common::JsonUtility json{};

        json.Set("schema_version", 3);
        json.Set("input_window_seconds", _inputWindowSeconds);

        json.Set(
            "feature_order",
            std::vector<std::string>{
                "mid_delta"
            }
        );

        json.Set("standard_deviation", "population");

        json.Set("sample_count", _normalizationParameters.SampleCount);

        json.Set("mid_delta_mean", _normalizationParameters.MidDeltaMean);
        json.Set("mid_delta_stddev", _normalizationParameters.MidDeltaStddev);

        Common::Storage::WriteFileAtomically(_normalizationPath, json.ToString());

        Logger::Info(
            std::format(
                "Saved normalization parameters: {}",
                _normalizationPath.string()
            ),
            true
        );
    }

    void HalfHourIndex::SetNormalizationParameters(const NormalizationParameters &parameters) {
        if (_index.ntotal > 0) {
            throw std::logic_error(
                "Cannot change normalization parameters after vectors have been indexed."
            );
        }

        ValidateNormalizationParameters(parameters);

        _normalizationParameters = parameters;
        _hasNormalizationParameters = true;
        Updated = true;
    }

    std::vector<float> HalfHourIndex::NormalizeQuery(std::span<const MLRecord> records) const {
        if (_hasNormalizationParameters == false && this->_indexType.has_value() && *this->_indexType == INX_TYPE_MAGNITUDE) {
            throw std::logic_error(
                "Cannot normalize a query without loaded normalization parameters."
            );
        }

        if (records.size() != _inputWindowSeconds) {
            throw std::invalid_argument(
                std::format(
                    "Query contains {} records; expected {}.",
                    records.size(),
                    _inputWindowSeconds
                )
            );
        }

        return NormalizeRecordsToVector(records, _normalizationParameters);
    }

    std::vector<HalfHourIndex::QueryResult> HalfHourIndex::Query(const std::vector<float> &normalizedQuery, const std::size_t maxResults) const {
        if (_hasNormalizationParameters == false && this->_indexType.has_value() && *this->_indexType == INX_TYPE_MAGNITUDE) {
            throw std::logic_error(
                "Cannot query an index without normalization parameters."
            );
        }

        std::vector<QueryResult> res{};

        if (normalizedQuery.size() == static_cast<std::size_t>(_dimension)) {
            if (maxResults > 0 && this->_index.ntotal > 0) {
                const faiss::idx_t resultCount = static_cast<faiss::idx_t>(
                    std::min(maxResults, static_cast<std::size_t>(this->_index.ntotal))
                );

                std::vector<float> distances(static_cast<std::size_t>(resultCount));
                std::vector<faiss::idx_t> ids(static_cast<std::size_t>(resultCount));

                this->_index.search(
                    1,
                    normalizedQuery.data(),
                    resultCount,
                    distances.data(),
                    ids.data()
                );

                res.reserve(static_cast<std::size_t>(resultCount));

                for (std::size_t i = 0; i < static_cast<std::size_t>(resultCount); ++i) {
                    const faiss::idx_t id = ids[i];

                    if (id < 0 || static_cast<std::size_t>(id) >= this->_windowLocations.size()) {
                        Logger::Error(
                            std::format("Missing metadata for FAISS vector ID {}", id)
                        );
                        throw std::runtime_error(
                            std::format("Missing metadata for FAISS vector ID {}", id)
                        );
                    }

                    const WindowLocation &location = this->_windowLocations[static_cast<std::size_t>(id)];
                    res.push_back(QueryResult{
                        .Distance = distances[i],
                        .TradingDayId = location.TradingDay,
                        .RecordIndex = location.RecordIndex
                    });
                }
            }
        }
        else {
            Logger::Error(
                std::format(
                    "Query dimension is {}, expected {}",
                    normalizedQuery.size(),
                    _dimension
                )
            );
            throw std::invalid_argument(
                std::format(
                    "Query dimension is {}, expected {}",
                    normalizedQuery.size(),
                    _dimension
                )
            );
        }

        return res;
    }

}
