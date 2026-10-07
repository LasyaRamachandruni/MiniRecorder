#pragma once

// Error handling convention for the whole project:
//
//  * Recoverable failures (I/O errors, corrupt or truncated data, invalid
//    configuration) are reported through Result<T> / Status. They never throw.
//  * Resource exhaustion raised by the standard library (std::bad_alloc,
//    std::system_error from thread creation) is not caught inside the library;
//    the command line tools catch it at main() and exit non-zero.
//  * Broken invariants (programming errors) are checked with assert().
//
// Result<T> is a deliberately small subset of C++23 std::expected so the code
// base stays on C++20.

#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace mr {

enum class Errc {
  kIo,
  kCorrupt,
  kTruncated,
  kInvalidArgument,
  kNotFound,
  kUnsupported,
  kCompression,
  kClosed,
};

[[nodiscard]] constexpr std::string_view to_string(Errc code) noexcept {
  switch (code) {
    case Errc::kIo:
      return "io";
    case Errc::kCorrupt:
      return "corrupt";
    case Errc::kTruncated:
      return "truncated";
    case Errc::kInvalidArgument:
      return "invalid-argument";
    case Errc::kNotFound:
      return "not-found";
    case Errc::kUnsupported:
      return "unsupported";
    case Errc::kCompression:
      return "compression";
    case Errc::kClosed:
      return "closed";
  }
  return "unknown";
}

struct Error {
  Errc code;
  std::string message;

  [[nodiscard]] std::string describe() const {
    std::string out{to_string(code)};
    out += ": ";
    out += message;
    return out;
  }
};

[[nodiscard]] inline Error make_error(Errc code, std::string message) {
  return Error{code, std::move(message)};
}

// Builds an kIo error that includes strerror(errno_value).
[[nodiscard]] Error io_error(std::string_view what, int errno_value);

template <typename T>
class [[nodiscard]] Result {
 public:
  // Implicit by design so that `return value;` and `return make_error(...);`
  // both work, mirroring std::expected.
  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}  // NOLINT
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}  // NOLINT

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] T& value() & {
    assert(ok());
    return std::get<0>(storage_);
  }
  [[nodiscard]] const T& value() const& {
    assert(ok());
    return std::get<0>(storage_);
  }
  [[nodiscard]] T&& value() && {
    assert(ok());
    return std::get<0>(std::move(storage_));
  }

  [[nodiscard]] const Error& error() const& {
    assert(!ok());
    return std::get<1>(storage_);
  }
  [[nodiscard]] Error&& error() && {
    assert(!ok());
    return std::get<1>(std::move(storage_));
  }

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }

 private:
  std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}  // NOLINT

  [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Error& error() const& {
    assert(!ok());
    return *error_;
  }
  [[nodiscard]] Error&& error() && {
    assert(!ok());
    return std::move(*error_);
  }

 private:
  std::optional<Error> error_;
};

using Status = Result<void>;

[[nodiscard]] inline Status ok_status() { return {}; }

}  // namespace mr

// Propagates the error of a Status/Result expression to the caller.
#define MR_RETURN_IF_ERROR(expr)              \
  do {                                        \
    if (auto mr_status_ = (expr); !mr_status_) { \
      return std::move(mr_status_).error();   \
    }                                         \
  } while (false)
