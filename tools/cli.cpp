#include "cli.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numeric>
#include <sstream>

namespace mr::cli {

ArgParser::ArgParser(std::string program, std::string synopsis)
    : program_(std::move(program)), synopsis_(std::move(synopsis)) {}

void ArgParser::option(std::string name, std::string help, std::string default_value) {
  option_order_.push_back(name);
  options_[std::move(name)] = Option{std::move(help), std::move(default_value), false, false};
}

void ArgParser::flag(std::string name, std::string help) {
  option_order_.push_back(name);
  options_[std::move(name)] = Option{std::move(help), "", true, false};
}

void ArgParser::positional(std::string name, std::string help, bool required) {
  positional_specs_.push_back({std::move(name), std::move(help), required});
}

Status ArgParser::parse(int argc, const char* const* argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if (arg == "--help" || arg == "-h") {
      help_ = true;
      return ok_status();
    }
    if (!arg.starts_with("--")) {
      positionals_.emplace_back(arg);
      continue;
    }
    std::string_view name = arg.substr(2);
    std::optional<std::string_view> inline_value;
    if (const auto eq = name.find('='); eq != std::string_view::npos) {
      inline_value = name.substr(eq + 1);
      name = name.substr(0, eq);
    }
    const auto it = options_.find(name);
    if (it == options_.end()) {
      return make_error(Errc::kInvalidArgument, "unknown option --" + std::string(name));
    }
    Option& opt = it->second;
    opt.given = true;
    if (opt.is_flag) {
      if (inline_value) {
        return make_error(Errc::kInvalidArgument,
                          "flag --" + std::string(name) + " takes no value");
      }
      continue;
    }
    if (inline_value) {
      opt.value = std::string(*inline_value);
    } else if (i + 1 < args.size()) {
      opt.value = std::string(args[++i]);
    } else {
      return make_error(Errc::kInvalidArgument, "option --" + std::string(name) + " needs a value");
    }
  }
  std::size_t required = 0;
  for (const auto& p : positional_specs_) {
    required += p.required ? 1 : 0;
  }
  if (positionals_.size() < required) {
    return make_error(Errc::kInvalidArgument,
                      "missing argument <" + positional_specs_[positionals_.size()].name + ">");
  }
  if (positionals_.size() > positional_specs_.size()) {
    return make_error(Errc::kInvalidArgument, "too many arguments");
  }
  return ok_status();
}

std::string ArgParser::usage() const {
  std::ostringstream out;
  out << "usage: " << program_;
  for (const auto& p : positional_specs_) {
    out << (p.required ? " <" : " [") << p.name << (p.required ? ">" : "]");
  }
  out << " [options]\n\n" << synopsis_ << "\n";
  if (!positional_specs_.empty()) {
    out << "\narguments:\n";
    for (const auto& p : positional_specs_) {
      out << "  " << p.name << "\n      " << p.help << "\n";
    }
  }
  out << "\noptions:\n";
  for (const auto& name : option_order_) {
    const Option& o = options_.at(name);
    out << "  --" << name;
    if (!o.is_flag) {
      out << " <value>  (default: " << (o.value.empty() ? "none" : o.value) << ")";
    }
    out << "\n      " << o.help << "\n";
  }
  return out.str();
}

const std::string& ArgParser::get(const std::string& name) const {
  return options_.at(name).value;
}

bool ArgParser::is_set(const std::string& name) const {
  return options_.at(name).given;
}

Result<std::int64_t> ArgParser::get_int(const std::string& name) const {
  const std::string& v = get(name);
  std::int64_t out = 0;
  const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
  if (ec != std::errc() || ptr != v.data() + v.size()) {
    return make_error(Errc::kInvalidArgument, "--" + name + " expects an integer, got '" + v + "'");
  }
  return out;
}

Result<double> ArgParser::get_double(const std::string& name) const {
  const std::string& v = get(name);
  double out = 0;
  const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
  if (ec != std::errc() || ptr != v.data() + v.size() || !std::isfinite(out)) {
    return make_error(Errc::kInvalidArgument, "--" + name + " expects a number, got '" + v + "'");
  }
  return out;
}

std::optional<std::string> ArgParser::get_positional(std::size_t index) const {
  if (index < positionals_.size()) {
    return positionals_[index];
  }
  return std::nullopt;
}

std::vector<std::string> split(std::string_view text, char sep) {
  std::vector<std::string> out;
  if (text.empty()) {
    return out;
  }
  std::size_t start = 0;
  for (;;) {
    const std::size_t pos = text.find(sep, start);
    out.emplace_back(
        text.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
    if (pos == std::string_view::npos) {
      break;
    }
    start = pos + 1;
  }
  return out;
}

Percentiles percentiles(std::vector<std::int64_t> samples) {
  Percentiles p;
  p.count = samples.size();
  if (samples.empty()) {
    return p;
  }
  std::sort(samples.begin(), samples.end());
  // Nearest-rank percentile.
  auto at = [&samples](double q) {
    const auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(samples.size())));
    return static_cast<double>(samples[std::clamp<std::size_t>(rank, 1, samples.size()) - 1]);
  };
  p.p50_ns = at(0.50);
  p.p90_ns = at(0.90);
  p.p99_ns = at(0.99);
  p.p999_ns = at(0.999);
  p.max_ns = static_cast<double>(samples.back());
  const long double sum = std::accumulate(
      samples.begin(), samples.end(), 0.0L,
      [](long double acc, std::int64_t v) { return acc + static_cast<long double>(v); });
  p.mean_ns = static_cast<double>(sum / static_cast<long double>(samples.size()));
  return p;
}

}  // namespace mr::cli
