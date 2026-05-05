#include "data_processor.h"
#include <iostream>

int main() {
    DataProcessor dp;
    if (!dp.parseData()) {
        std::cerr << "Failed to parse data\n";
        return 1;
    }
    std::cout << "Stations: " << dp.stations.size() << "\n";
    std::cout << "Measurements: " << dp.measurements.size() << "\n";
    return 0;
}
