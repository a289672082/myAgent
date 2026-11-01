#include "workspace_io.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <vector>
#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <dirent.h>
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
constexpr std::size_t MaxFile=1024*1024;
std::vector<fs::path> components(const std::string& relative) {
  const fs::path path(relative);
  if(path.empty() || path.is_absolute() || path.has_root_name())
    throw std::invalid_argument("Workspace path must be relative");
  std::vector<fs::path> names;
  for(const auto& name:path) {
    if(name.empty() || name=="." || name=="..")
      throw std::invalid_argument("Workspace traversal denied");
    names.push_back(name);
  }
  if(names.empty())throw std::invalid_argument("Empty workspace path");
  return names;
}
#ifndef _WIN32
struct FD {
  int fd=-1;
  explicit FD(int handle=-1):fd(handle) {}
  ~FD(){if(fd>=0)::close(fd);}
  FD(const FD&)=delete;
  FD& operator=(const FD&)=delete;
  FD(FD&& other) noexcept:fd(other.fd){other.fd=-1;}
  FD& operator=(FD&& other) noexcept {
    if(this!=&other){if(fd>=0)::close(fd);fd=other.fd;other.fd=-1;}return *this;
  }
};
void require_fd(int fd,const char* reason) {
  if(fd<0)throw std::runtime_error(std::string(reason)+": "+std::strerror(errno));
}
FD directory_fd(const fs::path& root,const std::vector<fs::path>& parts,
                std::size_t count) {
  FD dir(::open(root.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
  require_fd(dir.fd,"Cannot open workspace root");
  for(std::size_t i=0;i<count;++i) {
    FD next(::openat(dir.fd,parts[i].c_str(),
                O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    require_fd(next.fd,"Workspace directory traversal denied");
    dir=std::move(next);
  }
  return dir;
}
#else
// Windows uses path verification and exclusive CreateFileW for temporary writes.
// An adversary able to replace parent directories concurrently still needs OS
// ACL/container isolation; this is not a multi-tenant filesystem sandbox.
fs::path checked_path(const fs::path& root,const std::vector<fs::path>& names) {
  auto path=root;
  for(const auto& part:names) {
    path/=part;
    if(fs::is_symlink(fs::symlink_status(path)))
      throw std::runtime_error("Workspace reparse point denied");
  }
  const auto canonical=fs::weakly_canonical(path);
  const auto root_path=fs::weakly_canonical(root);
  auto ri=root_path.begin(), pi=canonical.begin();
  for(;ri!=root_path.end();++ri,++pi)
    if(pi==canonical.end()||*ri!=*pi)
      throw std::runtime_error("Workspace path escaped root");
  return path;
}
struct Handle {
  HANDLE raw=INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE h):raw(h){}
  ~Handle(){if(raw!=INVALID_HANDLE_VALUE)CloseHandle(raw);}
};
#endif
std::atomic<std::uint64_t> temp_counter{0};
}
std::string workspace_read(const fs::path& root,const std::string& relative) {
  const auto parts=components(relative);
#ifndef _WIN32
  auto dir=directory_fd(root,parts,parts.size()-1);
  FD file(::openat(dir.fd,parts.back().c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK));
  require_fd(file.fd,"Workspace read denied");
  struct stat statbuf{};
  if(::fstat(file.fd,&statbuf)!=0 || !S_ISREG(statbuf.st_mode) ||
     statbuf.st_size<0 || static_cast<std::uint64_t>(statbuf.st_size)>MaxFile)
    throw std::runtime_error("Workspace file missing or exceeds 1 MiB");
  std::string data;data.reserve(static_cast<std::size_t>(statbuf.st_size));
  char buffer[8192];ssize_t n;
  while((n=::read(file.fd,buffer,sizeof(buffer)))>0) {
    if(data.size()+static_cast<std::size_t>(n)>MaxFile)
      throw std::runtime_error("Workspace read exceeded 1 MiB");
    data.append(buffer,static_cast<std::size_t>(n));
  }
  if(n<0)throw std::runtime_error("Workspace read error");
  return data;
#else
  const auto path=checked_path(root,parts);
  Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
                          OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(file.raw==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot open workspace file");
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if(!GetFileInformationByHandleEx(file.raw,FileAttributeTagInfo,&tag,sizeof(tag)) ||
     (tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
     (tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY))
    throw std::runtime_error("Workspace reparse/directory denied");
  LARGE_INTEGER size{};
  if(!GetFileSizeEx(file.raw,&size) || size.QuadPart<0 ||
     size.QuadPart>static_cast<LONGLONG>(MaxFile))
    throw std::runtime_error("Workspace file exceeds 1 MiB");
  std::string result(static_cast<std::size_t>(size.QuadPart),'\0');
  DWORD read=0;
  if(!ReadFile(file.raw,result.data(),static_cast<DWORD>(result.size()),&read,nullptr))
    throw std::runtime_error("Workspace file read failed");
  result.resize(read);
  return result;
#endif
}
std::string workspace_list(const fs::path& root,const std::string& relative) {
  const auto parts=components(relative);
#ifndef _WIN32
  auto dir=directory_fd(root,parts,parts.size());
  DIR* raw=::fdopendir(::dup(dir.fd));
  if(!raw)throw std::runtime_error("Cannot open workspace directory");
  std::string names;unsigned count=0;
  while(auto* entry=::readdir(raw)) {
    const std::string name(entry->d_name);
    if(name=="."||name=="..")continue;
    if(++count>100){names+="(truncated)\n";break;}
    names+=name+"\n";
  }
  ::closedir(raw);
  return names;
#else
  const auto path=checked_path(root,parts);
  if(!fs::is_directory(path))throw std::runtime_error("Directory does not exist");
  std::string names;unsigned count=0;
  for(const auto& entry:fs::directory_iterator(path)) {
    if(++count>100){names+="(truncated)\n";break;}
    names+=entry.path().filename().string()+"\n";
  }
  return names;
#endif
}
bool workspace_write(const fs::path& root,const std::string& relative,
                     const std::string& content) {
  if(content.size()>MaxFile)throw std::runtime_error("Workspace write exceeds 1 MiB");
  const auto parts=components(relative);
  const auto nonce=std::to_string(temp_counter.fetch_add(1))+"-"+
    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string tempname="myagent-"+nonce+".tmp";
#ifndef _WIN32
  auto dir=directory_fd(root,parts,parts.size()-1);
  struct stat existing{};
  if(::fstatat(dir.fd,parts.back().c_str(),&existing,AT_SYMLINK_NOFOLLOW)==0 &&
     (!S_ISREG(existing.st_mode) || S_ISLNK(existing.st_mode)))
    throw std::runtime_error("Workspace write target is not a regular file");
  FD temp(::openat(dir.fd,tempname.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
  require_fd(temp.fd,"Cannot create exclusive workspace temp file");
  try {
    std::size_t done=0;
    while(done<content.size()) {
      const auto n=::write(temp.fd,content.data()+done,content.size()-done);
      if(n<=0)throw std::runtime_error("Workspace write failed");
      done+=static_cast<std::size_t>(n);
    }
    if(::fsync(temp.fd)!=0)throw std::runtime_error("Workspace fsync failed");
    if(::renameat(dir.fd,tempname.c_str(),dir.fd,parts.back().c_str())!=0)
      throw std::runtime_error("Workspace atomic rename failed");
  }catch(...){::unlinkat(dir.fd,tempname.c_str(),0);throw;}
#else
  const auto path=checked_path(root,parts);
  if(!fs::is_directory(path.parent_path()))
    throw std::runtime_error("Workspace destination missing");
  const auto temp=path.parent_path()/tempname;
  try {
    Handle file(CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,
                            CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(file.raw==INVALID_HANDLE_VALUE)
      throw std::runtime_error("Cannot create exclusive workspace temporary file");
    DWORD written=0;
    if(!WriteFile(file.raw,content.data(),static_cast<DWORD>(content.size()),&written,nullptr) ||
       written!=content.size() || !FlushFileBuffers(file.raw))
      throw std::runtime_error("Workspace temporary file write failed");
    if(fs::is_symlink(fs::symlink_status(path)))
      throw std::runtime_error("Workspace target became symbolic link");
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("Workspace atomic replacement failed");
  }catch(...){std::error_code ec;fs::remove(temp,ec);throw;}
#endif
  return true;
}
} // namespace myagent::detail
