#pragma once
// Deterministic, portable randomness. std:: distributions differ between standard
// libraries, so only the raw mt19937_64 stream is used and sampling is done by hand.
// Each (node, purpose) pair gets its own stream seeded by hashing its *name*, so adding
// a node never reshuffles the draws of any other node (common random numbers).
#include <cmath>
#include <cstdint>
#include <random>
#include <span>
#include <string_view>

namespace factory {

[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

[[nodiscard]] constexpr std::uint64_t fnv1a(std::string_view s) noexcept {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x100000001B3ULL;
    }
    return h;
}

class RngStream {
public:
    RngStream() : engine_(1) {}
    RngStream(std::uint64_t master_seed, std::string_view key)
        : engine_(splitmix64(master_seed ^ splitmix64(fnv1a(key)))) {}

    // Uniform in [0, 1).
    [[nodiscard]] double uniform() {
        return static_cast<double>(engine_() >> 11) * (1.0 / 9007199254740992.0);
    }
    [[nodiscard]] double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }
    [[nodiscard]] double exponential(double mean) { return -mean * std::log(1.0 - uniform()); }
    [[nodiscard]] double standard_normal() {
        constexpr double kTwoPi = 6.283185307179586;
        const double u1 = 1.0 - uniform();  // (0,1]
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(kTwoPi * u2);
    }
    [[nodiscard]] bool chance(double p) { return uniform() < p; }

    // Index drawn proportionally to `weights` (must be non-empty, non-negative).
    [[nodiscard]] std::size_t pick(std::span<const double> weights) {
        double total = 0;
        for (double w : weights) total += w;
        double u = uniform() * total;
        for (std::size_t i = 0; i < weights.size(); ++i) {
            if (u < weights[i]) return i;
            u -= weights[i];
        }
        return weights.size() - 1;
    }

private:
    std::mt19937_64 engine_;
};

}  // namespace factory
