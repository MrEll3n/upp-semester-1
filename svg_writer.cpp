#include "svg_writer.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <omp.h>
#include <sstream>
#include <unordered_map>

// Geographic bounds of the provided blank CZ map
static constexpr double MAP_LAT_MAX = 51.038;
static constexpr double MAP_LAT_MIN = 48.521;
static constexpr double MAP_LON_MIN = 12.102;
static constexpr double MAP_LON_MAX = 18.867;

static const char* MONTH_NAMES[12] = {
    "leden", "unor", "brezen", "duben", "kveten", "cerven",
    "cervenec", "srpen", "zari", "rijen", "listopad", "prosinec"
};

struct RGB { uint8_t r, g, b; };

// Blue (cold) -> Yellow (middle) -> Red (hot)
static RGB tempToColor(float temp, float min_temp, float max_temp) {
    float t = (temp - min_temp) / (max_temp - min_temp);
    t = std::max(0.0f, std::min(1.0f, t));
    if (t < 0.5f) {
        float s = t * 2.0f;
        return {uint8_t(s * 255), uint8_t(s * 255), uint8_t((1.0f - s) * 255)};
    } else {
        float s = (t - 0.5f) * 2.0f;
        return {255, uint8_t((1.0f - s) * 255), 0};
    }
}

// Extract SVG canvas size from width/height attributes or viewBox
static std::pair<float, float> parseSVGDimensions(const std::string& svg) {
    auto readAttr = [&](const std::string& attr) -> float {
        std::string needle = attr + "=\"";
        size_t pos = svg.find(needle);
        if (pos == std::string::npos) return 0.0f;
        pos += needle.size();
        size_t end = svg.find('"', pos);
        try { return std::stof(svg.substr(pos, end - pos)); } catch (...) { return 0.0f; }
    };

    // Prefer viewBox (more reliable)
    size_t vb = svg.find("viewBox=\"");
    if (vb != std::string::npos) {
        const char* p = svg.c_str() + vb + 9;
        char* next;
        strtof(p, &next); p = next + 1;  // min-x
        strtof(p, &next); p = next + 1;  // min-y
        float w = strtof(p, &next); p = next + 1;
        float h = strtof(p, nullptr);
        if (w > 0 && h > 0) return {w, h};
    }

    float w = readAttr("width");
    float h = readAttr("height");
    return {w > 0 ? w : 800.0f, h > 0 ? h : 517.0f};
}

void generateSVGs(
    const std::vector<Station>&    stations,
    const std::vector<MonthlyAvg>& monthly_avgs,
    float global_min,
    float global_max,
    const std::string& base_map_path,
    const std::string& output_dir)
{
    // Read base map once
    std::string base_map;
    {
        std::ifstream f(base_map_path);
        if (f.is_open()) {
            base_map.assign(std::istreambuf_iterator<char>(f), {});
        } else {
            // Minimal fallback if the blank map file is not present
            base_map = "<svg xmlns=\"http://www.w3.org/2000/svg\" "
                       "width=\"800\" height=\"517\"></svg>";
        }
    }

    auto [svg_w, svg_h] = parseSVGDimensions(base_map);

    // Find where to insert circles (just before </svg>)
    size_t end_tag = base_map.rfind("</svg>");
    if (end_tag == std::string::npos) end_tag = base_map.size();
    std::string svg_prefix = base_map.substr(0, end_tag);

    // Build fast lookup: encoded(station_id, month) -> avg_temp
    std::unordered_map<uint64_t, float> avg_lookup;
    avg_lookup.reserve(monthly_avgs.size());
    for (const auto& a : monthly_avgs) {
        uint64_t key = ((uint64_t)a.station_id << 4) | (a.month - 1);
        avg_lookup[key] = a.avg_temp;
    }

    std::string prefix = output_dir.empty() ? "" : (output_dir + "/");

    // One SVG per month — embarrassingly parallel
    #pragma omp parallel for schedule(static)
    for (int month = 1; month <= 12; month++) {
        std::ostringstream circles;

        for (const auto& s : stations) {
            uint64_t key = ((uint64_t)s.station_id << 4) | (month - 1);
            auto it = avg_lookup.find(key);
            if (it == avg_lookup.end()) continue;

            RGB color = tempToColor(it->second, global_min, global_max);

            float x = float((s.longtitude - MAP_LON_MIN) / (MAP_LON_MAX - MAP_LON_MIN) * svg_w);
            float y = float((MAP_LAT_MAX - s.latitude)   / (MAP_LAT_MAX - MAP_LAT_MIN) * svg_h);

            circles << "<circle cx=\"" << x << "\" cy=\"" << y << "\" r=\"5\" "
                    << "fill=\"rgb(" << int(color.r) << ","
                                     << int(color.g) << ","
                                     << int(color.b) << ")\" "
                    << "stroke=\"black\" stroke-width=\"0.5\"/>\n";
        }

        std::string svg = svg_prefix + circles.str() + "</svg>\n";
        std::ofstream out(prefix + MONTH_NAMES[month - 1] + ".svg");
        out << svg;
    }
}
