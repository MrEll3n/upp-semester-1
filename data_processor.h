#include <cstdint>
#include <string>
#include <vector>

const std::string PATH_MEASUREMENT = "data/mereni.csv";
const std::string PATH_STATIONS = "data/stanice.csv";

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

class DataProcessor {
    public:
        std::vector<Measurement> measurements;
        std::vector<Station> stations;

    bool parseData();

    static bool parseStations(std::vector<Station>& stations, const std::string& path);

    static bool parseMeasurements(std::vector<Measurement>& measurements, const std::string& path);
};
