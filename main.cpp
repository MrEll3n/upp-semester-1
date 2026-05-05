#include "data_processor.h"
#include "svg_writer.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <omp.h>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <stanice.csv> <mereni.csv> --serial|--parallel\n";
        return 1;
    }

    const std::string stations_path     = argv[1];
    const std::string measurements_path = argv[2];
    const std::string mode              = argv[3];
    const bool parallel = (mode == "--parallel");

    omp_set_num_threads(parallel ? omp_get_max_threads() : 1);
    std::cout << "Mode: " << mode
              << "  threads: " << (parallel ? omp_get_max_threads() : 1) << "\n";

    auto t0 = std::chrono::high_resolution_clock::now();

    // Parse
    DataProcessor dp;
    if (!dp.parseData(stations_path, measurements_path)) {
        std::cerr << "Failed to parse input files\n";
        return 1;
    }
    std::cout << "Loaded:          " << dp.stations.size()    << " stations, "
              << dp.measurements.size() << " measurements\n";

    // Filter
    dp.filterStations();
    std::cout << "After filtering: " << dp.stations.size()    << " stations, "
              << dp.measurements.size() << " measurements\n";

    if (dp.measurements.empty()) {
        std::cerr << "No measurements left after filtering\n";
        return 1;
    }

    // Global min/max for colour scale — thread-local then merge
    // (MSVC classic OpenMP doesn't support min/max reductions)
    int nthreads = omp_get_max_threads();
    std::vector<float> t_min(nthreads, dp.measurements[0].value);
    std::vector<float> t_max(nthreads, dp.measurements[0].value);

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < (int)dp.measurements.size(); i++) {
        int tid = omp_get_thread_num();
        float v = dp.measurements[i].value;
        if (v < t_min[tid]) t_min[tid] = v;
        if (v > t_max[tid]) t_max[tid] = v;
    }

    float global_min = *std::min_element(t_min.begin(), t_min.end());
    float global_max = *std::max_element(t_max.begin(), t_max.end());
    std::cout << "Global temp range: [" << global_min << ", " << global_max << "]\n";

    // Monthly averages (needed for SVG)
    auto monthly_avgs = dp.computeMonthlyAverages();

    // Fluctuation detection
    auto fluctuations = dp.detectFluctuations();
    std::cout << "Fluctuations detected: " << fluctuations.size() << "\n";

    // Write vykyvy.csv
    {
        std::ofstream csv("out/vykyvy.csv");
        csv << "station_id;month;year;diff\n";
        for (const auto& f : fluctuations)
            csv << f.station_id << ";" << int(f.month) << ";"
                << f.year      << ";" << f.diff        << "\n";
    }

    // Generate SVGs (looks for mapa_cr.svg in the working directory)
    std::filesystem::create_directories("out");
    generateSVGs(dp.stations, monthly_avgs, global_min, global_max, "mapa_cr.svg", "out");
    std::cout << "SVGs written (leden.svg ... prosinec.svg)\n";

    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "Total time: " << elapsed << " s\n";

    return 0;
}
