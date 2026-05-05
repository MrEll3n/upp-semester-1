#include "data_processor.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <omp.h>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

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
    // loop below trivially parallelizable with "#pragma omp parallel for"
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

// ---------------------------------------------------------------------------
// Filtering
// ---------------------------------------------------------------------------

void DataProcessor::filterStations() {
    struct StationStats {
        std::vector<uint16_t> years;  // may have duplicates, sorted+unique at end
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

// ---------------------------------------------------------------------------
// Monthly averages (for SVG coloring)
// ---------------------------------------------------------------------------

std::vector<MonthlyAvg> DataProcessor::computeMonthlyAverages() const {
    using Key = uint64_t;
    using Val = std::pair<double, uint32_t>;

    std::unordered_map<Key, Val> global_sums;

    // Thread-local accumulation, then critical merge
    #pragma omp parallel
    {
        std::unordered_map<Key, Val> local;

        #pragma omp for nowait schedule(static)
        for (int i = 0; i < (int)measurements.size(); i++) {
            const auto& m = measurements[i];
            Key key = ((Key)m.station_id << 4) | (m.month - 1);
            auto& [s, c] = local[key];
            s += m.value;
            c++;
        }

        #pragma omp critical
        for (auto& [k, v] : local) {
            auto& [gs, gc] = global_sums[k];
            gs += v.first;
            gc += v.second;
        }
    }

    std::vector<MonthlyAvg> result;
    result.reserve(global_sums.size());
    for (const auto& [key, val] : global_sums) {
        result.push_back({
            uint32_t(key >> 4),
            uint8_t((key & 0xF) + 1),
            float(val.first / val.second)
        });
    }
    return result;
}

// ---------------------------------------------------------------------------
// Fluctuation detection
// ---------------------------------------------------------------------------

std::vector<Fluctuation> DataProcessor::detectFluctuations() const {
    using Key3 = uint64_t;
    using Val   = std::pair<double, uint32_t>;

    // Step 1: per-(station, month, year) sums — thread-local + critical merge
    std::unordered_map<Key3, Val> year_month_sums;

    #pragma omp parallel
    {
        std::unordered_map<Key3, Val> local;

        #pragma omp for nowait schedule(static)
        for (int i = 0; i < (int)measurements.size(); i++) {
            const auto& m = measurements[i];
            Key3 key = ((Key3)m.station_id << 32) | ((Key3)m.month << 16) | m.year;
            auto& [s, c] = local[key];
            s += m.value;
            c++;
        }

        #pragma omp critical
        for (auto& [k, v] : local) {
            auto& [gs, gc] = year_month_sums[k];
            gs += v.first;
            gc += v.second;
        }
    }

    // Step 2: group by (station, month) -> sorted vector of (year, avg)
    using Key2 = uint64_t;
    std::unordered_map<Key2, std::vector<std::pair<uint16_t, float>>> station_month;

    for (const auto& [key, val] : year_month_sums) {
        uint32_t station_id = uint32_t(key >> 32);
        uint8_t  month      = uint8_t((key >> 16) & 0xFF);
        uint16_t year       = uint16_t(key & 0xFFFF);
        Key2 k2 = ((Key2)station_id << 4) | (month - 1);
        station_month[k2].emplace_back(year, float(val.first / val.second));
    }

    // Step 3: detect fluctuations per (station, month) — embarrassingly parallel
    std::vector<std::pair<Key2, std::vector<std::pair<uint16_t, float>>>> items(
        station_month.begin(), station_month.end());

    int nthreads = omp_get_max_threads();
    std::vector<std::vector<Fluctuation>> thread_results(nthreads);

    #pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < (int)items.size(); i++) {
        int tid = omp_get_thread_num();
        auto& [k2, year_avgs] = items[i];

        std::sort(year_avgs.begin(), year_avgs.end());
        if (year_avgs.size() < 2) continue;

        float min_t = year_avgs[0].second, max_t = year_avgs[0].second;
        for (auto& [y, t] : year_avgs) {
            min_t = std::min(min_t, t);
            max_t = std::max(max_t, t);
        }

        float threshold = 0.75f * (max_t - min_t);

        uint32_t station_id = uint32_t(k2 >> 4);
        uint8_t  month      = uint8_t((k2 & 0xF) + 1);

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
