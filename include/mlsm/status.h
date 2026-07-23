#pragma once

#include <string>
#include <utility>

namespace mlsm {

// Simple Status type modeled after LevelDB/RocksDB.
// Ok is the common case and carries no message.
class Status {
 public:
  enum Code {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kIOError = 3,
    kInvalidArgument = 4,
  };

  Status() : code_(kOk) {}
  Status(Code code, std::string msg) : code_(code), msg_(std::move(msg)) {}

  static Status OK() { return Status(); }
  static Status NotFound(std::string msg = "") {
    return Status(kNotFound, std::move(msg));
  }
  static Status Corruption(std::string msg = "") {
    return Status(kCorruption, std::move(msg));
  }
  static Status IOError(std::string msg = "") {
    return Status(kIOError, std::move(msg));
  }
  static Status InvalidArgument(std::string msg = "") {
    return Status(kInvalidArgument, std::move(msg));
  }

  bool ok() const { return code_ == kOk; }
  bool IsNotFound() const { return code_ == kNotFound; }
  bool IsCorruption() const { return code_ == kCorruption; }
  bool IsIOError() const { return code_ == kIOError; }

  Code code() const { return code_; }
  const std::string& message() const { return msg_; }

  std::string ToString() const {
    switch (code_) {
      case kOk:
        return "OK";
      case kNotFound:
        return "NotFound: " + msg_;
      case kCorruption:
        return "Corruption: " + msg_;
      case kIOError:
        return "IOError: " + msg_;
      case kInvalidArgument:
        return "InvalidArgument: " + msg_;
    }
    return "Unknown";
  }

 private:
  Code code_;
  std::string msg_;
};

}  // namespace mlsm