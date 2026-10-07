// mr_recover: turns a crash leftover (.partial) into a readable log.

#include <chrono>
#include <cstdio>
#include <exception>
#include <format>
#include <iostream>

#include <mr/recovery.hpp>

#include "cli.hpp"

namespace {

int run(int argc, const char* const* argv) {
  mr::cli::ArgParser args(
      "mr_recover",
      "Scans a .partial log chunk by chunk, validates CRCs, truncates at the last\n"
      "good chunk, rebuilds the footer and renames the file.");
  args.positional("input", "file to recover (usually *.mrlog.partial)");
  args.option("output", "output path (default: input without .partial)", "");
  args.flag("no-verify", "check CRCs only; do not decompress and validate records");
  args.flag("dry-run", "scan and report without modifying anything");
  if (auto s = args.parse(argc, argv); !s) {
    std::cerr << "mr_recover: " << s.error().message << "\n\n" << args.usage();
    return 2;
  }
  if (args.help_requested()) {
    std::cout << args.usage();
    return 0;
  }
  const std::filesystem::path input = args.get_positional(0).value_or(std::string{});
  const std::filesystem::path output = args.get("output").empty()
                                           ? mr::default_recovery_output(input)
                                           : std::filesystem::path(args.get("output"));
  const mr::RecoveryOptions options{.verify_records = !args.is_set("no-verify"),
                                    .dry_run = args.is_set("dry-run")};

  const auto t0 = std::chrono::steady_clock::now();
  auto report = mr::recover_file(input, output, options);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!report) {
    std::cerr << "mr_recover: " << report.error().describe() << "\n";
    return 1;
  }
  const mr::RecoveryReport& r = report.value();
  std::cout << std::format(
      "input:            {}\n"
      "output:           {}{}\n"
      "status:           {}\n"
      "chunks recovered: {}\n"
      "records:          {}\n"
      "bytes kept:       {} of {} ({} discarded)\n"
      "stopped because:  {}\n"
      "elapsed:          {:.3f} s ({:.1f} MB/s)\n",
      input.string(), output.string(), options.dry_run ? " (dry run, not written)" : "",
      r.already_complete ? "already complete" : "recovered", r.chunks_recovered,
      r.records_recovered, r.bytes_kept, r.input_bytes, r.bytes_discarded, r.stop_reason, secs,
      secs > 0 ? static_cast<double>(r.input_bytes) / 1e6 / secs : 0.0);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "mr_recover: fatal: " << e.what() << "\n";
    return 1;
  }
}
