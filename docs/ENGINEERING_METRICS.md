# Project Sonny Engineering Metrics

This report records the evidence behind the [README](../README.md) statistics. Collection date: **November 19, 2026**, using the local working tree and retained storage. Sonny has substantial historical storage and selective index loading; the available forecast evidence is a short sandbox experiment whose signed predictions do not beat a zero-move baseline.

## Scope and provenance

The inspected source is based on commit `d1012bb920559780d6267d30e690ab5f57a79238`, with pre-existing local changes to direction weights, index reuse, direction mode, and the FAISS checkout. This report describes that working configuration, not the unchanged commit. No prediction/runtime code was modified for measurement. The later test-only corrections align stale expectations with those existing changes.

Evidence consists of file lengths, FAISS headers, a complete scan of location metadata, current call paths, and `storage/logs/2026-11-19.log`. The log is **531,757 bytes**, SHA-256:

```text
67e1d6603f263802d572af243773ab53212875da2d287e7096cac5dd24e9c179
```

Storage and logs are excluded from the public checkout, so a fresh clone cannot independently reproduce the measured corpus or experiment without those retained artifacts. The collector below makes the arithmetic repeatable when they are available. The older Word architecture/research documents were inspected as historical background; their proposals, estimates, and third-party benchmark references are not measurements of this implementation. The supplied screenshots show download, index update, and sandbox UI states, but do not provide timed benchmark intervals or a complete quote counter series.

The log contains one run: **09:37:26–09:47:17**, 591 seconds between logged endpoints. Forecast anchors span **09:37:47–09:47:10**, with evaluation messages through 09:47:16. These are wall-clock strings as logged, without UTC offsets or full source timestamps. The collection context is America/Phoenix; the file itself does not attest its timezone. No several-hour log is available in the inspected storage.

## Reproduce the measurements

From the project root, with the original storage retained:

```bash
python3 docs/measure_engineering_metrics.py > /tmp/sonny-engineering-metrics.json
# Alternative location for a retained snapshot:
python3 docs/measure_engineering_metrics.py \
    --storage /path/to/storage \
    --log /path/to/storage/logs/2026-11-19.log
```

The [standard-library collector](measure_engineering_metrics.py) is read-only. It stats artifacts, reads 45-byte index headers, scans the 12-byte metadata locations, and parses the log. It does not load the 7.92 GiB vector collection, contact market-data services, write historical files, or start forecasts. It assumes the observed little-endian 64-bit FAISS serialization and four-byte records, verifies the `IxF2` tag and payload lengths, and rejects unexpected layouts or multiple runs. It is an evidence collector for this log format, not a general benchmark harness. Scanning all metadata reads about 1.19 GiB.

Additional inspection commands:

```bash
git rev-parse HEAD
git diff -- src/core/inputconfig.hpp src/core/weekdayconfig.hpp \
    src/core/setup.hpp src/core/halfhourmultiindex.cpp
du -B1 -s storage/corpus storage/records storage/indexes storage/metadata
sha256sum storage/logs/2026-11-19.log
rg '^MemTotal:' /proc/meminfo
clang++ --version
cmake --version
protobuf/install/bin/protoc --version
git -C faiss rev-parse HEAD
git -C protobuf rev-parse HEAD
```

`du` reports filesystem allocation (and may include directory blocks), whereas headline figures sum regular-file `st_size`. Do not substitute one for the other. All GiB/MiB conversions below use powers of 1,024. No total storage number is presented as RAM consumption.

## Historical scale and storage

The artifact snapshot contains **2,272 `.rec` files**, from **2017-02-17 through 2026-11-10**. Every file is **93,600 bytes**, which is **23,400 × 4 bytes**. Thus the measured stored-record count is **53,164,800**. These are one-second slots, including first-record zero sentinels and carried-forward quiet seconds, not 53 million distinct trades or source ticks. “2,272 sessions” counts stored date files; it does not establish complete exchange-calendar coverage.

