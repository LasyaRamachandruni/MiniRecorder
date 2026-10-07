// End-to-end crash test: run the real mr_record binary, SIGKILL it while it
// is writing, then recover the .partial file and check that every chunk the
// recorder had reported durable (and possibly more) is back, and that only a
// torn tail smaller than one chunk was discarded.

#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include <mr/file_util.hpp>
#include <mr/reader.hpp>
#include <mr/recovery.hpp>

#include "test_util.hpp"

extern char** environ;  // NOLINT

namespace {

using mr::test::TempDir;

struct Child {
  pid_t pid = -1;
  int progress_read_fd = -1;
};

Child spawn_recorder(const std::filesystem::path& out, const std::string& fsync,
                     int extra_seconds) {
  int fds[2];
  if (::pipe(fds) != 0) {
    ADD_FAILURE() << "pipe failed";
    return {};
  }
  const std::string progress = std::to_string(fds[1]);
  const std::string duration = std::to_string(extra_seconds);
  std::vector<std::string> argv_s{
      MR_RECORD_PATH, "--out",        out.string(), "--duration",    duration,
      "--fsync",      fsync,          "--chunk-kb", "256",           "--camera",
      "320x240@30",   "--rate-scale", "4",          "--progress-fd", progress,
      "--quiet"};
  std::vector<char*> argv;
  for (auto& s : argv_s) {
    argv.push_back(s.data());
  }
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  pid_t pid = -1;
  const int rc = posix_spawn(&pid, MR_RECORD_PATH, &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(fds[1]);
  if (rc != 0) {
    ADD_FAILURE() << "posix_spawn failed: " << rc;
    ::close(fds[0]);
    return {};
  }
  return {pid, fds[0]};
}

// Reads "durable N" lines until at least `target` chunks are durable or the
// timeout expires; returns the last N seen.
std::uint64_t wait_for_durable(int fd, std::uint64_t target, std::chrono::seconds timeout) {
  std::uint64_t last = 0;
  std::string buffer;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (last < target && std::chrono::steady_clock::now() < deadline) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 100) <= 0) {
      continue;
    }
    char chunk[256];
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n <= 0) {
      break;
    }
    buffer.append(chunk, static_cast<std::size_t>(n));
    std::size_t pos = 0;
    while ((pos = buffer.find('\n')) != std::string::npos) {
      const std::string line = buffer.substr(0, pos);
      buffer.erase(0, pos + 1);
      if (line.rfind("durable ", 0) == 0) {
        last = std::stoull(line.substr(8));
      }
    }
  }
  return last;
}

class CrashKill : public ::testing::TestWithParam<std::string> {};

TEST_P(CrashKill, SigkillMidWriteThenRecoverKeepsAllDurableChunks) {
  TempDir dir;
  const auto out = dir.file("killed.mrlog");
  const Child child = spawn_recorder(out, GetParam(), 60);
  ASSERT_GT(child.pid, 0);

  const std::uint64_t durable =
      wait_for_durable(child.progress_read_fd, 25, std::chrono::seconds(30));
  ASSERT_GE(durable, 25U) << "recorder did not make progress";
  ASSERT_EQ(::kill(child.pid, SIGKILL), 0);
  int status = 0;
  ASSERT_EQ(::waitpid(child.pid, &status, 0), child.pid);
  ::close(child.progress_read_fd);
  ASSERT_TRUE(WIFSIGNALED(status));
  ASSERT_EQ(WTERMSIG(status), SIGKILL);

  const auto partial = mr::partial_path(out);
  ASSERT_TRUE(std::filesystem::exists(partial));
  EXPECT_FALSE(std::filesystem::exists(out));
  {
    auto image = mr::MappedFile::open(partial);
    ASSERT_TRUE(image);
    EXPECT_FALSE(mr::Reader::open(partial)) << "a killed recorder must not leave a footer";
  }

  auto report = mr::recover_file(partial, out);
  ASSERT_TRUE(report) << report.error().describe();
  // Everything reported durable before the kill is recovered (more chunks may
  // have been written between the last report and SIGKILL).
  EXPECT_GE(report->chunks_recovered, durable);
  // Only a torn tail can be discarded: less than one maximal chunk.
  EXPECT_LT(report->bytes_discarded,
            mr::format::kChunkHeaderSize + 2U * 256 * 1024 + 320 * 240 + 64);

  auto reader = mr::Reader::open(out);
  ASSERT_TRUE(reader) << reader.error().describe();
  mr::ZstdDecompressor dec;
  std::vector<std::byte> body;
  std::uint64_t records = 0;
  for (const auto& c : reader->footer().chunks) {
    auto h =
        mr::parse_and_verify_chunk(reader->file_bytes().subspan(c.offset, c.total_size), dec, body);
    ASSERT_TRUE(h) << h.error().describe();
    records += h->record_count;
  }
  EXPECT_EQ(records, report->records_recovered);
  EXPECT_GT(records, 100U);
  RecordProperty("fsync", GetParam());
  RecordProperty("durable_reported", std::to_string(durable));
  RecordProperty("chunks_recovered", std::to_string(report->chunks_recovered));
  RecordProperty("bytes_discarded", std::to_string(report->bytes_discarded));
}

INSTANTIATE_TEST_SUITE_P(FsyncPolicies, CrashKill, ::testing::Values("chunk", "interval:50"),
                         [](const auto& param_info) {
                           return param_info.param == "chunk" ? std::string("PerChunk")
                                                              : std::string("Interval");
                         });

}  // namespace
