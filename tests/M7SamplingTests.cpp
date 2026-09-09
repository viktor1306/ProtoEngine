#include "benchmark/BenchmarkStats.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

using namespace proto::benchmark;

int main() {
    try {
        BenchmarkSampleReservoir reservoir(32, 0x123456789abcdef0ull);
        for (uint64_t frame = 0; frame < 10000; ++frame) {
            BenchmarkSample sample;
            sample.frame = frame;
            sample.wallMilliseconds = static_cast<double>(frame);
            reservoir.add(std::move(sample));
        }
        if (reservoir.seen() != 10000 || reservoir.samples().size() != 32)
            throw std::runtime_error("Reservoir count/capacity contract failed");
        std::unordered_set<uint64_t> frames;
        for (const auto& sample : reservoir.samples())
            frames.insert(sample.frame);
        if (frames.size() != reservoir.samples().size())
            throw std::runtime_error("Reservoir retained duplicate frame samples");
        const auto minimum = *std::min_element(frames.begin(), frames.end());
        const auto maximum = *std::max_element(frames.begin(), frames.end());
        if (minimum >= 2500 || maximum < 7500)
            throw std::runtime_error("Reservoir retained only an early run prefix");
        std::array<uint64_t,4> bins{};
        for (uint64_t seed = 1; seed <= 64; ++seed) {
            BenchmarkSampleReservoir varied(32, seed * 0x9e3779b97f4a7c15ull);
            for (uint64_t frame = 0; frame < 10000; ++frame) {
                BenchmarkSample value;
                value.frame = frame;
                varied.add(std::move(value));
            }
            for (const auto& value : varied.samples()) ++bins[value.frame / 2500];
        }
        for (const auto count : bins)
            if (count < 350 || count > 680)
                throw std::runtime_error("Reservoir overweights part of the measured timeline");

        BenchmarkSampleReservoir empty(0);
        BenchmarkSample sample;
        empty.add(std::move(sample));
        if (empty.seen() != 1 || !empty.samples().empty())
            throw std::runtime_error("Zero-capacity reservoir contract failed");
        std::cout << "M7 uniform bounded sample reservoir passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