| Artifact category | Files | File bytes | Display size | Allocated file bytes¹ |
|---|---:|---:|---:|---:|
| Derived bid/ask OHLC `.corpus` | 2,272 | 2,977,267,424 | 2.772796 GiB | 2,977,955,840 |
| Daily `.validated` markers | 2,272 | 197,208 | 0.188072 MiB | 9,306,112 |
| Signed, unnormalized `.rec` | 2,272 | 212,659,200 | 202.807617 MiB | 214,040,576 |
| FAISS `.index` | 26 | 8,499,462,290 | 7.915741 GiB | 8,499,560,448 |
| Location `.meta` | 26 | 1,274,919,168 | 1.187361 GiB | 1,274,994,688 |
| Normalization JSON | 13 | 2,733 | 2.669 KiB | 53,248 |

¹Allocated file bytes = sum of `st_blocks × 512`; excludes directory entries. Values are specific to this filesystem snapshot. Logs, source/dependencies, build products, and test fixtures are outside these categories.

**Representation evidence.** [MLRecord](../src/core/structures.hpp) contains one `float mid_delta`; [SaveDayRecordsToDisk](../src/dukascopy/dukascopy.cpp) asserts its size is four bytes and writes the native structure directly. Its first value is zero; later values subtract adjacent closing midpoints. The text corpus stores a multiplier and eight integer OHLC columns. Neither archive is a retained original tick feed. There is no separate normalized `.rec` dataset: normalization is applied to absolute deltas for FAISS construction and queries.

**Integrity scope.** The exporter validates session anchoring and tick arrays, uses a five-minute maximum-gap heuristic, creates daily fingerprints, and commits files through recoverable temporary/backup/transaction steps. The measurement checks file lengths, index/metadata counts, referenced-day existence, date ranges, and complete input-window offsets. It does not re-download source ticks, revalidate every candle or fingerprint, or prove every source tick is present. Holidays and early closes are not explicitly modeled; missing dates are not classified as holidays versus unavailable/rejected sessions.

## Index distribution and construction

Only the **20-second** input-window directory is present. It contains 13 magnitude and 13 all-week direction indexes; no weekday-group index artifacts or other input-window collections are present.

| New York interval(s) | Number of intervals | Vectors per index | Bytes per index | Bytes per location file |
|---|---:|---:|---:|---:|
| 09:30–10:00 through 15:00–15:30 | 12 | 4,089,600 | 327,168,045 | 49,075,200 |
| 15:30–16:00 | 1 | 4,046,432 | 323,714,605 | 48,557,184 |

Every interval has both a magnitude and a direction index. The twelve non-final intervals each contain `2,272 × 1,800` windows. The last contains `2,272 × (1,800 − 20 + 1)`. Each family therefore represents **53,121,632 windows**; the total **106,243,264 stored vectors** represents two feature views of those windows, not twice as much historical coverage.

The collector scanned every location: all 26 files reference exactly 2,272 dates, with extrema **20170217–20261110**, 1,800 locations per date in non-final intervals and 1,781 in the final interval. All referenced `.rec` days exist and every window fits within its session. FAISS headers report dimension **20** and `IndexFlatL2`; each file length equals `45 + vector_count × 20 × 4`. This header interpretation follows `write_index_header` and the flat-index branch in the local `faiss/faiss/impl/index_write.cpp` checkout.

The [builder](../src/core/setup.cpp) reads days in two passes per segment: Welford statistics over absolute deltas, then normalized magnitude/sign-vector construction. Non-final segments include 19 padding records, producing 1,800 window starts. Normalization sample counts are **4,132,768** (`2,272 × 1,819`) in non-final segments and **4,089,600** in the last. Padding overlaps the next segment's statistics; these are not additional unique corpus records. The stored normalization schema is version 3, with one `mid_delta` feature and population standard deviation.

Magnitude and direction vectors are float32 arrays even though direction coordinates encode only signed, weighted positions. Locations are **12 bytes per vector** (`uint32_t recordIndex` + 8 date characters), asserted in [HalfHourIndex](../src/core/halfhourindex.hpp). Location payload is **15% of vector payload** at dimension 20. Normalization JSON and daily validation markers are accounted for separately.

