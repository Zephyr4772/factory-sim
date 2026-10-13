#pragma once
// Typed, typo-catching access to a node's JSON parameters.
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "factory/distribution.hpp"

namespace factory {

using json = nlohmann::json;

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Accepts a number (constant) or {"dist": "...", ...}:
//   constant{value} exponential{mean|rate} normal{mean,stddev} uniform{min,max}
//   triangular{min,mode,max} lognormal{mean,stddev}
[[nodiscard]] Distribution parse_distribution(const json& j, const std::string& context);
[[nodiscard]] json distribution_to_json(const Distribution& d);

class ParamReader {
public:
    ParamReader(const json& obj, std::string context);

    [[nodiscard]] bool has(std::string_view key) const;
    [[nodiscard]] double number(std::string_view key, double def);
    [[nodiscard]] int integer(std::string_view key, int def);
    [[nodiscard]] bool boolean(std::string_view key, bool def);
    [[nodiscard]] std::string string(std::string_view key, std::string def);
    [[nodiscard]] Distribution dist(std::string_view key, Distribution def);
    [[nodiscard]] std::optional<Distribution> opt_dist(std::string_view key);
    [[nodiscard]] const json* object(std::string_view key);  // raw value, marked as used

    // Throws ConfigError if the object contains keys that were never read (typos).
    void finish() const;
    [[noreturn]] void fail(const std::string& message) const;
    [[nodiscard]] const std::string& context() const noexcept { return ctx_; }

private:
    const json* find(std::string_view key);
    json obj_;
    std::string ctx_;
    std::set<std::string, std::less<>> used_;
};

}  // namespace factory
