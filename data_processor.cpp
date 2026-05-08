#include "data_processor.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <omp.h>
#include <unordered_map>
#include <unordered_set>

// Parsing
bool DataProcessor::parseStations(std::vector<Station>& stations, const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    file.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::string buf(size, '\0');
    file.read(&buf[0], size);

    char* p = &buf[0];
    char* end = p + size;

    while (p < end && *p != '\n') p++;
    if (p < end) p++;

    std::vector<char*> lines;
    lines.reserve(64);
    while (p < end) {
        if (*p != '\r' && *p != '\n') lines.push_back(p);
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }

    stations.resize(lines.size());
    for (size_t i = 0; i < lines.size(); i++) {
        Station& s = stations[i];
        char* next;
        p = lines[i];
        s.station_id  = static_cast<uint32_t>(std::strtoul(p, &next, 10)); p = next + 1;
        char* name_start = p;
        while (*p != ';') p++;
        s.name.assign(name_start, p); p++;
        s.latitude    = std::strtof(p, &next); p = next + 1;
        s.longtitude  = std::strtof(p, nullptr);
    }
    return true;
}

bool DataProcessor::parseMeasurements(std::vector<Measurement>& measurements, const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    file.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::string buf(size, '\0');
    file.read(&buf[0], size);

    char* p = &buf[0];
    char* end = p + size;

    while (p < end && *p != '\n') p++;
    if (p < end) p++;

    // collect line starts — separating discovery from parsing makes the parse
    std::vector<char*> lines;
    lines.reserve(size / 18);
    while (p < end) {
        if (*p != '\r' && *p != '\n') lines.push_back(p);
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }

    measurements.resize(lines.size());

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < (int)lines.size(); i++) {
        Measurement& m = measurements[i];
        char* next;
        char* lp = lines[i];
        m.station_id = static_cast<uint32_t>(std::strtoul(lp,  &next, 10)); lp = next + 1;
        m.ordinal    = static_cast<uint64_t>(std::strtoull(lp, &next, 10)); lp = next + 1;
        m.year       = static_cast<uint16_t>(std::strtoul(lp,  &next, 10)); lp = next + 1;
        m.month      = static_cast<uint8_t> (std::strtoul(lp,  &next, 10)); lp = next + 1;
        m.day        = static_cast<uint8_t> (std::strtoul(lp,  &next, 10)); lp = next + 1;
        m.value      = std::strtof(lp, nullptr);
    }
    return true;
}

bool DataProcessor::parseData(const std::string& stations_path, const std::string& measurements_path) {
    if (!parseStations(stations, stations_path)) return false;
    if (!parseMeasurements(measurements, measurements_path)) return false;
    return true;
}

// Filtering
void DataProcessor::filterStations() {
    struct StationStats {
        std::vector<uint16_t> years;
        uint32_t count = 0;
    };

    // Serial aggregation — random writes per station_id, not worth parallelising
    std::unordered_map<uint32_t, StationStats> stats;
    stats.reserve(stations.size() * 2);
    for (const auto& m : measurements) {
        auto& s = stats[m.station_id];
        s.years.push_back(m.year);
        s.count++;
    }

    // Deduplicate + sort years per station (can be done in parallel)
    std::vector<uint32_t> station_ids;
    station_ids.reserve(stats.size());
    for (auto& [sid, _] : stats) station_ids.push_back(sid);

    #pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < (int)station_ids.size(); i++) {
        auto& yrs = stats[station_ids[i]].years;
        std::sort(yrs.begin(), yrs.end());
        yrs.erase(std::unique(yrs.begin(), yrs.end()), yrs.end());
    }

    // Per-station validity check — embarrassingly parallel
    std::vector<bool> valid(stations.size());
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < (int)stations.size(); i++) {
        auto it = stats.find(stations[i].station_id);
        if (it == stats.end()) { valid[i] = false; continue; }
        const auto& s = it->second;

        if (float(s.count) / float(s.years.size()) < 100.0f) { valid[i] = false; continue; }

        // Check for 5 consecutive years
        int max_run = 1, run = 1;
        for (size_t j = 1; j < s.years.size(); j++) {
            run = (s.years[j] == s.years[j - 1] + 1) ? run + 1 : 1;
            if (run > max_run) max_run = run;
        }
        valid[i] = (max_run >= 5);
    }

    // Compact stations vector
    size_t j = 0;
    for (size_t i = 0; i < stations.size(); i++)
        if (valid[i]) stations[j++] = std::move(stations[i]);
    stations.resize(j);

    // Build valid-id set for measurement filtering
    std::unordered_set<uint32_t> valid_ids;
    valid_ids.reserve(stations.size());
    for (const auto& s : stations) valid_ids.insert(s.station_id);

    measurements.erase(
        std::remove_if(measurements.begin(), measurements.end(),
            [&](const Measurement& m) { return !valid_ids.count(m.station_id); }),
        measurements.end());
}

