// halfhourindex.cpp
// Class and method implementations for all things pertaining to the building and querying of market futures.

#include "halfhourindex.hpp"
#include "../common/storage.hpp"
#include "../common/logger.hpp"
#include "transformer.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
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
         * @brief Appends the optional sub-index type and weekday-group directories to a window's base path:
         * <base>/<N>s[/<type>[/weekdays/<group>]]. All-week indexes keep the layout they had before groups.
         */
        std::filesystem::path SubIndexDirectory(
            const std::filesystem::path &basePath,
            const std::size_t inputWindowSeconds,
            const std::optional<std::string_view> indexType,
            const std::optional<WeekdayGroup> &weekdayGroup
        ) {
            std::filesystem::path directory = basePath / std::format("{}s", inputWindowSeconds);

            if (indexType.has_value()) {
                directory /= *indexType;
            }

            if (weekdayGroup.has_value()) {
                directory = directory / HalfHourIndex::WEEKDAY_GROUPS_DIRECTORY / weekdayGroup->name;
            }

            return directory;
        }

        /**
         * @brief Writes JSON to a temporary file and renames it into place, so readers never see a partial file.
         */
        void WriteJsonFileAtomically(const Common::JsonUtility &json, const std::filesystem::path &path, const std::string_view description) {
            std::filesystem::create_directories(path.parent_path());

            // Write to a temporary file first.
            std::filesystem::path temporaryPath = path;
            temporaryPath += ".tmp";

            {
                std::ofstream file(
                    temporaryPath,
                    std::ios::binary | std::ios::trunc
                );

                if (file.is_open() == false) {
                    throw std::runtime_error(
                        std::format("Unable to open {} for writing: {}", description, temporaryPath.string())
                    );
                }

                file << json.ToString();
                file.flush();

                if (file.good() == false) {
                    throw std::runtime_error(
                        std::format("Failed writing {}: {}", description, temporaryPath.string())
                    );
                }
            }

            // Rename the completed file into place.
            std::error_code error;

            std::filesystem::rename(
                temporaryPath,
                path,
                error
            );

            // Fallback for platforms that don't replace existing destinations.
            if (error) {
                std::error_code removeError;

                std::filesystem::remove(
                    path,
                    removeError
                );

                error.clear();

                std::filesystem::rename(
                    temporaryPath,
                    path,
                    error
                );
            }

            if (error) {
                std::filesystem::remove(temporaryPath);

                throw std::runtime_error(
                    std::format("Failed to install {}: {}", description, error.message())
                );
            }
        }

        std::vector<std::string> WeekdayNames(const WeekdayGroup &group) {
            std::vector<std::string> names{};

            for (const std::chrono::weekday day : group.Weekdays()) {
                names.emplace_back(WeekdayName(day));
            }

            std::sort(names.begin(), names.end());
            return names;
        }

    }

    std::filesystem::path HalfHourIndex::GetIndexPath(
        const std::size_t numOfSeconds,
        const std::size_t inputWindowSeconds,
        std::optional<std::string_view> indexType,
        const std::optional<WeekdayGroup> &weekdayGroup
    ) {
        return SubIndexDirectory(Common::Storage::INDEXES_BASE_PATH, inputWindowSeconds, indexType, weekdayGroup) /
            std::format(
                "{}.index",
                GetHalfHourName(numOfSeconds)
            );
    }

    std::filesystem::path HalfHourIndex::GetMetadataPath(
        const std::size_t numOfSeconds,
        const std::size_t inputWindowSeconds,
        std::optional<std::string_view> indexType,
        const std::optional<WeekdayGroup> &weekdayGroup
    ) {
        return SubIndexDirectory(Common::Storage::METADATA_BASE_PATH, inputWindowSeconds, indexType, weekdayGroup) /
            std::format(
                "{}.meta",
                GetHalfHourName(numOfSeconds)
            );
    }

    std::filesystem::path HalfHourIndex::GetNormalizationPath(
        const std::size_t numOfSeconds,
        const std::size_t inputWindowSeconds,
        std::optional<std::string_view> indexType,
        const std::optional<WeekdayGroup> &weekdayGroup
    ) {
        const std::filesystem::path metadataPath = GetMetadataPath(numOfSeconds, inputWindowSeconds, indexType, weekdayGroup);
        return metadataPath.parent_path() / (metadataPath.stem().string() + ".normalization.json");
    }

    std::filesystem::path HalfHourIndex::GetWeekdayManifestPath(
        const std::size_t numOfSeconds,
        const std::size_t inputWindowSeconds,
        std::optional<std::string_view> indexType,
        const WeekdayGroup &weekdayGroup
    ) {
        const std::filesystem::path metadataPath = GetMetadataPath(numOfSeconds, inputWindowSeconds, indexType, weekdayGroup);
        return metadataPath.parent_path() / (metadataPath.stem().string() + ".weekdays.json");
    }

    bool HalfHourIndex::FilesExist(
        const std::size_t numOfSeconds,
        const std::size_t inputWindowSeconds,
        std::optional<std::string_view> indexType,
        const std::optional<WeekdayGroup> &weekdayGroup
    ) {
        // The direction index does not use normalization-parameter files.
        const bool requiresNormalization = (indexType.has_value() && *indexType == INX_TYPE_DIRECTION) == false;

        const bool filesExist = Common::Storage::FileExists(GetIndexPath(numOfSeconds, inputWindowSeconds, indexType, weekdayGroup)) &&
            Common::Storage::FileExists(GetMetadataPath(numOfSeconds, inputWindowSeconds, indexType, weekdayGroup)) &&
            (requiresNormalization == false ||
                Common::Storage::FileExists(GetNormalizationPath(numOfSeconds, inputWindowSeconds, indexType, weekdayGroup)));

        if (filesExist == false || weekdayGroup.has_value() == false) {
            return filesExist;
        }

        const std::filesystem::path manifestPath = GetWeekdayManifestPath(numOfSeconds, inputWindowSeconds, indexType, *weekdayGroup);

        if (Common::Storage::FileExists(manifestPath) == false) {
            return false;
        }

        try {
            ValidateWeekdayManifest(manifestPath, inputWindowSeconds, *weekdayGroup);
        }
        catch (const std::exception &error) {
            Logger::Warning(std::format("Weekday index is not usable for its configured group: {}", error.what()));
            return false;
        }

        return true;
    }

    void HalfHourIndex::DeleteFiles(
        const std::size_t numOfSeconds,
        const std::size_t inputWindowSeconds,
        std::optional<std::string_view> indexType,
        const std::optional<WeekdayGroup> &weekdayGroup
    ) {
        Common::Storage::DeleteFile(GetIndexPath(numOfSeconds, inputWindowSeconds, indexType, weekdayGroup));
        Common::Storage::DeleteFile(GetMetadataPath(numOfSeconds, inputWindowSeconds, indexType, weekdayGroup));
        Common::Storage::DeleteFile(GetNormalizationPath(numOfSeconds, inputWindowSeconds, indexType, weekdayGroup));

        if (weekdayGroup.has_value()) {
            Common::Storage::DeleteFile(GetWeekdayManifestPath(numOfSeconds, inputWindowSeconds, indexType, *weekdayGroup));
        }
    }

    void HalfHourIndex::ValidateWeekdayManifest(
        const std::filesystem::path &manifestPath,
        const std::size_t inputWindowSeconds,
        const WeekdayGroup &weekdayGroup
    ) {
        if (Common::Storage::FileExists(manifestPath) == false) {
            throw std::runtime_error(std::format(
                "Weekday group manifest missing: {}. The weekday indexes must be rebuilt.",
                manifestPath.string()
            ));
        }

        const Common::JsonUtility json = Common::JsonUtility::FromFile(manifestPath);

        if (json.Get<int>("schema_version") != 1) {
            throw std::runtime_error(std::format("Unsupported weekday group manifest version: {}", manifestPath.string()));
        }

        if (json.Get<std::size_t>("input_window_seconds") != inputWindowSeconds) {
            throw std::runtime_error(std::format(
                "{} was built for a {}-second input window, not {} seconds.",
                manifestPath.string(),
                json.Get<std::size_t>("input_window_seconds"),
                inputWindowSeconds
            ));
        }

        const std::string storedGroup = json.Get<std::string>("weekday_group");
        std::vector<std::string> storedWeekdays = json.Get<std::vector<std::string>>("weekdays");
        std::sort(storedWeekdays.begin(), storedWeekdays.end());

        if (storedGroup != weekdayGroup.name || storedWeekdays != WeekdayNames(weekdayGroup)) {
            throw std::runtime_error(std::format(
                "{} holds weekday group \"{}\" with different weekdays than the configured \"{}\"; "
                "rebuild the weekday indexes after changing DIRECTION_WEEKDAY_GROUPS.",
                manifestPath.string(),
                storedGroup,
                weekdayGroup.name
            ));
        }
    }

    void HalfHourIndex::SaveWeekdayManifestToDisk() const {
        Common::JsonUtility json{};

        json.Set("schema_version", 1);
        json.Set("input_window_seconds", _inputWindowSeconds);
        json.Set("index_type", std::string(_indexType.value_or("")));
        json.Set("weekday_group", std::string(_weekdayGroup->name));
        json.Set("weekdays", WeekdayNames(*_weekdayGroup));

        WriteJsonFileAtomically(json, _weekdayManifestPath, "weekday group manifest");

        Logger::Info(std::format("Saved weekday group manifest: {}", _weekdayManifestPath.string()), true);
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
        std::optional<std::string_view> indexType,
        std::optional<WeekdayGroup> weekdayGroup
    )
        : _inputWindowSeconds(inputWindowSeconds),
        _indexType(indexType),
        _dimension(static_cast<int>(InputVectorDimension(inputWindowSeconds))),
        _index(_dimension),
        _weekdayGroup(weekdayGroup),
        _indexPath(GetIndexPath(numOfSeconds, inputWindowSeconds, indexType, weekdayGroup)),
        _metadataPath(GetMetadataPath(numOfSeconds, inputWindowSeconds, indexType, weekdayGroup)),
        _normalizationPath(GetNormalizationPath(numOfSeconds, inputWindowSeconds, indexType, weekdayGroup))
    {
        if (IsSupportedInputWindow(inputWindowSeconds) == false) {
            throw std::invalid_argument(
                std::format("Unsupported input window length: {}", inputWindowSeconds)
            );
        }

        if (weekdayGroup.has_value()) {
            _weekdayManifestPath = GetWeekdayManifestPath(numOfSeconds, inputWindowSeconds, indexType, *weekdayGroup);
        }

        Initialize();
    }

    void HalfHourIndex::Initialize() {
        if (Common::Storage::FileExists(this->_indexPath)) {
            // Checked before reading the vectors: a weekday index from another grouping must never be searched.
            if (this->_weekdayGroup.has_value()) {
                ValidateWeekdayManifest(this->_weekdayManifestPath, this->_inputWindowSeconds, *this->_weekdayGroup);
            }

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

                // Last, so the manifest only exists once the index and metadata are on disk.
                if (this->_weekdayGroup.has_value()) {
                    SaveWeekdayManifestToDisk();
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
        Logger::Info(
            std::format("Loading metadata: {}", this->_metadataPath.string()),
            true
        );

        this->_windowLocations.reserve(SECONDS_PER_HALF_HOUR);

        std::ifstream metaFile(this->_metadataPath, std::ios::binary);

        if (metaFile && metaFile.is_open()) {
            WindowLocation loc{};
            while (metaFile.read(reinterpret_cast<char*>(&loc), sizeof(loc))) {
                this->_windowLocations.push_back(loc);
            }
            if (metaFile.eof()) {
                Logger::Info("Successfully loaded metadata", true);
            }

            metaFile.close();
        }
        else {
            Logger::Error("Failed to load metadata file", true);
        }
    }

    void HalfHourIndex::SaveMetadataToDisk() {
        Logger::Info(
            std::format("Saving metadata: {}", this->_metadataPath.string()),
            true
        );

        if (Common::Storage::FileExists(this->_metadataPath.parent_path()) == false) {
            Common::Storage::CreateDirectory(this->_metadataPath.parent_path());
        }

        if (Common::Storage::FileExists(this->_metadataPath)) {
            Common::Storage::DeleteFile(this->_metadataPath);
        }

        std::ofstream metaFile(this->_metadataPath, std::ios::binary);

        if (metaFile && metaFile.is_open()) {
            for (const WindowLocation &loc : this->_windowLocations) {
                metaFile.write(reinterpret_cast<const char*>(&loc), sizeof(WindowLocation));

                if (!metaFile) {
                    Logger::Error(
                        std::format(
                            "Failed while writing metadata: {}",
                            this->_metadataPath.string()
                        ),
                        true
                    );
                    metaFile.close();
                    break;
                }
            }

            metaFile.close();

            Logger::Info(
                std::format(
                    "Successfully saved {} metadata entries",
                    this->_windowLocations.size()
                ),
                true
            );
        }
        else {
            Logger::Error(
                std::format(
                    "Failed to open metadata file for writing: {}",
                    this->_metadataPath.string()
                ),
                true
            );
        }
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
            .sample_count = json.Get<std::uint64_t>("sample_count"),

            .mid_delta_mean = json.Get<double>("mid_delta_mean"),
            .mid_delta_stddev = json.Get<double>("mid_delta_stddev")
        };

        ValidateNormalizationParameters(parameters);

        _normalizationParameters = parameters;
        _hasNormalizationParameters = true;

        Logger::Info(
            std::format(
                "Loaded normalization parameters: {} ({} samples)",
                _normalizationPath.string(),
                parameters.sample_count
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

        json.Set("sample_count", _normalizationParameters.sample_count);

        json.Set("mid_delta_mean", _normalizationParameters.mid_delta_mean);
        json.Set("mid_delta_stddev", _normalizationParameters.mid_delta_stddev);

        WriteJsonFileAtomically(json, _normalizationPath, "normalization JSON");

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
                        .distance = distances[i],
                        .tradingDayId = location.tradingDay,
                        .recordIndex = location.recordIndex
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
