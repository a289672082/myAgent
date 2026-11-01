#include "file_io.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <system_error>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#define NOMINMAX
#include <windows.h>
#endif
namespace myagent::detail {
namespace fs=std::filesystem;
namespace {
std::atomic<std::uint64_t> counter{0};
struct TempCleanup {
  fs::path path;
  ~TempCleanup(){if(!path.empty()){std::error_code ec;fs::remove(path,ec);}}
};
struct SingleWriter {
  fs::path directory;
  explicit SingleWriter(const fs::path& path,bool enabled) {
    if(!enabled)return;
    directory=path;directory+=".lock";
    std::error_code ec;
    if(!fs::create_directory(directory,ec)) {
      directory.clear();
      throw std::runtime_error("Checkpoint locked by another writer (or stale lock)");
    }
  }
  ~SingleWriter() {
    if(!directory.empty()){std::error_code ec;fs::remove(directory,ec);}
  }
};
} // namespace
std::string read_regular_file(const fs::path& path,std::size_t limit) {
#ifndef _WIN32
  const int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
  if(fd<0)throw std::runtime_error("Cannot safely open file");
  struct Guard{int fd;~Guard(){::close(fd);}} guard{fd};
  struct stat info{};
  if(::fstat(fd,&info)!=0 || !S_ISREG(info.st_mode) || info.st_size<0 ||
     static_cast<std::uint64_t>(info.st_size)>limit)
    throw std::runtime_error("File is not regular or exceeds size limit");
  std::string result;
  result.reserve(static_cast<std::size_t>(info.st_size));
  char buffer[8192];
  for(;;) {
    const ssize_t count=::read(fd,buffer,sizeof(buffer));
    if(count<0 && errno==EINTR)continue;
    if(count<0)throw std::runtime_error("File read failed");
    if(count==0)break;
    if(static_cast<std::size_t>(count)>limit-result.size())
      throw std::runtime_error("File grew beyond size limit during read");
    result.append(buffer,static_cast<std::size_t>(count));
  }
  return result;
#else
  HANDLE raw=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,
                         nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  if(raw==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot safely open file");
  struct Guard{HANDLE value;~Guard(){CloseHandle(value);}} guard{raw};
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if(!GetFileInformationByHandleEx(raw,FileAttributeTagInfo,&tag,sizeof(tag)) ||
      (tag.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
    throw std::runtime_error("File is directory or reparse point");
  LARGE_INTEGER info{};
  if(!GetFileSizeEx(raw,&info) || info.QuadPart<0 ||
     static_cast<std::uint64_t>(info.QuadPart)>limit)
    throw std::runtime_error("File exceeds size limit");
  std::string result;result.reserve(static_cast<std::size_t>(info.QuadPart));
  char buffer[8192];
  for(;;) {
    DWORD count=0;
    if(!ReadFile(raw,buffer,sizeof(buffer),&count,nullptr))
      throw std::runtime_error("File read failed");
    if(!count)break;
    if(static_cast<std::size_t>(count)>limit-result.size())
      throw std::runtime_error("File grew beyond size limit during read");
    result.append(buffer,count);
  }
  return result;
#endif
}
void atomic_replace(const fs::path& path,const std::string& data,bool single_writer) {
  if(path.empty())throw std::invalid_argument("Empty output path");
  SingleWriter lock(path,single_writer);
  fs::path temp=path;
  temp+="."+std::to_string(std::chrono::steady_clock::now()
      .time_since_epoch().count())+"-"+std::to_string(counter++)+".tmp";
  TempCleanup cleanup{temp};
#ifndef _WIN32
  const int fd=::open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
  if(fd<0)throw std::runtime_error("Cannot exclusively create temporary file");
  struct Guard{int fd;~Guard(){if(fd>=0)::close(fd);}} guard{fd};
  std::size_t offset=0;
  while(offset<data.size()) {
    const auto n=::write(fd,data.data()+offset,data.size()-offset);
    if(n<0 && errno==EINTR)continue;
    if(n<=0)throw std::runtime_error("Temporary file write failed");
    offset+=static_cast<std::size_t>(n);
  }
  if(::fsync(fd)!=0)throw std::runtime_error("Temporary file fsync failed");
  if(::rename(temp.c_str(),path.c_str())!=0)
    throw std::runtime_error("Atomic file rename failed");
  cleanup.path.clear();
  const auto parent=path.has_parent_path()?path.parent_path():fs::path(".");
  const int dir=::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
  if(dir>=0){::fsync(dir);::close(dir);}
#else
  HANDLE raw=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,
                         CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(raw==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create temporary file");
  std::size_t offset=0;
  try {
    while(offset<data.size()) {
      DWORD written=0;
      const auto size=static_cast<DWORD>(std::min<std::size_t>(data.size()-offset,65536));
      if(!WriteFile(raw,data.data()+offset,size,&written,nullptr)||written==0)
        throw std::runtime_error("Temporary file write failed");
      offset+=written;
    }
    if(!FlushFileBuffers(raw))throw std::runtime_error("Temporary file flush failed");
  }catch(...){CloseHandle(raw);throw;}
  CloseHandle(raw);
  if(!MoveFileExW(temp.c_str(),path.c_str(),
                  MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Atomic file replacement failed");
  cleanup.path.clear();
#endif
}
} // namespace myagent::detail