// uint64_t-encoded hash keys — bit shifting hidden inside helper functions
static uint64_t smKey(uint32_t station_id, uint8_t month) {
    return ((uint64_t)station_id << 8) | month;
}
static uint64_t smyKey(uint32_t station_id, uint8_t month, uint16_t year) {
    return ((uint64_t)station_id << 32) | ((uint64_t)month << 16) | year;
}
static uint32_t smKeyStation(uint64_t key) { return uint32_t(key >> 8); }
static uint8_t  smKeyMonth  (uint64_t key) { return uint8_t(key & 0xFF); }
static uint32_t smyKeyStation(uint64_t key) { return uint32_t(key >> 32); }
static uint8_t  smyKeyMonth  (uint64_t key) { return uint8_t((key >> 16) & 0xFF); }
static uint16_t smyKeyYear   (uint64_t key) { return uint16_t(key & 0xFFFF); }

using SumCount = std::pair<double, uint32_t>;

// Monthly averages (for SVG coloring)
std::vector<MonthlyAvg> DataProcessor::computeMonthlyAverages() const {
    std::unordered_map<uint64_t, SumCount> global_sums;

    // Thread-local accumulation, then critical merge
    #pragma omp parallel
    {
        std::unordered_map<uint64_t, SumCount> local;

        #pragma omp for nowait schedule(static)
        for (int i = 0; i < (int)measurements.size(); i++) {
            const auto& m = measurements[i];
            auto& [sum, count] = local[smKey(m.station_id, m.month)];
            sum += m.value;
            count++;
        }

        #pragma omp critical
        for (auto& [key, val] : local) {
            auto& [sum, count] = global_sums[key];
            sum   += val.first;
            count += val.second;
        }
    }

    std::vector<MonthlyAvg> result;
    result.reserve(global_sums.size());
    for (const auto& [key, val] : global_sums)
        result.push_back({smKeyStation(key), smKeyMonth(key), float(val.first / val.second)});
    return result;
}

// Fluctuation detection
std::vector<Fluctuation> DataProcessor::detectFluctuations() const {
    // Step 1: per-(station, month, year) sums
    std::unordered_map<uint64_t, SumCount> year_sums;

    #pragma omp parallel
    {
        std::unordered_map<uint64_t, SumCount> local;

        #pragma omp for nowait schedule(static)
        for (int i = 0; i < (int)measurements.size(); i++) {
            const auto& m = measurements[i];
            auto& [sum, count] = local[smyKey(m.station_id, m.month, m.year)];
            sum += m.value;
            count++;
        }

        #pragma omp critical
        for (auto& [key, val] : local) {
            auto& [sum, count] = year_sums[key];
            sum   += val.first;
            count += val.second;
        }
    }

    // Step 2: group by (station, month) -> vector of (year, avg_temp)
    std::unordered_map<uint64_t, std::vector<std::pair<uint16_t, float>>> by_station_month;
    for (const auto& [key, val] : year_sums) {
        uint64_t sm = smKey(smyKeyStation(key), smyKeyMonth(key));
        by_station_month[sm].emplace_back(smyKeyYear(key), float(val.first / val.second));
    }

    // Step 3: detect fluctuations per (station, month) — embarrassingly parallel
    std::vector<std::pair<uint64_t, std::vector<std::pair<uint16_t, float>>>> items(
        by_station_month.begin(), by_station_month.end());

    int nthreads = omp_get_max_threads();
    std::vector<std::vector<Fluctuation>> thread_results(nthreads);

    #pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < (int)items.size(); i++) {
        int tid = omp_get_thread_num();
        auto& [key, year_avgs] = items[i];

        std::sort(year_avgs.begin(), year_avgs.end());
        if (year_avgs.size() < 2) continue;

        float min_t = year_avgs[0].second, max_t = year_avgs[0].second;
        for (auto& [y, t] : year_avgs) {
            min_t = std::min(min_t, t);
            max_t = std::max(max_t, t);
        }

        float threshold = 0.75f * (max_t - min_t);

        uint32_t station_id = smKeyStation(key);
        uint8_t  month      = smKeyMonth(key);

        for (size_t j = 1; j < year_avgs.size(); j++) {
            if (year_avgs[j].first != year_avgs[j - 1].first + 1) continue;
            float diff = std::abs(year_avgs[j].second - year_avgs[j - 1].second);
            if (diff > threshold)
                thread_results[tid].push_back({station_id, month, year_avgs[j].first, diff});
        }
    }

    std::vector<Fluctuation> result;
    for (auto& v : thread_results)
        result.insert(result.end(), v.begin(), v.end());
    return result;
}