The current direction weights increase linearly from 0.4 to 2.2; coordinates are scaled by the square root because L2 squares coordinate differences. The collector reads the first vector in each stored direction index and checks agreement with those weights at nonzero coordinates; this is a spot-check, not a complete reconstruction audit. The remaining vector payload is not read. No measured benefit over the previous weights is claimed.

Index build duration, build throughput, and peak construction memory are **unavailable**. Screenshots capture progress, not a timed complete build. No index was rebuilt for this report.

## Memory and residency

[SessionMultiIndex::SelectSegments/LoadSegments](../src/core/sessionmultiindex.cpp) selects a contiguous run around the input window's start interval, clamps it to the 13-interval session, retains common segments, and releases outgoing segments before reading replacements. Missing or unreadable segments are marked unavailable and skipped. The search runs over the loaded set, not over disk-resident vectors in the remaining intervals.

| Configuration or observation | Value | Evidence and limit |
|---|---|---|
| Current input selection | One 20-second input | `USE_ALL_INPUT_WINDOWS=false`, `SELECTED_INPUT_WINDOW_SECONDS=20` in [inputconfig.hpp](../src/core/inputconfig.hpp) |
| Current segment cap | 3 of 13 per active window | `MAX_LOADED_SEGMENTS=3` |
| Observed loaded collection | Six indexes: magnitude + AllDays direction for 11:00–11:30, 11:30–12:00, 12:00–12:30 | Six load messages, 09:37:26–09:37:29; selection summary at 09:37:29 |
| Loaded vector/location payload calculation | **2,257,459,200 bytes = 2.102423 GiB** | `6 × 4,089,600 × (20 × 4 + 12)`; excludes headers and overhead |
| Entire collection's vector/location payload calculation | **9,774,380,288 bytes = 9.103101 GiB** | All vectors and locations; hypothetical resident payload, not measured allocation |
| Historical cache | At most 4,096 whole-day vectors, cleared when cap is reached | [HistoricalRecordStore](../src/runtime/historicalrecordstore.cpp); current 2,272-day payload would total 202.81 MiB if all cached |
| Developer hardware context | Approximately 16 GB system RAM | User-provided constraint; `/proc/meminfo` at collection reported 15,517,524 KiB total (14.798664 GiB usable by the OS) |
| Process RSS / peak RSS / allocator capacity | **Unavailable** | No retained memory measurement |

Selective loading is confirmed by code and the log. It limits logical index residency; it does not mean total process RAM equals the payload calculation or that the whole collection exceeds this laptop's physical RAM. FAISS vector storage is moved from the loaded flat index rather than copied into another full vector buffer. Metadata `std::vector` capacity, temporary loading/search buffers, the day cache, UI, logger, Paho threads, libraries, and the OS page cache all complicate RAM accounting. The 4,096-day cache cap corresponds to 383,385,600 bytes (365.625 MiB) of four-byte full-session payload before container overhead.

All-window mode supports 10/15/20/30/45/60 seconds and defaults to one segment per window; these configurations were not run in the measured experiment. Weekday-only mode restricts direction coverage; Compare mode adds a separate group-specific direction index per resident segment. Larger windows duplicate more float coordinates per historical window. More resident intervals increase time-of-day coverage, memory, and exact-search work. These are architectural tradeoffs, not measured speedups.

## Runtime performance

| Metric | Available observation | What it establishes |
|---|---|---|
| Index preload interval | 09:37:26 first load → 09:37:29 selection summary | Approximately 3 seconds between coarse asynchronous log stamps for six indexes and metadata; cache state unknown; not a precise load benchmark |
| Forecast cadence | 111 anchors; 110 gaps; mean **5.118182 s**, median **5 s**, range **5–6 s** | Source-anchor cadence in this run; configured wait is 5 seconds **after** computation |
| FAISS query latency / throughput | **Unavailable** | No start/end timing around `_index.search` |
| Forecast computation time / percentiles | **Unavailable** | Log lines and source-anchor differences do not isolate computation |
| Quote ingestion rate | **Unavailable** | UI screenshot counter has no known start/duration; no retained counter series |
| Forecast evaluator throughput | **Unavailable** as an isolated benchmark | 333 reports were emitted, but this does not measure evaluator service capacity |
| Index construction time / throughput | **Unavailable** | No retained full build interval |
| Memory consumption | **Unavailable** for Sonny process | File lengths and payload calculations are not RSS |

