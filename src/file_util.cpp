#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include <mr/file_util.hpp>

namespace mr {

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
  if (this != &other) {
    reset();
    fd_ = other.release();
  }
  return *this;
}

void FileDescriptor::reset() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

Status FileDescriptor::close() {
  if (fd_ < 0) {
    return ok_status();
  }
  const int fd = release();
  // POSIX leaves the descriptor state unspecified after EINTR; on Linux it is
  // always closed, so never retry.
  if (::close(fd) != 0) {
    return io_error("close", errno);
  }
  return ok_status();
}

Result<FileDescriptor> open_file(const std::filesystem::path& path, int flags, unsigned mode) {
  // open(2) is variadic by POSIX definition.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const int fd = ::open(path.c_str(), flags | O_CLOEXEC, static_cast<mode_t>(mode));
  if (fd < 0) {
    return io_error("open " + path.string(), errno);
  }
  return FileDescriptor(fd);
}

Status write_all(int fd, ByteSpan data) {
  while (!data.empty()) {
    const ssize_t n = ::write(fd, data.data(), data.size());
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return io_error("write", errno);
    }
    data = data.subspan(static_cast<std::size_t>(n));
  }
  return ok_status();
}

Status pwrite_all(int fd, ByteSpan data, std::uint64_t offset) {
  while (!data.empty()) {
    const ssize_t n = ::pwrite(fd, data.data(), data.size(), static_cast<off_t>(offset));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return io_error("pwrite", errno);
    }
    data = data.subspan(static_cast<std::size_t>(n));
    offset += static_cast<std::uint64_t>(n);
  }
  return ok_status();
}

Status pread_exact(int fd, MutableByteSpan out, std::uint64_t offset) {
  while (!out.empty()) {
    const ssize_t n = ::pread(fd, out.data(), out.size(), static_cast<off_t>(offset));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return io_error("pread", errno);
    }
    if (n == 0) {
      return make_error(Errc::kTruncated, "unexpected end of file");
    }
    out = out.subspan(static_cast<std::size_t>(n));
    offset += static_cast<std::uint64_t>(n);
  }
  return ok_status();
}

Status sync_data(int fd) {
  if (::fdatasync(fd) != 0) {
    return io_error("fdatasync", errno);
  }
  return ok_status();
}

Status sync_directory(const std::filesystem::path& dir) {
  auto fd = open_file(dir.empty() ? std::filesystem::path(".") : dir, O_RDONLY | O_DIRECTORY);
  if (!fd) {
    return std::move(fd).error();
  }
  if (::fsync(fd->get()) != 0) {
    return io_error("fsync directory", errno);
  }
  return fd->close();
}

Status truncate_file(int fd, std::uint64_t size) {
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
    return io_error("ftruncate", errno);
  }
  return ok_status();
}

Result<std::uint64_t> file_size(int fd) {
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    return io_error("fstat", errno);
  }
  return static_cast<std::uint64_t>(st.st_size);
}

Status durable_rename(const std::filesystem::path& from, const std::filesystem::path& to) {
  if (std::rename(from.c_str(), to.c_str()) != 0) {
    return io_error("rename " + from.string() + " -> " + to.string(), errno);
  }
  return sync_directory(to.parent_path());
}

// ---------------------------------------------------------------- MappedFile

Result<MappedFile> MappedFile::open(const std::filesystem::path& path) {
  auto fd = open_file(path, O_RDONLY);
  if (!fd) {
    return std::move(fd).error();
  }
  auto size = file_size(fd->get());
  if (!size) {
    return std::move(size).error();
  }
  if (size.value() == 0) {
    return MappedFile();
  }
  void* addr = ::mmap(nullptr, size.value(), PROT_READ, MAP_PRIVATE, fd->get(), 0);
  if (addr == MAP_FAILED) {
    return io_error("mmap " + path.string(), errno);
  }
  // Sequential scans dominate (iteration, recovery); ask for read-ahead.
  ::madvise(addr, size.value(), MADV_SEQUENTIAL);
  return MappedFile(static_cast<const std::byte*>(addr), size.value());
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    unmap();
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

MappedFile::~MappedFile() {
  unmap();
}

void MappedFile::unmap() noexcept {
  if (data_ != nullptr) {
    // munmap takes a non-const pointer; the mapping itself is read-only.
    ::munmap(const_cast<std::byte*>(data_), size_);  // NOLINT(*-const-cast)
    data_ = nullptr;
    size_ = 0;
  }
}

}  // namespace mr
