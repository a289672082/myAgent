#include "workspace_io.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>
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
#ifdef _WIN32
    // Reject alternate data streams (file:stream) and drive-like segments.
    if(name.native().find(L':')!=std::wstring::npos)
      throw std::invalid_argument("Workspace alternate data streams denied");
#endif
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
  // Anchor at the filesystem root and open EVERY ancestor through the
  // previously verified directory handle. O_NOFOLLOW on a full pathname
  // alone protects only its last component.
  if(root.empty())throw std::invalid_argument("Workspace root cannot be empty");
  const auto absolute=fs::absolute(root).lexically_normal();
  FD dir(::open(absolute.root_path().c_str(),
                O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
  require_fd(dir.fd,"Cannot open filesystem root");
  for(const auto& component:absolute.relative_path()) {
    if(component=="." || component=="..")
      throw std::invalid_argument("Invalid workspace root component");
    FD next(::openat(dir.fd,component.c_str(),
                  O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    require_fd(next.fd,"Workspace root ancestor denied");
    dir=std::move(next);
  }
  for(std::size_t i=0;i<count;++i) {
    FD next(::openat(dir.fd,parts[i].c_str(),
                O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    require_fd(next.fd,"Workspace directory traversal denied");
    dir=std::move(next);
  }
  return dir;
}
#else
// Windows: pin the workspace root and each traversed directory with open
// handles that do NOT share WRITE or DELETE. A path-only symlink check followed
// by CreateFileW leaves a rename/reparse race; held handles make ancestors
// nonreplaceable until the final operation completes. The caller must keep the
// returned CheckedPath alive throughout read/list/write.
struct Handle {
  HANDLE raw=INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):raw(h){}
  ~Handle(){if(raw!=INVALID_HANDLE_VALUE)CloseHandle(raw);}
  Handle(const Handle&)=delete;
  Handle& operator=(const Handle&)=delete;
  Handle(Handle&& other) noexcept:raw(other.raw){other.raw=INVALID_HANDLE_VALUE;}
  Handle& operator=(Handle&& other) noexcept {
    if(this!=&other) {
      if(raw!=INVALID_HANDLE_VALUE)CloseHandle(raw);
      raw=other.raw;other.raw=INVALID_HANDLE_VALUE;
    }
    return *this;
  }
};
struct CheckedPath {
  fs::path path;
  std::vector<Handle> held_directories;
};
void hold_directory(const fs::path& path,std::vector<Handle>& handles) {
  Handle dir(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,
                         OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(dir.raw==INVALID_HANDLE_VALUE)
    throw std::runtime_error("Workspace directory open denied");
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if(!GetFileInformationByHandleEx(dir.raw,FileAttributeTagInfo,&tag,sizeof(tag)) ||
     !(tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY) ||
     (tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
    throw std::runtime_error("Workspace directory reparse point denied");
  handles.push_back(std::move(dir));
}
CheckedPath checked_path(const fs::path& root,const std::vector<fs::path>& names,
                         bool directory=false) {
  CheckedPath checked;
  // Do NOT recanonicalize a saved root: canonical() follows a junction
  // that may have been swapped in after worker registration. Pin every
  // ancestor from the volume root and reject *all* reparse components.
  const auto absolute=fs::absolute(root).lexically_normal();
  checked.path=absolute.root_path();
  if(checked.path.empty())throw std::runtime_error("Invalid workspace root");
  hold_directory(checked.path,checked.held_directories);
  for(const auto& component:absolute.relative_path()) {
    checked.path/=component;
    hold_directory(checked.path,checked.held_directories);
  }
  const auto dirs=directory?names.size():names.size()-1;
  for(std::size_t i=0;i<dirs;++i) {
    checked.path/=names[i];
    hold_directory(checked.path,checked.held_directories);
  }
  if(!directory)checked.path/=names.back();
  return checked;
}
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
  auto checked=checked_path(root,parts);
  const auto& path=checked.path;
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
  std::size_t offset=0;
  while(offset<result.size()) {
    DWORD read=0;
    if(!ReadFile(file.raw,result.data()+offset,
                 static_cast<DWORD>(result.size()-offset),&read,nullptr))
      throw std::runtime_error("Workspace file read failed");
    if(read==0)throw std::runtime_error("Workspace file truncated during read");
    offset+=read;
  }
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
  auto checked=checked_path(root,parts,true);
  const auto& path=checked.path;
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
  auto checked=checked_path(root,parts);
  const auto& path=checked.path;
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
    // Windows 在临时文件句柄仍由本进程独占时可能拒绝 MoveFileExW。
    // 先 FlushFileBuffers，再关闭临时文件，随后执行同目录原子替换。
    CloseHandle(file.raw);
    file.raw=INVALID_HANDLE_VALUE;
    // GetFileAttributesW catches Windows junctions and other reparse tags,
    // not only filesystem::is_symlink. Reject directories as write targets.
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES &&
       (attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))
      throw std::runtime_error("Workspace write target is not a regular file");
    if(attributes==INVALID_FILE_ATTRIBUTES &&
       GetLastError()!=ERROR_FILE_NOT_FOUND)
      throw std::runtime_error("Workspace write target attribute check failed");
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("Workspace atomic replacement failed");
  }catch(...){std::error_code ec;fs::remove(temp,ec);throw;}
#endif
  return true;
}
} // namespace myagent::detail