No latency average, median, p95, or p99 can be supported by the log. Cadence statistics are not latency statistics. Logger timestamps are assigned in the asynchronous logger observer and have one-second resolution, so subtracting adjacent log lines would mix queueing, I/O, scheduling, and computation. No expensive benchmark, network run, full dependency rebuild, or index rebuild was introduced.

## Forecast evaluation

### Configuration and sample

The log labels identify **20-second input / AllDays / 8-, 16-, and 32-match caps**, six indexes in three intervals, and sandbox endpoints. Current source requests 128 candidates per sub-index/segment, retains up to 32 usable historical futures, and evaluates 1/2/3/4/5 seconds. All 111 evaluations per cap report a complete five-second window with observed 5/5, yielding **333 reports and 1,665 horizon rows**. These are **111 shared forecast anchors**, not 333 independent market samples; horizons and configurations reuse observations. No horizons are missing or marked `INCOMPLETE` in this run.

The current [forecast implementation](../src/runtime/forecast.cpp) uses separately weighted magnitude matches and direction votes. Magnitude uses `1/(squared_distance + 1e-6)^2`, direction uses `1/(1 + squared_distance)`, and the cumulative-direction flat threshold is $0.0005. It outputs full magnitude times the winning vote's sign, including zero for a tie. The log records predicted values, expected magnitudes, effective matches, distance lists, segment origins, observed outcomes, signed errors, zero-baseline errors, and direction hits. It does not record a source/binary identifier, all weight parameters, or an index/data manifest; tying every runtime detail to the inspected source remains uncertain.

### Does this establish unseen live-market evaluation?

[Orchestrator](../src/runtime/runtime.cpp) observes each completed second before appending it to the next input window. [PredictionExperiment](../src/runtime/predictionexperiment.cpp) searches a snapshot and reads historical future from `recordIndex + inputWindowSeconds`; a match without five remaining historical seconds is discarded. [PredictionAnalytics](../src/runtime/predictionanalytics.cpp) scores only timestamps strictly after the snapshot's anchor, retaining observations for 180 seconds to handle forecasts submitted after some outcomes arrive.

The scanned index dates end **November 10**, before the **November 19** log date. No online index insertion occurs in this runtime path. This supports historical separation from the logged stream run and outcomes held out from each input snapshot. However, the log omits full quote dates, original payloads, and source provenance. **It cannot independently establish genuinely unseen production-market data, exclude sandbox replay, or prove the forecast was available before every scored outcome.** This report therefore labels the results as subsequent-observation **sandbox evaluation**, not a verified production out-of-sample trial.

### Metric definitions

For each horizon and configuration, `y` is the evaluator's cumulative observed midpoint move, `p` the signed forecast, and `m` the separately logged expected magnitude. Units are **USD per SPY share**.

- Signed forecast MAE = `mean(abs(p − y))`, using the logged six-decimal absolute-error column.
- Magnitude MAE = `mean(abs(m − abs(y)))`. The collector joins the five-decimal magnitude diagnostic to the corresponding paired forecast anchor and six-decimal evaluation outcome. It does not treat signed forecast MAE as magnitude MAE or assume `m = abs(p)` when the direction vote ties.
- Zero-move baseline MAE = `mean(abs(y))`. This baseline applies to both a signed zero prediction and a zero-magnitude prediction.
- Relative error reduction = `100 × (baseline MAE − model MAE) / baseline MAE`. Negative values mean worse error than the baseline.
- Direction accuracy = logged `HIT` count / completed rows. Signs use a ±$0.0005 flat band, matching the evaluator; flat is a third class, not arbitrarily assigned up/down.
- Direction baseline = accuracy of the most frequent observed up/down/flat class at that horizon. This is a **retrospective sample-majority comparator**, not a trained baseline or an implementable strategy chosen in advance. Zero/flat accuracy is also shown. A 50% coin-flip comparison alone would ignore flats and class imbalance.

