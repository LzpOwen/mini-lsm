#include "mlsm/env.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "mlsm/sequential_file.h"
#include "mlsm/writable_file.h"

namespace mlsm {

namespace {

// Builds an IOError Status that tacks on the current errno description, so
// failures carry the path plus a human-readable reason ("open: No such file").
Status PosixError(const std::string& context) {
  return Status::IOError(context + ": " + std::strerror(errno));
}

class PosixWritableFile : public WritableFile {
 public:
  PosixWritableFile(std::string path, int fd)
      : path_(std::move(path)), fd_(fd) {}

  ~PosixWritableFile() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  Status Append(const Slice& data) override {
    const char* src = data.data();
    size_t left = data.size();
    while (left > 0) {
      ssize_t written = ::write(fd_, src, left);
      if (written < 0) {
        if (errno == EINTR) continue;  // Retry interrupted syscall.
        return PosixError("write " + path_);
      }
      src += written;
      left -= static_cast<size_t>(written);
    }
    return Status::OK();
  }

  // We write straight through to the fd, so there is no user-space buffer to
  // push; Flush is a no-op and durability comes from Sync.
  Status Flush() override { return Status::OK(); }

  Status Sync() override {
    if (::fsync(fd_) != 0) {
      return PosixError("fsync " + path_);
    }
    return Status::OK();
  }

  Status Close() override {
    if (fd_ >= 0) {
      int fd = fd_;
      fd_ = -1;
      if (::close(fd) != 0) {
        return PosixError("close " + path_);
      }
    }
    return Status::OK();
  }

 private:
  const std::string path_;
  int fd_;
};

class PosixSequentialFile : public SequentialFile {
 public:
  PosixSequentialFile(std::string path, int fd)
      : path_(std::move(path)), fd_(fd) {}

  ~PosixSequentialFile() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  Status Read(size_t n, Slice* result, char* scratch) override {
    // Fill up to n bytes; a read() shorter than requested is only EOF once it
    // returns 0, so loop to coalesce partial reads into one block.
    size_t done = 0;
    while (done < n) {
      ssize_t r = ::read(fd_, scratch + done, n - done);
      if (r < 0) {
        if (errno == EINTR) continue;
        return PosixError("read " + path_);
      }
      if (r == 0) break;  // EOF.
      done += static_cast<size_t>(r);
    }
    *result = Slice(scratch, done);
    return Status::OK();
  }

  Status Skip(size_t n) override {
    if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) < 0) {
      return PosixError("lseek " + path_);
    }
    return Status::OK();
  }

 private:
  const std::string path_;
  int fd_;
};

}  // namespace

Status NewWritableFile(const std::string& path, WritableFile** result) {
  *result = nullptr;
  int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) {
    return PosixError("open " + path);
  }
  *result = new PosixWritableFile(path, fd);
  return Status::OK();
}

Status NewAppendableFile(const std::string& path, WritableFile** result) {
  *result = nullptr;
  int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (fd < 0) {
    return PosixError("open " + path);
  }
  *result = new PosixWritableFile(path, fd);
  return Status::OK();
}

Status NewSequentialFile(const std::string& path, SequentialFile** result) {
  *result = nullptr;
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return PosixError("open " + path);
  }
  *result = new PosixSequentialFile(path, fd);
  return Status::OK();
}

bool FileExists(const std::string& path) {
  return ::access(path.c_str(), F_OK) == 0;
}

Status GetFileSize(const std::string& path, uint64_t* size) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    *size = 0;
    return PosixError("stat " + path);
  }
  *size = static_cast<uint64_t>(st.st_size);
  return Status::OK();
}

Status MakeDir(const std::string& path) {
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    return PosixError("mkdir " + path);
  }
  return Status::OK();
}

}  // namespace mlsm
