#include "factory/distribution.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace factory {

double Distribution::sample(RngStream& rng) const {
    switch (kind) {
        case Kind::Constant:
            return std::max(0.0, a);
        case Kind::Exponential:
            return a > 0 ? rng.exponential(a) : 0.0;
        case Kind::Normal: {
            // Resample instead of clamping: clamping at 0 would bias the mean upwards.
            for (int i = 0; i < 64; ++i) {
                const double x = a + b * rng.standard_normal();
                if (x > 0) return x;
            }
            return std::max(0.0, a * 0.01);
        }
        case Kind::Uniform:
            return std::max(0.0, rng.uniform(std::min(a, b), std::max(a, b)));
        case Kind::Triangular: {
            const double lo = a, mode = b, hi = c;
            if (hi <= lo) return std::max(0.0, lo);
            const double u = rng.uniform();
            const double f = (mode - lo) / (hi - lo);
            if (u < f) return lo + std::sqrt(u * (hi - lo) * (mode - lo));
            return hi - std::sqrt((1.0 - u) * (hi - lo) * (hi - mode));
        }
        case Kind::LogNormal: {
            if (a <= 0) return 0.0;
            const double ratio = b / a;
            const double sigma2 = std::log(1.0 + ratio * ratio);
            const double mu = std::log(a) - sigma2 / 2.0;
            return std::exp(mu + std::sqrt(sigma2) * rng.standard_normal());
        }
    }
    return 0.0;
}

double Distribution::mean() const {
    switch (kind) {
        case Kind::Constant:
        case Kind::Exponential:
        case Kind::Normal:
        case Kind::LogNormal: return a;
        case Kind::Uniform: return (a + b) / 2.0;
        case Kind::Triangular: return (a + b + c) / 3.0;
    }
    return 0.0;
}

std::string Distribution::describe() const {
    std::ostringstream os;
    switch (kind) {
        case Kind::Constant: os << "const(" << a << ")"; break;
        case Kind::Exponential: os << "exp(mean=" << a << ")"; break;
        case Kind::Normal: os << "normal(" << a << "," << b << ")"; break;
        case Kind::Uniform: os << "uniform(" << a << "," << b << ")"; break;
        case Kind::Triangular: os << "tri(" << a << "," << b << "," << c << ")"; break;
        case Kind::LogNormal: os << "lognormal(" << a << "," << b << ")"; break;
    }
    return os.str();
}

}  // namespace factory
