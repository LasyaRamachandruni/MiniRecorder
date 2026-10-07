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
  Errc code = Errc::kIo;
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

// The error is stored as a plain member next to the value rather than in a
// shared variant: it costs one empty std::string per Result but keeps the
// error path trivially initialized, which also keeps static analysis quiet.
template <typename T>
class [[nodiscard]] Result {
 public:
  // Implicit by design so that `return value;` and `return make_error(...);`
  // both work, mirroring std::expected.
  Result(T value) : value_(std::in_place_index<1>, std::move(value)) {}
  Result(Error error) : error_(std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return value_.index() == 1; }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] T& value() & {
    assert(ok());
    return std::get<1>(value_);
  }
  [[nodiscard]] const T& value() const& {
    assert(ok());
    return std::get<1>(value_);
  }
  [[nodiscard]] T&& value() && {
    assert(ok());
    return std::get<1>(std::move(value_));
  }

  [[nodiscard]] const Error& error() const& {
    assert(!ok());
    return error_;
  }
  [[nodiscard]] Error&& error() && {
    assert(!ok());
    return std::move(error_);
  }

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }

 private:
  std::variant<std::monostate, T> value_;
  Error error_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : ok_(false), error_(std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Error& error() const& {
    assert(!ok());
    return error_;
  }
  [[nodiscard]] Error&& error() && {
    assert(!ok());
    return std::move(error_);
  }

 private:
  bool ok_ = true;
  Error error_;
};

using Status = Result<void>;

[[nodiscard]] inline Status ok_status() {
  return {};
}

}  // namespace mr

// Propagates the error of a Status/Result expression to the caller. A macro
// because it has to `return` from the calling function.
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define MR_RETURN_IF_ERROR(expr)                 \
  do {                                           \
    if (auto mr_status_ = (expr); !mr_status_) { \
      return std::move(mr_status_).error();      \
    }                                            \
  } while (false)
// NOLINTEND(cppcoreguidelines-macro-usage)
