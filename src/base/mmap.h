#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace base {

class MMapFile {
public:
  struct Range {
    void* data = nullptr;

    void* begin() const { return data; }
  };

  struct Options {
    bool writable = false;
    bool prefault = false;
    bool shared   = false;

    Options& setPrefault(bool value) {
      prefault = value;
      return *this;
    }

    Options& setShared(bool value) {
      shared = value;
      return *this;
    }
  };

  static Options writable() {
    Options options;
    options.writable = true;
    return options;
  }

  MMapFile() = default;

  explicit MMapFile(const std::string& path) {
    Options options;
    Open(path, 0, 0, options, true);
  }

  MMapFile(const std::string& path,
           size_t offset,
           size_t length,
           const Options& options) {
    Open(path, offset, length, options, false);
  }

  MMapFile(const MMapFile&)            = delete;
  MMapFile& operator=(const MMapFile&) = delete;

  MMapFile(MMapFile&& other) noexcept { MoveFrom(std::move(other)); }

  MMapFile& operator=(MMapFile&& other) noexcept {
    if (this != &other) {
      Close();
      MoveFrom(std::move(other));
    }
    return *this;
  }

  ~MMapFile() { Close(); }

  Range writableRange() const { return Range{data_}; }

  void* data() const { return data_; }

  size_t size() const { return size_; }

private:
  void Open(const std::string& path,
            size_t offset,
            size_t length,
            const Options& options,
            bool read_only_existing) {
    path_ = path;
    int open_flags = options.writable ? O_RDWR | O_CREAT : O_RDONLY;
    fd_            = ::open(path.c_str(), open_flags, 0666);
    if (fd_ < 0) {
      throw std::runtime_error("open failed: " + std::string(std::strerror(errno)));
    }

    if (read_only_existing) {
      struct stat st {};
      if (fstat(fd_, &st) != 0) {
        throw std::runtime_error("fstat failed: " + std::string(std::strerror(errno)));
      }
      length = static_cast<size_t>(st.st_size);
    } else if (options.writable && length > 0) {
      const off_t required_size = static_cast<off_t>(offset + length);
      if (ftruncate(fd_, required_size) != 0) {
        throw std::runtime_error("ftruncate failed: " +
                                 std::string(std::strerror(errno)));
      }
    }

    size_ = length;
    if (size_ == 0) {
      return;
    }

    int prot  = PROT_READ | (options.writable ? PROT_WRITE : 0);
    int flags = (options.shared ? MAP_SHARED : MAP_PRIVATE);
#ifdef MAP_POPULATE
    if (options.prefault) {
      flags |= MAP_POPULATE;
    }
#endif

    data_ = ::mmap(nullptr, size_, prot, flags, fd_, static_cast<off_t>(offset));
    if (data_ == MAP_FAILED) {
      data_ = nullptr;
      throw std::runtime_error("mmap failed: " + std::string(std::strerror(errno)));
    }
  }

  void Close() {
    if (data_ != nullptr && size_ > 0) {
      ::munmap(data_, size_);
    }
    data_ = nullptr;
    size_ = 0;
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  void MoveFrom(MMapFile&& other) {
    path_       = std::move(other.path_);
    fd_         = other.fd_;
    data_       = other.data_;
    size_       = other.size_;
    other.fd_   = -1;
    other.data_ = nullptr;
    other.size_ = 0;
  }

  std::string path_;
  int fd_      = -1;
  void* data_  = nullptr;
  size_t size_ = 0;
};

using MemoryMapping = MMapFile;

} // namespace base
