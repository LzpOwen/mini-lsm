#pragma once

#include <cstring>
#include <string>

namespace mlsm {

// A lightweight, non-owning view over a byte range.
// The caller must ensure the underlying storage outlives the Slice.
class Slice {
 public:
  Slice() : data_(""), size_(0) {}
  Slice(const char* d, size_t n) : data_(d), size_(n) {}
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}
  Slice(const char* s) : data_(s), size_(std::strlen(s)) {}

  const char* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  char operator[](size_t n) const { return data_[n]; }

  std::string ToString() const { return std::string(data_, size_); }

  bool operator==(const Slice& other) const {
    return size_ == other.size_ &&
           std::memcmp(data_, other.data_, size_) == 0;
  }
  bool operator!=(const Slice& other) const { return !(*this == other); }

 private:
  const char* data_;
  size_t size_;
};

}  // namespace mlsm