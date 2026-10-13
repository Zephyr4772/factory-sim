#pragma once
#include <string>

#include "factory/rng.hpp"

namespace factory {

// Time distributions, in seconds. Meaning of a/b/c per kind:
//   Constant a=value | Exponential a=mean | Normal a=mean b=stddev (resampled until > 0)
//   Uniform a=min b=max | Triangular a=min b=mode c=max | LogNormal a=mean b=stddev
struct Distribution {
    enum class Kind { Constant, Exponential, Normal, Uniform, Triangular, LogNormal };
    Kind kind = Kind::Constant;
    double a = 0.0;
    double b = 0.0;
    double c = 0.0;

    [[nodiscard]] static Distribution constant(double v) { return {Kind::Constant, v, 0, 0}; }
    [[nodiscard]] static Distribution exponential(double mean) { return {Kind::Exponential, mean, 0, 0}; }
    [[nodiscard]] static Distribution normal(double mean, double sd) { return {Kind::Normal, mean, sd, 0}; }

    [[nodiscard]] double sample(RngStream& rng) const;  // seconds, never negative
    [[nodiscard]] double mean() const;
    [[nodiscard]] std::string describe() const;
};

}  // namespace factory
