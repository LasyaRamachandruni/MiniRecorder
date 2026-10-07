#pragma once

// Thin RAII wrappers over the POSIX file APIs used by the storage engine.

#include <cstdint>
#include <filesystem>

#include <mr/bytes.hpp>
#include <mr/result.hpp>

namespace mr {

class FileDescriptor {
 public:
  FileDescriptor() noexcept = default;
  explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.release()) {}
  FileDescriptor& operator=(FileDescriptor&& other) noexcept;
  ~FileDescriptor() { reset(); }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  int release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }
  void reset() noexcept;
  // close() that reports errors (deferred write-back errors surface here).
  [[nodiscard]] Status close();

 private:
  int fd_ = -1;
};

[[nodiscard]] Result<FileDescriptor> open_file(const std::filesystem::path& path, int flags,
                                               unsigned mode = 0644);
[[nodiscard]] Status write_all(int fd, ByteSpan data);
[[nodiscard]] Status pwrite_all(int fd, ByteSpan data, std::uint64_t offset);
[[nodiscard]] Status pread_exact(int fd, MutableByteSpan out, std::uint64_t offset);
[[nodiscard]] Status sync_data(int fd);
[[nodiscard]] Status sync_directory(const std::filesystem::path& dir);
[[nodiscard]] Status truncate_file(int fd, std::uint64_t size);
[[nodiscard]] Result<std::uint64_t> file_size(int fd);
// rename(2) followed by fsync of the parent directory so the new name is durable.
[[nodiscard]] Status durable_rename(const std::filesystem::path& from,
                                    const std::filesystem::path& to);

// Read-only memory mapping of a whole file.
class MappedFile {
 public:
  [[nodiscard]] static Result<MappedFile> open(const std::filesystem::path& path);

  MappedFile() noexcept = default;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;
  ~MappedFile();

  [[nodiscard]] ByteSpan bytes() const noexcept { return {data_, size_}; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
  MappedFile(const std::byte* data, std::size_t size) noexcept : data_(data), size_(size) {}
  void unmap() noexcept;

  const std::byte* data_ = nullptr;
  std::size_t size_ = 0;
};

[[nodiscard]] inline std::filesystem::path partial_path(const std::filesystem::path& final_path) {
  std::filesystem::path p = final_path;
  p += ".partial";
  return p;
}

}  // namespace mr
