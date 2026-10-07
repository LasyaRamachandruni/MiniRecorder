#pragma once

// Minimal command line parsing shared by the mr_* tools. Supports
// "--name value", "--name=value", boolean "--flag" and positional arguments.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <mr/result.hpp>

namespace mr::cli {

class ArgParser {
 public:
  ArgParser(std::string program, std::string synopsis);

  void option(std::string name, std::string help, std::string default_value);
  void flag(std::string name, std::string help);
  void positional(std::string name, std::string help, bool required = true);

  // Returns an error for unknown options, missing values or missing
  // positionals. "--help" sets help_requested().
  [[nodiscard]] Status parse(int argc, const char* const* argv);

  [[nodiscard]] bool help_requested() const noexcept { return help_; }
  [[nodiscard]] std::string usage() const;

  [[nodiscard]] const std::string& get(const std::string& name) const;
  [[nodiscard]] bool is_set(const std::string& name) const;  // flag given / option given explicitly
  [[nodiscard]] Result<std::int64_t> get_int(const std::string& name) const;
  [[nodiscard]] Result<double> get_double(const std::string& name) const;
  [[nodiscard]] std::optional<std::string> get_positional(std::size_t index) const;

 private:
  struct Option {
    std::string help;
    std::string value;
    bool is_flag = false;
    bool given = false;
  };
  struct Positional {
    std::string name;
    std::string help;
    bool required = true;
  };

  std::string program_;
  std::string synopsis_;
  std::map<std::string, Option, std::less<>> options_;
  std::vector<std::string> option_order_;
  std::vector<Positional> positional_specs_;
  std::vector<std::string> positionals_;
  bool help_ = false;
};

// Splits "a,b,c" into {"a","b","c"}; empty input yields an empty vector.
[[nodiscard]] std::vector<std::string> split(std::string_view text, char sep);

// Formats a percentile summary of latency samples (nanoseconds), e.g.
// "p50 1.23 ms  p99 4.56 ms  max 7.89 ms  (n=1000)".
struct Percentiles {
  std::size_t count = 0;
  double p50_ns = 0;
  double p90_ns = 0;
  double p99_ns = 0;
  double p999_ns = 0;
  double max_ns = 0;
  double mean_ns = 0;
};
[[nodiscard]] Percentiles percentiles(std::vector<std::int64_t> samples);

}  // namespace mr::cli
