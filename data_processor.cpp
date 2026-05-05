#include "data_processor.h"
#include <cstdlib>
#include <fstream>

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

    // skip header line
    while (p < end && *p != '\n') p++;
    if (p < end) p++;

    // collect line starts
    std::vector<char*> lines;
    lines.reserve(64);
    while (p < end) {
        if (*p != '\r' && *p != '\n') { lines.push_back(p); }
        while (p < end && *p != '\n') { p++; }
        if (p < end) { p++; }
    }

    stations.resize(lines.size());
    for (size_t i = 0; i < lines.size(); i++) {
        Station& s = stations[i];
        char* next;
        p = lines[i];
        s.station_id = static_cast<uint32_t>(std::strtoul(p, &next, 10)); p = next + 1;
        char* name_start = p;
        while (*p != ';') p++;
        s.name.assign(name_start, p); p++;
        s.latitude   = std::strtof(p, &next); p = next + 1;
        s.longtitude = std::strtof(p, nullptr);
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

    // skip header line
    while (p < end && *p != '\n') p++;
    if (p < end) p++;

    // collect line starts
    std::vector<char*> lines;
    lines.reserve(size / 18);
    while (p < end) {
        if (*p != '\r' && *p != '\n') { lines.push_back(p); }
        while (p < end && *p != '\n') { p++; }
        if (p < end) { p++; }
    }

    measurements.resize(lines.size());

    for (size_t i = 0; i < lines.size(); i++) {
        Measurement& m = measurements[i];
        char* next;
        p = lines[i];
        m.station_id = static_cast<uint32_t>(std::strtoul(p,  &next, 10)); p = next + 1;
        m.ordinal    = static_cast<uint64_t>(std::strtoull(p, &next, 10)); p = next + 1;
        m.year       = static_cast<uint16_t>(std::strtoul(p,  &next, 10)); p = next + 1;
        m.month      = static_cast<uint8_t> (std::strtoul(p,  &next, 10)); p = next + 1;
        m.day        = static_cast<uint8_t> (std::strtoul(p,  &next, 10)); p = next + 1;
        m.value      = std::strtof(p, nullptr);
    }
    return true;
}

bool DataProcessor::parseData() {
    if (!parseStations(stations, PATH_STATIONS)) { return false; }
    if (!parseMeasurements(measurements, PATH_MEASUREMENT)) { return false; }
    return true;
}
