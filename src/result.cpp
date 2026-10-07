#include <cstring>

#include <mr/result.hpp>

namespace mr {

Error io_error(std::string_view what, int errno_value) {
  std::string message{what};
  message += ": ";
  message += std::strerror(errno_value);  // NOLINT(concurrency-mt-unsafe): message text only
  return Error{Errc::kIo, std::move(message)};
}

}  // namespace mr
