#pragma once
#include <string>
#include <vector>
#include "data_processor.h"

// Generates leden.svg ... prosinec.svg in output_dir (empty = current dir).
// Reads base_map_path as the blank Czech Republic SVG and overlays station
// circles coloured by average monthly temperature on a blue-yellow-red scale.
void generateSVGs(
    const std::vector<Station>&    stations,
    const std::vector<MonthlyAvg>& monthly_avgs,
    float global_min,
    float global_max,
    const std::string& base_map_path,
    const std::string& output_dir
);