No historical-mean-magnitude, prior-move persistence, randomization, or transaction-cost baseline was logged. Zero magnitude is a weak baseline and its error reduction alone does not establish useful magnitude prediction. Magnitude diagnostics have rounding uncertainty of roughly $0.000005 per value; evaluation numbers have six-decimal rounding. Percentages below are descriptive, with no confidence/significance claim. No incompatible caps, windows, modes, or runs are averaged together.

### Magnitude errors

Each row has **111 completed samples**. “Reduction” compares the magnitude MAE with the zero-magnitude baseline.

| Match cap | Horizon | Magnitude MAE ($) | Zero baseline MAE ($) | Reduction |
|---:|---:|---:|---:|---:|
| 8 | 1s | 0.030305 | 0.044550 | +31.97% |
| 8 | 2s | 0.040677 | 0.063694 | +36.14% |
| 8 | 3s | 0.041715 | 0.073468 | +43.22% |
| 8 | 4s | 0.053136 | 0.084279 | +36.95% |
| 8 | 5s | 0.065881 | 0.099955 | +34.09% |
| 16 | 1s | 0.029265 | 0.044550 | +34.31% |
| 16 | 2s | 0.041078 | 0.063694 | +35.51% |
| 16 | 3s | 0.041923 | 0.073468 | +42.94% |
| 16 | 4s | 0.052765 | 0.084279 | +37.39% |
| 16 | 5s | 0.065825 | 0.099955 | +34.15% |
| 32 | 1s | 0.028820 | 0.044550 | +35.31% |
| 32 | 2s | 0.040588 | 0.063694 | +36.28% |
| 32 | 3s | 0.041821 | 0.073468 | +43.08% |
| 32 | 4s | 0.052164 | 0.084279 | +38.11% |
| 32 | 5s | 0.063459 | 0.099955 | +36.51% |

### Signed forecast errors

Each row has **111 completed samples**. These errors include wrong-direction forecasts. Every row is worse than the zero-move baseline.

| Match cap | Horizon | Signed MAE ($) | Zero baseline MAE ($) | Reduction |
|---:|---:|---:|---:|---:|
| 8 | 1s | 0.055323 | 0.044550 | -24.18% |
| 8 | 2s | 0.073580 | 0.063694 | -15.52% |
| 8 | 3s | 0.086187 | 0.073468 | -17.31% |
| 8 | 4s | 0.096990 | 0.084279 | -15.08% |
| 8 | 5s | 0.119264 | 0.099955 | -19.32% |
| 16 | 1s | 0.054480 | 0.044550 | -22.29% |
| 16 | 2s | 0.073000 | 0.063694 | -14.61% |
| 16 | 3s | 0.086858 | 0.073468 | -18.22% |
| 16 | 4s | 0.099171 | 0.084279 | -17.67% |
| 16 | 5s | 0.108449 | 0.099955 | -8.50% |
| 32 | 1s | 0.055594 | 0.044550 | -24.79% |
| 32 | 2s | 0.075402 | 0.063694 | -18.38% |
| 32 | 3s | 0.082254 | 0.073468 | -11.96% |
| 32 | 4s | 0.093763 | 0.084279 | -11.25% |
| 32 | 5s | 0.111824 | 0.099955 | -11.87% |

### Directional accuracy and baselines

All configurations share the same actual outcomes at each anchor. Up/down/flat counts sum to 111 at each horizon. Accuracy includes flats, using the same rules for model and baseline.

| Horizon | Up / down / flat | 8-match hits | 16-match hits | 32-match hits | Majority baseline | Zero/flat baseline |
|---:|---:|---:|---:|---:|---:|---:|
| 1s | 52 / 53 / 6 | 52/111 (46.85%) | 51/111 (45.95%) | 48/111 (43.24%) | 53/111 (47.75%) | 6/111 (5.41%) |
| 2s | 56 / 52 / 3 | 57/111 (51.35%) | 58/111 (52.25%) | 57/111 (51.35%) | 56/111 (50.45%) | 3/111 (2.70%) |
| 3s | 53 / 55 / 3 | 55/111 (49.55%) | 56/111 (50.45%) | 61/111 (54.95%) | 55/111 (49.55%) | 3/111 (2.70%) |
| 4s | 52 / 56 / 3 | 63/111 (56.76%) | 62/111 (55.86%) | 62/111 (55.86%) | 56/111 (50.45%) | 3/111 (2.70%) |
| 5s | 52 / 56 / 3 | 61/111 (54.95%) | 66/111 (59.46%) | 64/111 (57.66%) | 56/111 (50.45%) | 3/111 (2.70%) |

