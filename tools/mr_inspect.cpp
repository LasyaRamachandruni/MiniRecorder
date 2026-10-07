// mr_inspect: summary, per-stream statistics and quality events of a log.

#include <chrono>
#include <exception>
#include <format>
#include <iostream>
#include <string>

#include <mr/events.hpp>
#include <mr/reader.hpp>

#include "cli.hpp"

namespace {

std::string stream_name(const mr::Reader& r, mr::StreamId id) {
  const auto* info = r.stream_info(id);
  return info != nullptr ? info->name : std::format("#{}", id);
}

double mb(std::uint64_t bytes) { return static_cast<double>(bytes) / 1e6; }

std::string format_utc(std::int64_t unix_ns) {
  const auto tp = std::chrono::sys_time<std::chrono::nanoseconds>(std::chrono::nanoseconds(unix_ns));
  return std::format("{:%Y-%m-%d %H:%M:%S} UTC", std::chrono::floor<std::chrono::seconds>(tp));
}

void print_summary(const mr::Reader& r, const std::filesystem::path& path) {
  const auto& f = r.footer();
  std::uint64_t records = 0;
  std::uint64_t uncompressed = 0;
  std::uint64_t stored = 0;
  for (const auto& c : f.chunks) {
    records += c.record_count;
    uncompressed += c.uncompressed_size;
    stored += c.total_size;
  }
  const double span_s = static_cast<double>(f.end_time_ns - f.start_time_ns) / 1e9;
  std::cout << std::format(
      "file        {}\n"
      "size        {:.2f} MB, format v{}, created {}{}\n"
      "time span   {:.3f} s\n"
      "chunks      {} ({} records)\n"
      "data        {:.2f} MB stored, {:.2f} MB uncompressed, ratio {:.2f}\n\n",
      path.string(), mb(r.file_bytes().size()), mr::format::kVersion, format_utc(r.header().created_unix_ns),
      (f.flags & mr::format::footer_flags::kRecovered) != 0 ? ", RECOVERED after crash" : "", span_s,
      f.chunks.size(), records, mb(stored), mb(uncompressed),
      stored > 0 ? static_cast<double>(uncompressed) / static_cast<double>(stored) : 0.0);

  std::cout << std::format("{:>3} {:<14} {:<10} {:>8} {:>9} {:>9} {:>10} {:>9} {:>6} {:>8} {:>5} {:>7} {:>5} {:>5}\n",
                           "id", "stream", "kind", "nominal", "measured", "records", "payloadMB", "storedMB",
                           "ratio", "dropped", "gaps", "regress", "rate", "stale");
  for (const auto& s : f.streams) {
    const auto* info = r.stream_info(s.id);
    const double dur = static_cast<double>(s.last_time_ns - s.first_time_ns) / 1e9;
    const double measured = s.records > 1 && dur > 0 ? static_cast<double>(s.records - 1) / dur : 0.0;
    std::cout << std::format(
        "{:>3} {:<14} {:<10} {:>8.2f} {:>9.2f} {:>9} {:>10.3f} {:>9.3f} {:>6.2f} {:>8} {:>5} {:>7} {:>5} {:>5}\n",
        s.id, stream_name(r, s.id), info != nullptr ? mr::to_string(info->kind) : "?",
        info != nullptr ? info->nominal_rate_hz : 0.0, measured, s.records, mb(s.payload_bytes),
        mb(s.stored_bytes),
        s.stored_bytes > 0 ? static_cast<double>(s.payload_bytes) / static_cast<double>(s.stored_bytes) : 0.0,
        s.dropped, s.seq_gaps, s.ts_regressions, s.rate_events, s.stale_events);
  }
  std::cout << "\n(ratio = payload bytes / chunk bytes on disk; chunk bytes include record headers)\n";
}

int print_events(const mr::Reader& r, std::int64_t limit) {
  mr::ReadOptions opts;
  opts.stream_mask = std::uint64_t{1} << mr::kEventStreamId;
  auto it = r.messages(opts);
  mr::Message m;
  std::int64_t shown = 0;
  std::int64_t total = 0;
  std::cout << "\nquality / trigger events:\n";
  for (;;) {
    auto more = it.next(m);
    if (!more) {
      std::cerr << "mr_inspect: " << more.error().describe() << "\n";
      return 1;
    }
    if (!more.value()) {
      break;
    }
    ++total;
    const auto e = mr::decode_event(m.payload);
    if (!e || (limit >= 0 && shown >= limit)) {
      continue;
    }
    ++shown;
    std::cout << std::format("  {:>10.3f} s  {:<14} {:<20} {}\n",
                             static_cast<double>(e->timestamp_ns - r.footer().start_time_ns) / 1e9,
                             stream_name(r, e->stream), mr::to_string(e->kind), mr::describe(*e));
  }
  if (total == 0) {
    std::cout << "  none\n";
  } else if (shown < total) {
    std::cout << std::format("  ... {} more (use --limit -1 for all)\n", total - shown);
  }
  return 0;
}

int verify(const mr::Reader& r) {
  const auto t0 = std::chrono::steady_clock::now();
  mr::ZstdDecompressor dec;
  std::vector<std::byte> body;
  std::uint64_t bytes = 0;
  for (std::size_t i = 0; i < r.footer().chunks.size(); ++i) {
    const auto& c = r.footer().chunks[i];
    auto h = mr::parse_and_verify_chunk(r.file_bytes().subspan(c.offset, c.total_size), dec, body);
    if (!h) {
      std::cout << std::format("verify: chunk {} at offset {} FAILED: {}\n", i, c.offset, h.error().describe());
      return 1;
    }
    bytes += c.total_size;
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::cout << std::format("\nverify: all {} chunks OK ({:.2f} MB in {:.3f} s)\n", r.footer().chunks.size(),
                           mb(bytes), secs);
  return 0;
}

int run(int argc, const char* const* argv) {
  mr::cli::ArgParser args("mr_inspect", "Prints a summary, per-stream statistics and quality events of a log.");
  args.positional("file", "log file (*.mrlog)");
  args.flag("events", "list quality and trigger events");
  args.option("limit", "maximum events to list (-1 = all)", "50");
  args.flag("verify", "decompress every chunk and validate CRCs and records");
  if (auto s = args.parse(argc, argv); !s) {
    std::cerr << "mr_inspect: " << s.error().message << "\n\n" << args.usage();
    return 2;
  }
  if (args.help_requested()) {
    std::cout << args.usage();
    return 0;
  }
  const std::filesystem::path path = args.get_positional(0).value_or(std::string{});
  auto reader = mr::Reader::open(path);
  if (!reader) {
    std::cerr << "mr_inspect: " << path.string() << ": " << reader.error().describe() << "\n";
    return 1;
  }
  print_summary(*reader, path);
  int rc = 0;
  if (args.is_set("events")) {
    const auto limit = args.get_int("limit");
    if (!limit) {
      std::cerr << "mr_inspect: " << limit.error().message << "\n";
      return 2;
    }
    rc |= print_events(*reader, *limit);
  }
  if (args.is_set("verify")) {
    rc |= verify(*reader);
  }
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "mr_inspect: fatal: " << e.what() << "\n";
    return 1;
  }
}
