#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Measurement {
    uint32_t station_id;
    uint64_t ordinal;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    float value;
};

struct Station {
    uint32_t station_id;
    std::string name;
    float latitude;
    float longtitude;
};

struct MonthlyAvg {
    uint32_t station_id;
    uint8_t month;
    float avg_temp;
};

struct Fluctuation {
    uint32_t station_id;
    uint8_t month;
    uint16_t year;  // second year of the consecutive pair
    float diff;
};

class DataProcessor {
public:
    std::vector<Measurement> measurements;
    std::vector<Station> stations;

    bool parseData(const std::string& stations_path, const std::string& measurements_path);
    void filterStations();
    std::vector<MonthlyAvg> computeMonthlyAverages() const;
    std::vector<Fluctuation> detectFluctuations() const;

    static bool parseStations(std::vector<Station>& stations, const std::string& path);
    static bool parseMeasurements(std::vector<Measurement>& measurements, const std::string& path);
};