### Interpretation and limitations

The five-second magnitude MAE ranges from $0.063459 to $0.065881 versus $0.099955 for zero magnitude. Signed MAE ranges from $0.108449 to $0.119264 and is worse than zero-move MAE. The 16-match cap's five-second direction accuracy is 66/111 (59.46%) versus the retrospective majority comparator's 56/111 (50.45%). This isolated observation is not a validated best configuration.

The run covers less than ten minutes, uses sandbox data, compares several related settings, and has overlapping input windows and temporally related outcomes. Full provenance, an untouched validation period, more realistic baselines, uncertainty analysis accounting for dependence, and multiple market regimes are absent. There is no order execution, spread/slippage/fees evaluation, or profitability measurement. No causal throughput or forecasting improvement from an architecture or parameter change is established.

## Architectural claim evidence

| Claim | Current call path or implementation |
|---|---|
| UI starts work off its rendering thread | [main](../src/sonny.cpp) → [UI::DispatchOperation](../src/ui/ui.hpp) → setup/runtime callback |
| Historical direct-HTTP path | [BeginCorpusExport → ExportFullDay → FetchJson](../src/dukascopy/dukascopy.cpp) → default `SimpleCurlWrapper(false)` → libcurl; optional browser backend exists but is not selected here |
| Validated session storage | Exporter decodes UTC-hour responses, anchors New York regular-session seconds, validates and commits `.corpus`/`.rec` pairs |
| Segmented two-pass construction | [ExecuteUpdateIndexes](../src/core/setup.cpp) → [NormalizationAccumulator](../src/core/transformer.cpp) → [HalfHourMultiIndex::AddVectorsToIndex](../src/core/halfhourmultiindex.cpp) |
| Exact resident search | [HalfHourIndex::Query](../src/core/halfhourindex.cpp) → `faiss::IndexFlatL2::search`; [SessionMultiIndex](../src/core/sessionmultiindex.cpp) rescales magnitude distances and merges/deduplicates candidates |
| Quote decode and aggregation | [WebullStreamingClient::RunSession](../src/client/webullstream.cpp) → [Paho callback bridge](../src/client/mqttwrapper.cpp) → orchestrator callback → [OneSecondAggregator](../src/runtime/onesecondaggregator.cpp) |
| Snapshot before expensive work | [LiveInputWindow::TakeSnapshotIfUpdated](../src/runtime/liveinputwindow.cpp) copies under `mutex_`; returned snapshot is searched by the prediction operation after lock release |
| Independent scoring synchronization | `PredictionAnalytics::SubmitForecast/ObserveRecord/FlushIncomplete` each lock evaluator state |
| Shutdown and lifetime | Runtime's `StreamStopGuard` stops/joins client worker before local pipeline destruction; explicit stop precedes final evaluation flush |
| Credentials and transport | [SecretStore](../src/common/secretstore.cpp) reads environment-scoped keyring entries; Webull signs HTTPS controls and [MqttWrapper](../src/client/mqttwrapper.cpp) enables TLS certificate verification |
| Logging and UI | [Logger](../src/common/logger.cpp) queues messages for an observer worker; UI uses a separate view-state mutex |

Two primary pipeline paths do not imply only two OS threads: UI, logging, stream management, and Paho library callbacks also run. Callback decoding, aggregation, evaluation logging, and view updates can still consume ingestion-path time even though FAISS work is outside the rolling-window mutex. No contention benchmark has been performed.

## Verification

On November 19, the initially available test binary passed 175/178 cases. At the user's request, test expectations were updated for existing direction weights **0.4→2.2** (mean weight 1.3) and the current **AllDays** mode. After rebuilding the test target:

