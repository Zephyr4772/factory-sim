#include "factory/params.hpp"

#include <algorithm>

namespace factory {

namespace {
double req_number(const json& j, const char* key, const std::string& ctx) {
    if (!j.contains(key) || !j[key].is_number())
        throw ConfigError(ctx + ": distribution needs numeric '" + key + "'");
    return j[key].get<double>();
}
}  // namespace

Distribution parse_distribution(const json& j, const std::string& ctx) {
    if (j.is_number()) return Distribution::constant(j.get<double>());
    if (!j.is_object()) throw ConfigError(ctx + ": expected a number or a {\"dist\": ...} object");
    const std::string kind = j.value("dist", j.value("kind", std::string("constant")));
    Distribution d;
    if (kind == "constant") {
        d = Distribution::constant(req_number(j, "value", ctx));
    } else if (kind == "exponential") {
        if (j.contains("rate")) {
            const double rate = req_number(j, "rate", ctx);
            if (rate <= 0) throw ConfigError(ctx + ": rate must be > 0");
            d = Distribution::exponential(1.0 / rate);
        } else {
            d = Distribution::exponential(req_number(j, "mean", ctx));
        }
    } else if (kind == "normal") {
        d = Distribution::normal(req_number(j, "mean", ctx), req_number(j, "stddev", ctx));
    } else if (kind == "uniform") {
        d = {Distribution::Kind::Uniform, req_number(j, "min", ctx), req_number(j, "max", ctx), 0};
    } else if (kind == "triangular") {
        d = {Distribution::Kind::Triangular, req_number(j, "min", ctx), req_number(j, "mode", ctx),
             req_number(j, "max", ctx)};
        if (!(d.a <= d.b && d.b <= d.c)) throw ConfigError(ctx + ": triangular needs min <= mode <= max");
    } else if (kind == "lognormal") {
        d = {Distribution::Kind::LogNormal, req_number(j, "mean", ctx), req_number(j, "stddev", ctx), 0};
    } else {
        throw ConfigError(ctx + ": unknown distribution '" + kind +
                          "' (constant, exponential, normal, uniform, triangular, lognormal)");
    }
    if (d.a < 0 || (d.kind != Distribution::Kind::Uniform && d.b < 0))
        throw ConfigError(ctx + ": distribution parameters must be non-negative");
    return d;
}

json distribution_to_json(const Distribution& d) {
    using K = Distribution::Kind;
    switch (d.kind) {
        case K::Constant: return {{"dist", "constant"}, {"value", d.a}};
        case K::Exponential: return {{"dist", "exponential"}, {"mean", d.a}};
        case K::Normal: return {{"dist", "normal"}, {"mean", d.a}, {"stddev", d.b}};
        case K::Uniform: return {{"dist", "uniform"}, {"min", d.a}, {"max", d.b}};
        case K::Triangular: return {{"dist", "triangular"}, {"min", d.a}, {"mode", d.b}, {"max", d.c}};
        case K::LogNormal: return {{"dist", "lognormal"}, {"mean", d.a}, {"stddev", d.b}};
    }
    return {};
}

ParamReader::ParamReader(const json& obj, std::string context) : obj_(obj), ctx_(std::move(context)) {
    if (!obj_.is_object()) throw ConfigError(ctx_ + ": parameters must be a JSON object");
}

bool ParamReader::has(std::string_view key) const { return obj_.contains(std::string(key)); }

const json* ParamReader::find(std::string_view key) {
    used_.emplace(key);
    auto it = obj_.find(std::string(key));
    return it == obj_.end() ? nullptr : &*it;
}

void ParamReader::fail(const std::string& message) const { throw ConfigError(ctx_ + ": " + message); }

double ParamReader::number(std::string_view key, double def) {
    const json* v = find(key);
    if (!v) return def;
    if (!v->is_number()) fail("'" + std::string(key) + "' must be a number");
    return v->get<double>();
}

int ParamReader::integer(std::string_view key, int def) {
    const json* v = find(key);
    if (!v) return def;
    if (!v->is_number_integer()) fail("'" + std::string(key) + "' must be an integer");
    return v->get<int>();
}

bool ParamReader::boolean(std::string_view key, bool def) {
    const json* v = find(key);
    if (!v) return def;
    if (!v->is_boolean()) fail("'" + std::string(key) + "' must be true or false");
    return v->get<bool>();
}

std::string ParamReader::string(std::string_view key, std::string def) {
    const json* v = find(key);
    if (!v) return def;
    if (!v->is_string()) fail("'" + std::string(key) + "' must be a string");
    return v->get<std::string>();
}

std::optional<Distribution> ParamReader::opt_dist(std::string_view key) {
    const json* v = find(key);
    if (!v) return std::nullopt;
    return parse_distribution(*v, ctx_ + "." + std::string(key));
}

Distribution ParamReader::dist(std::string_view key, Distribution def) {
    auto d = opt_dist(key);
    return d ? *d : def;
}

const json* ParamReader::object(std::string_view key) { return find(key); }

void ParamReader::finish() const {
    for (auto it = obj_.begin(); it != obj_.end(); ++it) {
        if (used_.find(it.key()) == used_.end()) {
            std::string known;
            for (const auto& k : used_) known += (known.empty() ? "" : ", ") + k;
            fail("unknown parameter '" + it.key() + "' (known: " + known + ")");
        }
    }
}

}  // namespace factory
