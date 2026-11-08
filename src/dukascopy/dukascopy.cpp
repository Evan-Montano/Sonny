// dukascopy.cpp

// Implementation of the Dukascopy namespace for exporting historical raw tick

// data into a binary format used by Sonny's ML architecture.

#include "dukascopy.hpp"
#include "../common/jsonutil.hpp"
#include "../common/logger.hpp"
#include "../common/storage.hpp"
#include "../client/simplecurlwrapper.hpp"
#include "../core/setup.hpp"
#include "../core/structures.hpp"
#include "../ui/ui.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ftxui/dom/elements.hpp>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Dukascopy {

    inline static const std::string infoUrl =
        "https://jetta.dukascopy.com/v1/instruments/SPY.US-USD";

    inline static const std::string ticksUrlBase =
        "https://jetta.dukascopy.com/v1/ticks/SPY.US-USD/";

    constexpr std::int64_t MILLISECONDS_PER_SECOND = 1000;
    constexpr std::int64_t SECONDS_PER_MINUTE = 60;
    constexpr std::int64_t MINUTES_PER_HOUR = 60;
    constexpr std::int64_t HOURS_PER_DAY = 24;
    constexpr std::int64_t SECONDS_PER_HOUR = SECONDS_PER_MINUTE * MINUTES_PER_HOUR;
    constexpr std::int64_t MILLISECONDS_PER_MINUTE = SECONDS_PER_MINUTE * MILLISECONDS_PER_SECOND;
    constexpr std::int64_t MILLISECONDS_PER_HOUR = SECONDS_PER_HOUR * MILLISECONDS_PER_SECOND;
    constexpr float MIDPOINT_AVERAGE_FACTOR = 0.5f;
    constexpr int SESSION_OPEN_HOUR = 9;
    constexpr int SESSION_OPEN_MINUTE = 30;
    constexpr std::int64_t MAX_IN_SESSION_TICK_GAP_MINUTES = 5;
    constexpr std::size_t FINGERPRINT_BUFFER_SIZE_BYTES = 64 * 1024;
    constexpr std::size_t ML_RECORD_EXPECTED_SIZE_BYTES = 12;
    constexpr long HTTP_SUCCESS_STATUS_MIN = 200;
    constexpr long HTTP_SUCCESS_STATUS_MAX_EXCLUSIVE = 300;
    constexpr int MIN_PROGRESS_WINDOW_WIDTH = 55;

    // Regular US session length: 9:30 to 16:00 New York time (6.5 hours).
    // Early closes require an exchange calendar and are not inferred from tick silence.
    constexpr Common::UnixTimestamp sessionLengthSeconds = 23400;

    // SPY is highly liquid during regular hours. A longer gap may indicate a
    // truncated hourly response or an incomplete session rather than inactivity.
    // This is an integrity heuristic, not proof that every source tick was delivered.
    constexpr std::int64_t MAX_IN_SESSION_TICK_GAP_MS =
        MAX_IN_SESSION_TICK_GAP_MINUTES * MILLISECONDS_PER_MINUTE;

    enum CorpusOhlcColumn : std::size_t {
        OPEN_BID_COLUMN,
        HIGH_BID_COLUMN,
        LOW_BID_COLUMN,
        CLOSE_BID_COLUMN,
        OPEN_ASK_COLUMN,
        HIGH_ASK_COLUMN,
        LOW_ASK_COLUMN,
        CLOSE_ASK_COLUMN,
        CORPUS_OHLC_COLUMN_COUNT
    };

    std::unordered_map<std::string, std::string> headers = {
        { "Accept", "*/*" },
        { "Accept-Language", "en-US,en;q=0.6" },
        { "Cache-Control", "no-cache" },
        { "Origin", "https://widgets.dukascopy.com" },
        { "Pragma", "no-cache" },
        { "Priority", "u=1, i" },
        { "Referer", "https://widgets.dukascopy.com/" },
        { "sec-ch-ua", "\"Not:A-Brand\";v=\"8\", \"Chromium\";v=\"156\", \"Brave\";v=\"156\"" },
        { "sec-ch-ua-mobile", "?0" },
        { "sec-ch-ua-platform", "\"Windows\"" },
        { "sec-fetch-dest", "empty" },
        { "sec-fetch-mode", "cors" },
        { "sec-fetch-site", "same-site" },
        { "sec-gpc", "1" },
        { "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/156.0.0.0 Safari/537.36" }
    };

    struct ApiTickResponse {
        Common::UnixTimestamp Timestamp;
        float Multiplier;
        float Ask;
        float Bid;

        // Deltas.
        std::vector<std::int32_t> Times;
        std::vector<std::int16_t> Asks;
        std::vector<std::int16_t> Bids;

        // Actual volume values.
        std::vector<std::int32_t> AskVolumes;
        std::vector<std::int32_t> BidVolumes;
    };

    struct HourlyTickResponse {
        std::int64_t UtcHour;
        ApiTickResponse Data;
    };

    struct OHLCV_BidAsk {
        Common::UnixTimestamp Timestamp;
        std::int64_t OpenBid;
        std::int64_t HighBid;
        std::int64_t LowBid;
        std::int64_t CloseBid;

        std::int64_t OpenAsk;
        std::int64_t HighAsk;
        std::int64_t LowAsk;
        std::int64_t CloseAsk;

        std::uint32_t BidVolume;
        std::uint32_t AskVolume;

        float Multiplier;
    };

    /**
     * @brief Builds the Dukascopy URL for a trading date.
     */
    std::string BuildTicksUrl(const Common::DateTime& dt) {

        return ticksUrlBase +
            std::to_string(dt.GetYear()) + "/" +
            std::to_string(dt.GetMonth()) + "/" +
            std::to_string(dt.GetDay()) + "/";
    }

    /**
    * @brief A single decoded tick with an absolute time and absolute integer prices.
    */
    struct Tick {
        std::int64_t TimeMs;
        std::int64_t Bid;
        std::int64_t Ask;
        std::uint32_t BidVolume;
        std::uint32_t AskVolume;
        float Multiplier;
    };

    /**
    * @brief Decodes the delta-encoded hourly responses into absolute ticks ordered by time.
    *
    * @param tickResponses
    * @return std::vector<Tick>
    */
    std::optional<std::vector<Tick>> DecodeTicks(
        const std::vector<HourlyTickResponse>& tickResponses,
        const std::int64_t sessionOpenMs,
        const std::int64_t sessionCloseMs)
    {
        std::vector<Tick> ticks;
        std::optional<float> expectedMultiplier;

        for (const HourlyTickResponse& hourlyResponse : tickResponses) {
            const ApiTickResponse& response = hourlyResponse.Data;
            const float multiplier = response.Multiplier;

            if (response.Times.empty() ||
                response.Times.size() != response.Bids.size() ||
                response.Times.size() != response.Asks.size() ||
                response.Times.size() != response.BidVolumes.size() ||
                response.Times.size() != response.AskVolumes.size()) {
                return std::nullopt;
            }

            if (expectedMultiplier.has_value() == false) {
                expectedMultiplier = multiplier;
            }
            else if (multiplier != *expectedMultiplier) {
                // The corpus stores one multiplier for the entire trading day.
                // A multiplier change cannot be represented safely by this format.
                return std::nullopt;
            }

            const std::int64_t hourStartMs = hourlyResponse.UtcHour * MILLISECONDS_PER_HOUR;
            const std::int64_t hourEndMs = hourStartMs + MILLISECONDS_PER_HOUR;

            auto runningBid = static_cast<std::int64_t>(std::round(response.Bid / multiplier));
            auto runningAsk = static_cast<std::int64_t>(std::round(response.Ask / multiplier));
            auto runningTime = static_cast<std::int64_t>(response.Timestamp);

            if (runningBid <= 0 || runningAsk < runningBid) {
                return std::nullopt;
            }

            bool hasSessionTick = false;

            for (std::size_t i = 0; i < response.Times.size(); ++i) {
                const std::int64_t timeDelta = response.Times[i];

                if (timeDelta < 0 ||
                    runningTime > std::numeric_limits<std::int64_t>::max() - timeDelta) {
                    return std::nullopt;
                }

                runningTime += timeDelta;

                if (i > 0) {
                    runningBid += response.Bids[i];
                    runningAsk += response.Asks[i];
                }

                if (runningBid <= 0 || runningAsk < runningBid ||
                    runningTime < hourStartMs || runningTime >= hourEndMs ||
                    response.BidVolumes[i] < 0 || response.AskVolumes[i] < 0) {
                    return std::nullopt;
                }

                if (runningTime >= sessionOpenMs && runningTime < sessionCloseMs) {
                    hasSessionTick = true;
                }

                ticks.push_back(Tick{
                    .TimeMs = runningTime,
                    .Bid = runningBid,
                    .Ask = runningAsk,
                    .BidVolume = static_cast<std::uint32_t>(response.BidVolumes[i]),
                    .AskVolume = static_cast<std::uint32_t>(response.AskVolumes[i]),
                    .Multiplier = multiplier
                });
            }

            // Every UTC hour that intersects the regular session must contain
            // at least one actual in-session tick. An empty/irrelevant response
            // must not be mistaken for a successfully downloaded hour.
            if (hasSessionTick == false) {
                return std::nullopt;
            }
        }

        if (ticks.empty()) {
            return std::nullopt;
        }

        std::ranges::stable_sort(ticks, {}, &Tick::TimeMs);
        return ticks;
    }

    /**
     * @brief Creates a zero-volume candle using one tick for all OHLC values.
     */
    OHLCV_BidAsk FlatCandle(const Common::UnixTimestamp second, const Tick& tick) {
        return OHLCV_BidAsk{
            .Timestamp = second,
            .OpenBid = tick.Bid,
            .HighBid = tick.Bid,
            .LowBid = tick.Bid,
            .CloseBid = tick.Bid,

            .OpenAsk = tick.Ask,
            .HighAsk = tick.Ask,
            .LowAsk = tick.Ask,
            .CloseAsk = tick.Ask,

            .BidVolume = 0,
            .AskVolume = 0,

            .Multiplier = tick.Multiplier
        };
    }

    /**
    * @brief Folds a tick into a candle that already holds an earlier tick from the same second.
    */

    bool AddTickToCandle(OHLCV_BidAsk& candle, const Tick& tick) {
        if (std::numeric_limits<std::uint32_t>::max() - candle.BidVolume < tick.BidVolume ||
            std::numeric_limits<std::uint32_t>::max() - candle.AskVolume < tick.AskVolume) {
            return false;
        }

        candle.HighBid = std::max(candle.HighBid, tick.Bid);
        candle.LowBid = std::min(candle.LowBid, tick.Bid);
        candle.CloseBid = tick.Bid;

        candle.HighAsk = std::max(candle.HighAsk, tick.Ask);
        candle.LowAsk = std::min(candle.LowAsk, tick.Ask);
        candle.CloseAsk = tick.Ask;

        candle.BidVolume += tick.BidVolume;
        candle.AskVolume += tick.AskVolume;

        candle.Multiplier = tick.Multiplier;
        return true;
    }

    /**
    * @brief Builds exactly one candle per second of the regular session, so record N is always
    * N seconds after the open. Seconds without ticks repeat the previous close with zero volume.
    *
    * @param ticks Decoded and validated ticks for the day.
    * @param sessionOpen Session open as a Unix timestamp in seconds.
    * @param sessionClose Session close as a Unix timestamp in seconds (exclusive).
    * @return One candle per session second, or empty if no ticks fall inside the session.
    */
    std::optional<std::vector<OHLCV_BidAsk>>
    ExtractOneSecondCandlesticks(
        const std::vector<Tick>& ticks,
        const Common::UnixTimestamp sessionOpen,
        const Common::UnixTimestamp sessionClose) {
        const auto toMs = [](const Common::UnixTimestamp seconds) {
            return static_cast<std::int64_t>(seconds) * MILLISECONDS_PER_SECOND;
        };

        const std::int64_t sessionOpenMs = toMs(sessionOpen);
        const std::int64_t sessionCloseMs = toMs(sessionClose);

        auto next = std::ranges::lower_bound(ticks, sessionOpenMs, {}, &Tick::TimeMs);
        const auto sessionEnd = std::ranges::lower_bound(ticks, sessionCloseMs, {}, &Tick::TimeMs);

        if (next == sessionEnd) {
            return std::nullopt;
        }

        // Reject long gaps in actual session activity. Shorter quiet periods are
        // still represented by zero-volume candles carrying forward the last quote.
        std::int64_t previousTickMs = sessionOpenMs;
        for (auto tick = next; tick != sessionEnd; ++tick) {
            if (tick->TimeMs - previousTickMs > MAX_IN_SESSION_TICK_GAP_MS) {
                return std::nullopt;
            }

            previousTickMs = tick->TimeMs;
        }

        if (sessionCloseMs - previousTickMs > MAX_IN_SESSION_TICK_GAP_MS) {
            return std::nullopt;
        }

        // Use the last quote before the open whenever one is available.
        // If there is no pre-open quote, an in-session quote must arrive during
        // the first second. Never backfill earlier seconds from a later quote.
        const Tick* previous = nullptr;

        if (next != ticks.begin()) {
            previous = &*std::prev(next);
        }
        else if (next->TimeMs < sessionOpenMs + MILLISECONDS_PER_SECOND) {
            previous = &*next;
        }
        else {
            return std::nullopt;
        }

        std::vector<OHLCV_BidAsk> result;
        result.reserve(static_cast<std::size_t>(sessionClose - sessionOpen));

        for (Common::UnixTimestamp second = sessionOpen; second < sessionClose; ++second) {
            OHLCV_BidAsk candle{};
            bool hasCandle = false;

            if (previous != nullptr) {
                candle = FlatCandle(second, *previous);
                hasCandle = true;
            }

            const std::int64_t secondEndMs = toMs(second + 1);
            bool firstTick = true;

            for (; next != sessionEnd && next->TimeMs < secondEndMs; ++next) {
                if (firstTick) {
                    candle = FlatCandle(second, *next);
                    firstTick = false;
                    hasCandle = true;
                }

                if (AddTickToCandle(candle, *next) == false) {
                    return std::nullopt;
                }

                previous = &*next;
            }

            // This can only happen when there is no known quote to carry forward.
            if (hasCandle == false) {
                return std::nullopt;
            }

            result.push_back(candle);
        }

        if (result.size() != static_cast<std::size_t>(sessionLengthSeconds)) {
            return std::nullopt;
        }

        return result;
    }

    /**
     * @brief Returns the staging path used while writing a file.
     */
    std::filesystem::path BuildTemporaryPath(const std::filesystem::path& path) {
        std::filesystem::path temporaryPath = path;
        temporaryPath += ".tmp";
        return temporaryPath;
    }

    /**
     * @brief Returns the backup path used during file replacement.
     */
    std::filesystem::path BuildBackupPath(const std::filesystem::path& path) {
        std::filesystem::path backupPath = path;
        backupPath += ".bak";
        return backupPath;
    }

    /**
     * @brief Returns the validation marker path for a corpus file.
     */
    std::filesystem::path BuildValidationPath(const std::filesystem::path& corpusPath) {
        std::filesystem::path validationPath = corpusPath;
        validationPath += ".validated";
        return validationPath;
    }

    /**
     * @brief Calculates a file size and FNV-1a fingerprint.
     */
    std::optional<std::pair<std::uintmax_t, std::uint64_t>> CalculateFileFingerprint(
        const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (file.is_open() == false) {
            return std::nullopt;
        }

        // FNV-1a detects accidental file corruption; it is not cryptographic.
        constexpr std::uint64_t FNV_OFFSET_BASIS = 14695981039346656037ull;
        constexpr std::uint64_t FNV_PRIME = 1099511628211ull;

        std::uint64_t hash = FNV_OFFSET_BASIS;
        std::uintmax_t fileSize = 0;
        char buffer[FINGERPRINT_BUFFER_SIZE_BYTES];

        while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
            const std::streamsize bytesRead = file.gcount();
            fileSize += static_cast<std::uintmax_t>(bytesRead);

            for (std::streamsize i = 0; i < bytesRead; ++i) {
                hash ^= static_cast<unsigned char>(buffer[i]);
                hash *= FNV_PRIME;
            }
        }

        if (file.eof() == false || file.bad()) {
            return std::nullopt;
        }

        return std::pair{ fileSize, hash };
    }

    /**
     * @brief Removes a file if present and reports whether removal succeeded.
     */
    bool RemoveFileIfExists(const std::filesystem::path& path) {
        std::error_code error;
        std::filesystem::remove(path, error);

        if (error) {
            Logger::Error(std::format("Could not remove file {}: {}", path.string(), error.message()), true);
            return false;
        }

        return true;
    }

    /**
     * @brief Returns the transaction marker path for a corpus file.
     */
    std::filesystem::path BuildTransactionPath(const std::filesystem::path& path) {
        std::filesystem::path transactionPath = path;
        transactionPath += ".txn";
        return transactionPath;
    }

    /**
     * @brief Returns the commit marker path for a corpus file.
     */
    std::filesystem::path BuildCommitPath(const std::filesystem::path& path) {
        std::filesystem::path commitPath = path;
        commitPath += ".commit";
        return commitPath;
    }

    /**
     * @brief Recovers or cleans up artifacts from an interrupted daily file commit.
     */
    bool RecoverInterruptedDayCommit(const std::filesystem::path& corpusPath, const std::filesystem::path& recordsPath) {
        const std::filesystem::path corpusBackup = BuildBackupPath(corpusPath);
        const std::filesystem::path recordsBackup = BuildBackupPath(recordsPath);
        const std::filesystem::path corpusTemporary = BuildTemporaryPath(corpusPath);
        const std::filesystem::path recordsTemporary = BuildTemporaryPath(recordsPath);
        const std::filesystem::path validationTemporary =
            BuildTemporaryPath(BuildValidationPath(corpusPath));
        const std::filesystem::path transactionPath = BuildTransactionPath(corpusPath);
        const std::filesystem::path commitPath = BuildCommitPath(corpusPath);

        std::error_code error;
        const bool hasTransaction = std::filesystem::exists(transactionPath, error);
        if (error) {
            Logger::Error(std::format("Could not inspect {}: {}", transactionPath.string(), error.message()), true);
            return false;
        }

        const bool hasCommit = std::filesystem::exists(commitPath, error);
        if (error) {
            Logger::Error(std::format("Could not inspect {}: {}", commitPath.string(), error.message()), true);
            return false;
        }

        const bool hasCorpusBackup = std::filesystem::exists(corpusBackup, error);
        if (error) {
            Logger::Error(std::format("Could not inspect {}: {}", corpusBackup.string(), error.message()), true);
            return false;
        }

        const bool hasRecordsBackup = std::filesystem::exists(recordsBackup, error);
        if (error) {
            Logger::Error(std::format("Could not inspect {}: {}", recordsBackup.string(), error.message()), true);
            return false;
        }

        if (hasCommit) {
            // Both new files were installed before the commit marker was written.
            // Keep them and finish best-effort cleanup after an interrupted cleanup.
            if (RemoveFileIfExists(corpusBackup) == false ||
                RemoveFileIfExists(recordsBackup) == false ||
                RemoveFileIfExists(corpusTemporary) == false ||
                RemoveFileIfExists(recordsTemporary) == false ||
                RemoveFileIfExists(validationTemporary) == false ||
                RemoveFileIfExists(transactionPath) == false) {
                return false;
            }

            return RemoveFileIfExists(commitPath);
        }

        if (hasTransaction) {
            bool corpusOriginallyExisted = false;
            bool recordsOriginallyExisted = false;

            std::ifstream transactionFile(transactionPath);
            if (transactionFile.is_open() == false ||
                !(transactionFile >> corpusOriginallyExisted >> recordsOriginallyExisted)) {
                Logger::Error("Daily export transaction marker is unreadable; refusing to modify the files.", true);
                return false;
            }

            if (hasCorpusBackup) {
                if (RemoveFileIfExists(corpusPath) == false) {
                    return false;
                }

                std::filesystem::rename(corpusBackup, corpusPath, error);
                if (error) {
                    Logger::Error(std::format("Could not restore {}: {}", corpusPath.string(), error.message()), true);
                    return false;
                }
            }
            else if (corpusOriginallyExisted == false && RemoveFileIfExists(corpusPath) == false) {
                return false;
            }

            if (hasRecordsBackup) {
                if (RemoveFileIfExists(recordsPath) == false) {
                    return false;
                }

                std::filesystem::rename(recordsBackup, recordsPath, error);
                if (error) {
                    Logger::Error(std::format("Could not restore {}: {}", recordsPath.string(), error.message()), true);
                    return false;
                }
            }
            else if (recordsOriginallyExisted == false && RemoveFileIfExists(recordsPath) == false) {
                return false;
            }

            if (RemoveFileIfExists(corpusTemporary) == false ||
                RemoveFileIfExists(recordsTemporary) == false ||
                RemoveFileIfExists(validationTemporary) == false ||
                RemoveFileIfExists(transactionPath) == false) {
                return false;
            }

            Logger::Warning(
                "Recovered an interrupted daily corpus replacement; the previous files were restored where available.",
                true
            );
        }
        else if (hasCorpusBackup || hasRecordsBackup) {
            Logger::Error(
                "Found a daily backup without its transaction marker; refusing to guess which files should be retained.",
                true
            );
            return false;
        }

        return RemoveFileIfExists(corpusTemporary) &&
            RemoveFileIfExists(recordsTemporary) &&
            RemoveFileIfExists(validationTemporary);
    }

    /**
     * @brief Checks that corpus and record files have a complete valid structure.
     */
    bool HasCompleteDayStructure(
        const std::filesystem::path& corpusPath,
        const std::filesystem::path& recordsPath)
    {
        static_assert(sizeof(Core::MLRecord) == ML_RECORD_EXPECTED_SIZE_BYTES);

        std::error_code error;

        if (std::filesystem::exists(corpusPath, error) == false || error) {
            return false;
        }

        if (std::filesystem::exists(recordsPath, error) == false || error) {
            return false;
        }

        const std::uintmax_t expectedRecordBytes =
            static_cast<std::uintmax_t>(sessionLengthSeconds) * sizeof(Core::MLRecord);

        const std::uintmax_t actualRecordBytes = std::filesystem::file_size(recordsPath, error);
        if (error || actualRecordBytes != expectedRecordBytes) {
            return false;
        }

        std::ifstream corpusFileStream(corpusPath);
        if (corpusFileStream.is_open() == false) {
            return false;
        }

        std::string line;
        if (!std::getline(corpusFileStream, line) ||
            line.starts_with("multiplier=") == false) {
            return false;
        }

        const std::string multiplierText = line.substr(std::string("multiplier=").size());
        float multiplier = 0.0f;
        const auto [multiplierEnd, multiplierError] = std::from_chars(
            multiplierText.data(),
            multiplierText.data() + multiplierText.size(),
            multiplier
        );

        if (multiplierError != std::errc{} ||
            multiplierEnd != multiplierText.data() + multiplierText.size() ||
            std::isfinite(multiplier) == false || multiplier <= 0.0f) {
            return false;
        }

        std::size_t candleCount = 0;
        while (std::getline(corpusFileStream, line)) {
            std::array<std::int64_t, CORPUS_OHLC_COLUMN_COUNT> values{};
            std::size_t valueStart = 0;

            for (std::size_t i = 0; i < values.size(); ++i) {
                const std::size_t comma = line.find(',', valueStart);
                const std::size_t valueEnd =
                    i == values.size() - 1 ? line.size() : comma;

                if ((i == values.size() - 1 && comma != std::string::npos) ||
                    (i != values.size() - 1 && comma == std::string::npos) ||
                    valueEnd <= valueStart) {
                    return false;
                }

                const auto [parsedEnd, parseError] = std::from_chars(
                    line.data() + valueStart,
                    line.data() + valueEnd,
                    values[i]
                );

                if (parseError != std::errc{} || parsedEnd != line.data() + valueEnd || values[i] <= 0) {
                    return false;
                }

                valueStart = valueEnd + 1;
            }

            const auto validOhlc = [](const std::int64_t open,
                                      const std::int64_t high,
                                      const std::int64_t low,
                                      const std::int64_t close) {
                return high >= open && high >= close &&
                    low <= open && low <= close && high >= low;
            };

            if (validOhlc(values[OPEN_BID_COLUMN], values[HIGH_BID_COLUMN], values[LOW_BID_COLUMN], values[CLOSE_BID_COLUMN]) == false ||
                validOhlc(values[OPEN_ASK_COLUMN], values[HIGH_ASK_COLUMN], values[LOW_ASK_COLUMN], values[CLOSE_ASK_COLUMN]) == false ||
                values[OPEN_ASK_COLUMN] < values[OPEN_BID_COLUMN] || values[HIGH_ASK_COLUMN] < values[HIGH_BID_COLUMN] ||
                values[LOW_ASK_COLUMN] < values[LOW_BID_COLUMN] || values[CLOSE_ASK_COLUMN] < values[CLOSE_BID_COLUMN]) {
                return false;
            }

            ++candleCount;
        }

        if (corpusFileStream.eof() == false ||
            candleCount != static_cast<std::size_t>(sessionLengthSeconds)) {
            return false;
        }

        std::ifstream recordsFileStream(recordsPath, std::ios::binary);
        if (recordsFileStream.is_open() == false) {
            return false;
        }

        Core::MLRecord record{};
        std::size_t recordCount = 0;

        while (recordsFileStream.read(reinterpret_cast<char*>(&record), sizeof(record))) {
            if (std::isfinite(record.mid_delta) == false ||
                std::isfinite(record.spread_delta) == false ||
                std::isfinite(record.volume) == false ||
                record.volume < 0.0f) {
                return false;
            }

            ++recordCount;
        }

        return recordsFileStream.eof() &&
            recordCount == static_cast<std::size_t>(sessionLengthSeconds);
    }

    /**
     * @brief Checks the daily files against their validation marker and fingerprints.
     */
    bool IsCompleteDayOnDisk(
        const std::filesystem::path& corpusPath,
        const std::filesystem::path& recordsPath)
    {
        if (HasCompleteDayStructure(corpusPath, recordsPath) == false) {
            return false;
        }

        const auto corpusFingerprint = CalculateFileFingerprint(corpusPath);
        const auto recordsFingerprint = CalculateFileFingerprint(recordsPath);

        if (corpusFingerprint.has_value() == false || recordsFingerprint.has_value() == false) {
            return false;
        }

        std::ifstream validationFile(BuildValidationPath(corpusPath));
        std::string validationLabel;
        std::uintmax_t expectedCorpusSize = 0;
        std::uint64_t expectedCorpusHash = 0;
        std::uintmax_t expectedRecordsSize = 0;
        std::uint64_t expectedRecordsHash = 0;

        if (validationFile.is_open() == false ||
            !std::getline(validationFile, validationLabel) ||
            validationLabel != "sonny-dukascopy-full-session-v1" ||
            !(validationFile >> expectedCorpusSize >> expectedCorpusHash) ||
            !(validationFile >> expectedRecordsSize >> expectedRecordsHash)) {
            return false;
        }

        return corpusFingerprint->first == expectedCorpusSize &&
            corpusFingerprint->second == expectedCorpusHash &&
            recordsFingerprint->first == expectedRecordsSize &&
            recordsFingerprint->second == expectedRecordsHash;
    }

    /**
     * @brief Writes fingerprints that identify the validated daily files.
     */
    bool WriteDayValidationMarker(
        const std::filesystem::path& corpusPath,
        const std::filesystem::path& recordsPath)
    {
        const auto corpusFingerprint = CalculateFileFingerprint(corpusPath);
        const auto recordsFingerprint = CalculateFileFingerprint(recordsPath);

        if (corpusFingerprint.has_value() == false || recordsFingerprint.has_value() == false) {
            Logger::Error("Could not fingerprint committed daily files.", true);
            return false;
        }

        const std::filesystem::path validationPath = BuildValidationPath(corpusPath);
        std::filesystem::path temporaryPath = validationPath;
        temporaryPath += ".tmp";

        if (RemoveFileIfExists(temporaryPath) == false) {
            return false;
        }

        {
            std::ofstream validationFile(temporaryPath, std::ios::out | std::ios::trunc);
            if (validationFile.is_open() == false) {
                Logger::Error("Could not create the daily validation marker.", true);
                return false;
            }

            validationFile
                << "sonny-dukascopy-full-session-v1\n"
                << corpusFingerprint->first << ' ' << corpusFingerprint->second << '\n'
                << recordsFingerprint->first << ' ' << recordsFingerprint->second << '\n';
            validationFile.flush();
            if (validationFile.fail()) {
                Logger::Error("Could not write the daily validation marker.", true);
                return false;
            }

            validationFile.close();
            if (validationFile.fail()) {
                Logger::Error("Could not close the daily validation marker.", true);
                return false;
            }
        }

        std::error_code error;
        std::filesystem::remove(validationPath, error);
        if (error) {
            Logger::Error(std::format("Could not replace validation marker: {}", error.message()), true);
            return false;
        }

        std::filesystem::rename(temporaryPath, validationPath, error);
        if (error) {
            Logger::Error(std::format("Could not install validation marker: {}", error.message()), true);
            return false;
        }

        return true;
    }

    /**
     * @brief Replaces the daily corpus and record files using a recoverable commit.
     */
    bool CommitDayFiles(
        const std::filesystem::path& corpusTemporary,
        const std::filesystem::path& recordsTemporary,
        const std::filesystem::path& corpusPath,
        const std::filesystem::path& recordsPath)
    {
        const std::filesystem::path corpusBackup = BuildBackupPath(corpusPath);
        const std::filesystem::path recordsBackup = BuildBackupPath(recordsPath);
        const std::filesystem::path transactionPath = BuildTransactionPath(corpusPath);
        const std::filesystem::path commitPath = BuildCommitPath(corpusPath);

        std::error_code error;
        const bool corpusOriginallyExisted = std::filesystem::exists(corpusPath, error);
        if (error) {
            Logger::Error(std::format("Could not inspect {}: {}", corpusPath.string(), error.message()), true);
            return false;
        }

        const bool recordsOriginallyExisted = std::filesystem::exists(recordsPath, error);
        if (error) {
            Logger::Error(std::format("Could not inspect {}: {}", recordsPath.string(), error.message()), true);
            return false;
        }

        if (std::filesystem::exists(corpusBackup, error) ||
            std::filesystem::exists(recordsBackup, error) ||
            std::filesystem::exists(transactionPath, error) ||
            std::filesystem::exists(commitPath, error) || error) {
            Logger::Error("A daily export transaction artifact already exists; refusing to overwrite it.", true);
            return false;
        }

        {
            std::ofstream transactionFile(transactionPath, std::ios::out | std::ios::trunc);
            if (transactionFile.is_open() == false) {
                Logger::Error("Could not create daily export transaction marker.", true);
                return false;
            }

            transactionFile << corpusOriginallyExisted << ' ' << recordsOriginallyExisted << '\n';
            transactionFile.flush();
            if (transactionFile.fail()) {
                Logger::Error("Could not write daily export transaction marker.", true);
                return false;
            }
        }

        if (corpusOriginallyExisted) {
            std::filesystem::rename(corpusPath, corpusBackup, error);
            if (error) {
                Logger::Error(std::format("Could not back up {}: {}", corpusPath.string(), error.message()), true);
                RecoverInterruptedDayCommit(corpusPath, recordsPath);
                return false;
            }
        }

        if (recordsOriginallyExisted) {
            std::filesystem::rename(recordsPath, recordsBackup, error);
            if (error) {
                Logger::Error(std::format("Could not back up {}: {}", recordsPath.string(), error.message()), true);
                RecoverInterruptedDayCommit(corpusPath, recordsPath);
                return false;
            }
        }

        std::filesystem::rename(corpusTemporary, corpusPath, error);
        if (!error) {
            std::filesystem::rename(recordsTemporary, recordsPath, error);
        }

        if (error) {
            Logger::Error(std::format("Could not install validated daily files: {}", error.message()), true);
            RecoverInterruptedDayCommit(corpusPath, recordsPath);
            return false;
        }

        {
            std::ofstream commitFile(commitPath, std::ios::out | std::ios::trunc);
            if (commitFile.is_open() == false) {
                Logger::Error("Could not create daily export commit marker.", true);
                RecoverInterruptedDayCommit(corpusPath, recordsPath);
                return false;
            }

            commitFile << "committed\n";
            commitFile.flush();
            if (commitFile.fail()) {
                Logger::Error("Could not write daily export commit marker.", true);
                RecoverInterruptedDayCommit(corpusPath, recordsPath);
                return false;
            }
        }

        // The commit marker tells recovery that both new files are installed.
        // Remove backups first and the marker last so cleanup can resume safely.
        if (RemoveFileIfExists(corpusBackup) == false ||
            RemoveFileIfExists(recordsBackup) == false ||
            RemoveFileIfExists(transactionPath) == false) {
            return false;
        }

        return RemoveFileIfExists(commitPath);
    }

    /**
     * @brief Writes the daily OHLC values to a corpus file.
     */
    bool SaveDayCorpusToDisk(
        const std::vector<OHLCV_BidAsk>& candleSticks,
        const std::filesystem::path& outputPath)
    {
        if (candleSticks.size() != static_cast<std::size_t>(sessionLengthSeconds)) {
            Logger::Error("Refusing to write a corpus that does not contain a complete regular session.", true);
            return false;
        }

        std::error_code error;
        std::filesystem::create_directories(outputPath.parent_path(), error);
        if (error) {
            Logger::Error(std::format("Could not create corpus directory: {}", error.message()), true);
            return false;
        }

        std::ofstream corpusFileStream(outputPath, std::ios::out | std::ios::trunc);

        if (corpusFileStream.is_open() == false) {
            Logger::Error(std::format("Could not open corpus file: {}", outputPath.string()), true);
            return false;
        }

        // The multiplier is metadata for the entire file, so write it once.
        corpusFileStream
            << "multiplier="
            << candleSticks.front().Multiplier
            << '\n';

        for (const OHLCV_BidAsk& bidAsk : candleSticks) {
            corpusFileStream
                << bidAsk.OpenBid << ','
                << bidAsk.HighBid << ','
                << bidAsk.LowBid << ','
                << bidAsk.CloseBid << ','
                << bidAsk.OpenAsk << ','
                << bidAsk.HighAsk << ','
                << bidAsk.LowAsk << ','
                << bidAsk.CloseAsk
                << '\n';
        }

        corpusFileStream.flush();
        if (corpusFileStream.fail()) {
            Logger::Error(std::format("Failed writing corpus file: {}", outputPath.string()), true);
            return false;
        }

        corpusFileStream.close();
        if (corpusFileStream.fail()) {
            Logger::Error(std::format("Failed closing corpus file: {}", outputPath.string()), true);
            return false;
        }

        return true;
    }

    /**
     * @brief Writes the daily candle data as binary ML records.
     */
    bool SaveDayRecordsToDisk(
        const std::vector<OHLCV_BidAsk>& candleSticks,
        const std::filesystem::path& outputPath)
    {
        if (candleSticks.size() != static_cast<std::size_t>(sessionLengthSeconds)) {
            Logger::Error("Refusing to write records that do not contain a complete regular session.", true);
            return false;
        }

        std::error_code error;
        std::filesystem::create_directories(outputPath.parent_path(), error);
        if (error) {
            Logger::Error(std::format("Could not create records directory: {}", error.message()), true);
            return false;
        }

        std::ofstream recFileStream(
            outputPath,
            std::ios::binary | std::ios::out | std::ios::trunc
        );

        if (recFileStream.is_open() == false) {
            Logger::Error(std::format("Could not open record file: {}", outputPath.string()), true);
            return false;
        }

        static_assert(sizeof(Core::MLRecord) == ML_RECORD_EXPECTED_SIZE_BYTES);

        const float multiplier = candleSticks.front().Multiplier;

        const auto getMid =
            [multiplier](const OHLCV_BidAsk& candle) -> float {
                const float bid = static_cast<float>(candle.CloseBid) * multiplier;
                const float ask = static_cast<float>(candle.CloseAsk) * multiplier;
                return (bid + ask) * MIDPOINT_AVERAGE_FACTOR;
            };

        const auto getSpread =
            [multiplier](const OHLCV_BidAsk& candle) -> float {
                const float bid = static_cast<float>(candle.CloseBid) * multiplier;
                const float ask = static_cast<float>(candle.CloseAsk) * multiplier;
                return ask - bid;
            };

        float previousMid = getMid(candleSticks.front());
        float previousSpread = getSpread(candleSticks.front());

        for (std::size_t i = 0; i < candleSticks.size(); ++i) {
            constexpr float firstDeltaSentinel = 0.0f;

            const OHLCV_BidAsk& candle = candleSticks[i];
            const float mid = getMid(candle);
            const float spread = getSpread(candle);

            const Core::MLRecord record{
                .mid_delta = i == 0 ? firstDeltaSentinel : mid - previousMid,
                .spread_delta = i == 0 ? firstDeltaSentinel : spread - previousSpread,
                .volume = static_cast<float>(candle.BidVolume)
            };

            recFileStream.write(
                reinterpret_cast<const char*>(&record),
                sizeof(record)
            );

            if (!recFileStream) {
                Logger::Error(std::format("Failed writing ML record to {}", outputPath.string()), true);
                return false;
            }

            previousMid = mid;
            previousSpread = spread;
        }

        recFileStream.flush();
        if (recFileStream.fail()) {
            Logger::Error(std::format("Failed flushing record file: {}", outputPath.string()), true);
            return false;
        }

        recFileStream.close();
        if (recFileStream.fail()) {
            Logger::Error(std::format("Failed closing record file: {}", outputPath.string()), true);
            return false;
        }

        return true;
    }

    /**
     * @brief Executes a request and parses a successful response as JSON.
     */
    std::optional<Common::JsonUtility> FetchJson(
        Client::SimpleCurlWrapper& curlWrapper,
        Client::CurlRequest& request)
    {
        std::string body;
        request.SetCallback([&body](std::string response) {
            body = std::move(response);
        });
        const Client::CurlRequestEnum result = curlWrapper.ExecuteHttpRequest(request);
        const std::string& uri = request.GetFormattedUri();
        if (result != Client::CurlRequestEnum::SUCCESS) {
            Logger::Warning(std::format("Request failed: {} ({})", uri, request.GetErrorMessage()), true);
            return std::nullopt;
        }
        // The browser path does not report a status, so 0 is accepted.
        const long status = request.GetResponseCode();
        if (status != 0 && (status < HTTP_SUCCESS_STATUS_MIN || status >= HTTP_SUCCESS_STATUS_MAX_EXCLUSIVE)) {
            Logger::Warning(std::format("Request returned HTTP {}: {}", status, uri), true);
            return std::nullopt;
        }
        if (body.empty()) {
            return std::nullopt;
        }
        try {
            return Common::JsonUtility(body);
        }
        catch (const std::exception& exception) {
            Logger::Warning(
                std::format("Response could not be parsed as JSON: {} ({})", uri, exception.what()),
                true
            );
            return std::nullopt;
        }
    }

    /**
    * @brief Reads one hour of tick data from its JSON response.
    *
    * @param hourJson
    * @return The tick data, or nothing if any required field is missing or invalid.
    */
    std::optional<ApiTickResponse> ParseTickResponse(const Common::JsonUtility& hourJson) {
        ApiTickResponse res{};

        try {
            hourJson.TryGet<Common::UnixTimestamp>("timestamp", res.Timestamp);
            hourJson.TryGet<float>("multiplier", res.Multiplier);
            hourJson.TryGet<float>("bid", res.Bid);
            hourJson.TryGet<float>("ask", res.Ask);
            hourJson.TryGet<std::vector<std::int32_t>>("times", res.Times);
            hourJson.TryGet<std::vector<std::int16_t>>("asks", res.Asks);
            hourJson.TryGet<std::vector<std::int16_t>>("bids", res.Bids);
            hourJson.TryGet<std::vector<std::int32_t>>("askVolumes", res.AskVolumes);
            hourJson.TryGet<std::vector<std::int32_t>>("bidVolumes", res.BidVolumes);
        }
        catch (const std::exception& exception) {
            Logger::Warning(
                std::format("Hourly response could not be decoded: {}", exception.what()),
                true
            );
            return std::nullopt;
        }

        if (res.Timestamp <= 0 ||
            std::isfinite(res.Multiplier) == false || res.Multiplier <= 0.0f ||
            std::isfinite(res.Bid) == false || res.Bid <= 0.0f ||
            std::isfinite(res.Ask) == false || res.Ask < res.Bid) {
            Logger::Warning("Hourly response contains invalid timestamp, multiplier, bid, or ask values.", true);
            return std::nullopt;
        }

        const std::size_t tickCount = res.Times.size();
        if (tickCount == 0 ||
            res.Asks.size() != tickCount ||
            res.Bids.size() != tickCount ||
            res.AskVolumes.size() != tickCount ||
            res.BidVolumes.size() != tickCount) {
            Logger::Warning("Hourly response arrays are empty or have different lengths; rejecting the response.", true);
            return std::nullopt;
        }

        if (std::ranges::any_of(res.Times, [](const std::int32_t value) { return value < 0; }) ||
            std::ranges::any_of(res.AskVolumes, [](const std::int32_t value) { return value < 0; }) ||
            std::ranges::any_of(res.BidVolumes, [](const std::int32_t value) { return value < 0; })) {
            Logger::Warning("Hourly response contains negative time deltas or volumes.", true);
            return std::nullopt;
        }

        return res;
    }

    /**
     * @brief Fetches, validates, and writes one complete trading day.
     */
    bool ExportFullDay(const Common::DateTime& dt, Client::SimpleCurlWrapper& curlWrapper) {
        const std::filesystem::path corpusPath = Common::Storage::CORPUS_BASE_PATH / (dt.ToString_Date() + ".corpus");
        const std::filesystem::path recordsPath = Common::Storage::RECORDS_BASE_PATH / (dt.ToString_Date() + ".rec");

        const std::filesystem::path corpusTemporary = BuildTemporaryPath(corpusPath);
        const std::filesystem::path recordsTemporary = BuildTemporaryPath(recordsPath);
        const std::filesystem::path validationTemporary = BuildTemporaryPath(BuildValidationPath(corpusPath));

        if (RecoverInterruptedDayCommit(corpusPath, recordsPath) == false) {
            Logger::Error(std::format("Could not recover existing files for {}. Day was not modified.", dt.ToString_Date()), true);
            return false;
        }

        // New York time is used here so the session follows US daylight-saving changes.
        const Common::UnixTimestamp sessionOpen = Common::DateTime(
            dt.GetYear(), dt.GetMonth(), dt.GetDay(), SESSION_OPEN_HOUR, SESSION_OPEN_MINUTE, 0, true
        ).GetTimestamp();

        const Common::UnixTimestamp sessionClose = sessionOpen + sessionLengthSeconds;
        const std::int64_t sessionOpenMs = static_cast<std::int64_t>(sessionOpen) * MILLISECONDS_PER_SECOND;
        const std::int64_t sessionCloseMs = static_cast<std::int64_t>(sessionClose) * MILLISECONDS_PER_SECOND;

        // Dukascopy's endpoint is addressed by UTC hour. Derive the exact hours
        // intersecting this New York session instead of assuming a fixed DST offset.
        const std::int64_t firstUtcHour = static_cast<std::int64_t>(sessionOpen) / SECONDS_PER_HOUR;
        const std::int64_t lastUtcHour = (static_cast<std::int64_t>(sessionClose) - 1) / SECONDS_PER_HOUR;

        const std::string ticksUrl = BuildTicksUrl(dt);
        std::vector<HourlyTickResponse> tickResponses;
        tickResponses.reserve(static_cast<std::size_t>(lastUtcHour - firstUtcHour + 1));

        Client::CurlRequest hourRequest("");
        hourRequest.AddHeader(headers);

        for (std::int64_t utcHour = firstUtcHour; utcHour <= lastUtcHour; ++utcHour) {
            const int hourOfDay = static_cast<int>(utcHour % HOURS_PER_DAY);

            hourRequest.ReassignUri(ticksUrl + std::to_string(hourOfDay));

            const std::optional<Common::JsonUtility> hourJson = FetchJson(curlWrapper, hourRequest);
            if (hourJson.has_value() == false) {
                Logger::Warning(
                    std::format(
                        "Day rejected: {} (no usable response for UTC hour {}).",
                        dt.ToString_Date(),
                        hourOfDay
                    ),
                    true
                );
                return false;
            }

            std::optional<ApiTickResponse> response = ParseTickResponse(*hourJson);
            if (response.has_value() == false) {
                Logger::Warning(
                    std::format(
                        "Day rejected: {} (invalid data for UTC hour {}).",
                        dt.ToString_Date(),
                        hourOfDay
                    ),
                    true
                );
                return false;
            }

            tickResponses.push_back(HourlyTickResponse{
                .UtcHour = utcHour,
                .Data = std::move(*response)
            });
        }

        const std::optional<std::vector<Tick>> ticks = DecodeTicks(tickResponses, sessionOpenMs, sessionCloseMs);

        if (ticks.has_value() == false) {
            Logger::Warning(
                std::format(
                    "Day rejected: {} (tick data failed hourly coverage, timestamp, price, volume, or multiplier validation).",
                    dt.ToString_Date()
                ),
                true
            );
            return false;
        }

        const std::optional<std::vector<OHLCV_BidAsk>> candleSticks = ExtractOneSecondCandlesticks(*ticks, sessionOpen, sessionClose);

        if (candleSticks.has_value() == false ||
            candleSticks->size() != static_cast<std::size_t>(sessionLengthSeconds)) {
            Logger::Warning(
                std::format(
                    "Day rejected: {} (could not construct a complete, correctly anchored session).",
                    dt.ToString_Date()
                ),
                true
            );
            return false;
        }

        if (RemoveFileIfExists(corpusTemporary) == false ||
            RemoveFileIfExists(recordsTemporary) == false ||
            RemoveFileIfExists(validationTemporary) == false) {
            return false;
        }

        if (SaveDayCorpusToDisk(*candleSticks, corpusTemporary) == false ||
            SaveDayRecordsToDisk(*candleSticks, recordsTemporary) == false) {
            RemoveFileIfExists(corpusTemporary);
            RemoveFileIfExists(recordsTemporary);
            Logger::Warning(
                std::format("Day rejected: {} (could not write both staged output files).", dt.ToString_Date()),
                true
            );
            return false;
        }

        if (HasCompleteDayStructure(corpusTemporary, recordsTemporary) == false) {
            RemoveFileIfExists(corpusTemporary);
            RemoveFileIfExists(recordsTemporary);
            Logger::Warning(
                std::format("Day rejected: {} (staged files failed post-write integrity checks).", dt.ToString_Date()),
                true
            );
            return false;
        }

        if (CommitDayFiles(corpusTemporary, recordsTemporary, corpusPath, recordsPath) == false) {
            RemoveFileIfExists(corpusTemporary);
            RemoveFileIfExists(recordsTemporary);
            Logger::Error(
                std::format("Day export failed: {} (validated files could not be committed).", dt.ToString_Date()),
                true
            );
            return false;
        }

        if (WriteDayValidationMarker(corpusPath, recordsPath) == false) {
            Logger::Warning(
                std::format("Day files were installed, but their validation marker could not be written: {}", dt.ToString_Date()),
                true
            );
            return false;
        }

        Logger::Info(
            std::format(
                "Day processed: {} ({} candles; all required UTC hours validated)",
                dt.ToString_Date(),
                candleSticks->size()
            ),
            true
        );

        return true;
    }

    /**
     * @brief Runs the configured historical corpus export and updates progress.
     */
    void BeginCorpusExport(bool replaceExisting) {
        {
            std::string headerMessage = replaceExisting ?
            "   Beginning full corpus export" : "   Beginning missing records export";
            Logger::Info("========================================", true);
            Logger::Info(headerMessage, true);
            Logger::Info("========================================", true);
        }
        std::string status{};
        std::string currentDate{};
        std::size_t daysElapsed = 0;
        auto view = UI::Get().PushView([&] {
            const float progress =
                Core::Setup::TOTAL_DAYS == 0
                ? 0.0f
                : static_cast<float>(daysElapsed) /
                static_cast<float>(Core::Setup::TOTAL_DAYS);
            return ftxui::window(
                ftxui::text("Downloading Historical Records") | ftxui::bold,
                ftxui::vbox({
                    ftxui::text(status),
                    ftxui::separator(),
                    ftxui::hbox({
                        ftxui::text("Current date: "),
                        ftxui::text(currentDate) | ftxui::bold,
                    }),
                    ftxui::gauge(progress),
                    ftxui::text(
                        std::format(
                            "{} / {} days",
                            daysElapsed,
                            Core::Setup::TOTAL_DAYS
                        )
                    )
                }) |
                ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, MIN_PROGRESS_WINDOW_WIDTH)
            );
        });
        UI::Get().Refresh();
        // Wrapper running browser instance. Will be sharing this object throughout the full corpus run.
        Client::SimpleCurlWrapper curlWrapper;
        std::string description{};
        {
            Client::CurlRequest infoRequest(infoUrl);
            infoRequest.AddHeader(headers);
            const std::optional<Common::JsonUtility> spyData = FetchJson(curlWrapper, infoRequest);
            if (spyData) {
                spyData->TryGet<std::string>("description", description);
            }
            else {
                Logger::Error(
                    "Unable to access info URI. Terminating process.",
                    true
                );
            }
        }
        if (description.empty() == false) {
            Logger::Info(
                std::format(
                    "Description: {}",
                    description
                ), true
            );
            using namespace Common::Storage;
            Common::DateTime exportDate = Core::Setup::BEGIN_DATE;
            for (; exportDate <= Core::Setup::END_DATE; exportDate.NextDay()) {
                UI::Get().Update([&] {
                    currentDate = exportDate.ToString_Date();
                    status = std::format("Processing {}", currentDate);
                });

                const std::filesystem::path corpusPath = CORPUS_BASE_PATH / (exportDate.ToString_Date() + ".corpus");
                const std::filesystem::path recordsPath = RECORDS_BASE_PATH / (exportDate.ToString_Date() + ".rec");

                if (exportDate.IsWeekday() == false) {
                    Logger::Warning(
                        std::format("Weekend skipped: {}", exportDate.ToString_Date()),
                        true
                    );
                }
                else {
                    const bool filesRecovered = RecoverInterruptedDayCommit(corpusPath, recordsPath);
                    if (filesRecovered == false) {
                        Logger::Error(
                            std::format(
                                "Existing files could not be safely recovered: {}. Day skipped.",
                                exportDate.ToString_Date()
                            ),
                            true
                        );
                    }
                    else {
                        const bool existingFilesAreComplete = IsCompleteDayOnDisk(corpusPath, recordsPath);
                        if (replaceExisting || existingFilesAreComplete == false) {
                            ExportFullDay(exportDate, curlWrapper);
                        }
                        else {
                            Logger::Info(
                                std::format("Existing complete day skipped: {}", exportDate.ToString_Date()),
                                true
                            );
                        }
                    }
                }
                UI::Get().Update([&] { ++daysElapsed; });
            }
        }
        else {
            Logger::Error("Description empty. Terminating process.", true);
        }
    }
}