```bash
cmake --build build --target sonny_common_tests
ctest --test-dir build --output-on-failure
```

**178/178 tests passed**, zero failures; reported test-suite wall time **4.52 seconds**. This is a verification result, not a runtime performance benchmark. The two changed test files are `tests/core/halfhourindex_tests.cpp` and `tests/core/weekdaygroups_tests.cpp`. No application behavior was changed. Existing tests cover runtime stages and forecast construction; this suite is not a statistical validation or a complete audit of evaluator correctness.

Build instructions were compared with top-level CMake, the helper scripts, dependency checkouts, installed `protoc`, and existing compile commands. Top-level minimum CMake is **3.23**, Sonny uses **C++26**, forces **Debug / `-g -O0`**, and fetches FTXUI 7.0.3, GoogleTest 1.18.0, and Paho MQTT C++ 1.6.0. The existing compile commands agree. Local Clang reports **23.1.5**; CMake reports **4.3.0**. FAISS is separately built with Release flags by the documented command.

The local FAISS checkout is `14392c952788ca1d6980361cf01d1c8736afc4db`; local Protobuf is `c0d0827c8d52ab52afdf4ee477b58e78b4c903ef`, with installed `libprotoc 38.0-dev`. These observations do not prove the previously installed FAISS library was built from that exact current checkout. The original README's 36.2/pinned-release claim was inconsistent with its HEAD clone command and local install. Instructions now state that dependency clones are unpinned and compiler/runtime versions must match. Dependency install/build/run commands are preserved; Ubuntu OpenSSL/pkg-config dependencies and standard-library/time-zone requirements are explicit. A clean Fedora/Ubuntu/WSL installation was not tested.

Markdown relative links/fragments, table column counts, balanced code fences, and shell-block syntax were checked locally. The diagram's 16 unique nodes and 18 edge declarations passed a structural check and follow the inspected pipeline; no Mermaid renderer was available for visual validation. A second collector run reproduced the storage and experiment measurements, and an independent evaluation-row parser matched all 15 signed MAEs and sample counts. No generated historical record or index was changed by this documentation task. Test fixtures use temporary working directories.

## Unavailable measurements and next instrumentation

These are proposed additions for future experiments, not implemented work in this task.

| Missing measurement or uncertainty | Instrumentation needed |
|---|---|
| Per-query latency and query throughput | `steady_clock` around each FAISS search and merged search; window/segment/type labels; query counts; retained samples or histograms for mean/median/p95/p99; warm/cold-cache and thread settings |
| Forecast duration and timely availability | Monotonic timestamps for snapshot, search, cache lookup, construction, submission, and publication; retain full source anchor timestamps and decision lead time to each outcome |
| Quote and record rates / data quality | Interval counters for received/decoded/rejected/late quotes, observed/carried-forward seconds, resets and reconnects; monotonic elapsed time |
| Build/load timings and memory peaks | Per-segment start/end timings, vector counts and cold/warm-cache labels; process RSS/high-water mark and hardware/OS/compiler/FAISS/OpenMP configuration |
| Evaluator service capacity | Isolated bounded replay benchmark with known observations/forecasts, report count, elapsed time, and contention profile |
| Exact experiment/index provenance | Source and binary IDs, active settings including weight formulas, input data/date manifest, index/metadata checksums, build flags, timezone, symbol/environment, and full source quote timestamps |
| Verified unseen production outcomes | Retained dated production payloads and receipt times, frozen historical manifest, explicit replay/delay detection, untouched evaluation interval, and availability timing |
| More informative forecast baselines | Prior-move persistence, fixed historical magnitude and majority direction learned before evaluation, comparable sampling, longer retained periods, and dependence-aware uncertainty |
| Source-data completeness | Exchange calendar with early closes, rejected/missing-day accounting, and provider/schema/sequence validation where available |

The strongest resume-ready evidence is the end-to-end C++ implementation, four-byte records at 53.16-million-record scale, a measured 26-index collection, selective residency, and synchronized ingestion/prediction paths. Predictive accuracy and runtime performance should retain the qualifications above.
