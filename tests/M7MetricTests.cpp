#include "benchmark/BenchmarkStats.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace proto::benchmark;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int main() {
    try {
        BenchmarkReport report;
        report.rawFrameCount = 4;
        report.actualDurationMilliseconds = 10;
        for (unsigned i = 1; i <= 4; ++i) {
            BenchmarkSample sample;
            sample.frame = i;
            sample.wallMilliseconds = i;
            report.samples.push_back(sample);
        }
        report.gpuSamples = {{1,2},{2,4},{2,1000},{3,6}};
        auto summary = summarize(report);
        check(summary.wall.count == 4 && summary.wall.median == 2.5,
              "Median does not interpolate the middle pair");
        check(std::abs(summary.wall.p95 - 3.85) < 1e-10 && std::abs(summary.wall.p99 - 3.97) < 1e-10,
              "Tail percentiles use a different estimator");
        check(summary.gpuSampleCount == 3 && summary.gpu.median == 4,
              "Duplicate completed GPU serial skewed the distribution");
        report.samples[0].wallMilliseconds = std::numeric_limits<double>::quiet_NaN();
        summary = summarize(report);
        check(summary.wall.count == 3 && summary.wall.median == 3, "Invalid sample became a zero-duration frame");
        check(summarize(BenchmarkReport{}).gpu.count == 0, "Unavailable GPU timing was counted");
        const auto paths = benchmarkOutputPaths("folder/run.json");
        check(paths.json == "folder/run.json" && paths.csv == "folder/run.csv" && paths.manifest == "folder/run.manifest.json",
              "Metric output artifacts collide");
        std::cout << "M7 quantiles, GPU serial deduplication and unavailable samples passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
