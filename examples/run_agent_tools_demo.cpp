// 智能体常用工具合集：一个 MCP stdio 服务端注册全部 10 组真实工具，无需命令行参数。
// 安全默认值：只允许访问 ./demo_workspace；拒绝符号链接、路径穿越和超大文件。
// 注意：这是可运行的教学 Demo，不是针对恶意并发文件系统操作加固的沙箱。
#include "mymcp/mcp.hpp"
#include <httplib.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using mymcp::Json;
using mymcp::json::object;
using mymcp::json::array;

namespace {
constexpr std::uintmax_t kMaxFile = 1024 * 1024;  // 单文件读写最多 1 MiB
constexpr std::size_t kMaxOutput = 48 * 1024;      // 单次工具响应/进程输出大小限制
constexpr std::size_t kMaxResults = 100;           // 搜索结果最多 100 条
constexpr std::size_t kMaxScanned = 10000;         // 搜索遍历上限
constexpr int kMaxDepth = 12;

[[noreturn]] void bad(const std::string& msg) { throw std::runtime_error(msg); }
std::string str(const Json& p, std::string_view key, std::string fallback = "") {
  if (!p.contains(std::string(key))) return fallback;
  if (!p.at(std::string(key)).is_string()) bad("参数必须为字符串: " + std::string(key));
  return p.at(std::string(key)).as_string();
}
bool bool_arg(const Json& p, std::string_view key, bool fallback = false) {
  if (!p.contains(std::string(key))) return fallback;
  if (!p.at(std::string(key)).is_bool()) bad("参数必须为布尔值: " + std::string(key));
  return p.at(std::string(key)).as_bool();
}
void bounded(std::string_view s, std::size_t max, std::string_view what) {
  if (s.size() > max) bad(std::string(what) + " 超出长度限制");
}
Json result(Json data) { return mymcp::detail::make_tool_result(data); }

// 工具输入的 JSON Schema：仅接受已声明参数，避免智能体无意传入不受支持选项。
Json schema(std::initializer_list<std::pair<std::string, std::string>> fields,
            std::initializer_list<std::string> required = {}) {
  object props;
  for (auto& [key, kind] : fields) props[key] = object{{"type", kind}};
  array req;
  for (auto& key : required) req.push_back(key);
  return object{{"type", "object"}, {"properties", props}, {"required", req},
                {"additionalProperties", false}};
}

// 在工作区内运行：先逐层拒绝符号链接，再检查规范化路径是否仍在根目录下。
// 注意路径检查后到打开文件之间存在 TOCTOU 窗口，不能用于不可信多租户服务。
struct Workspace {
  fs::path root;
  Workspace() {
    root = fs::absolute(fs::current_path() / "demo_workspace").lexically_normal();
    if (fs::is_symlink(fs::symlink_status(root))) bad("工作区不能是符号链接");
    fs::create_directories(root);
    root = fs::canonical(root);
  }
  fs::path path(const std::string& relative, bool must_exist = false) const {
    bounded(relative, 512, "路径");
    if (relative.empty()) bad("路径不能为空");
    fs::path rel = fs::u8path(relative);
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) bad("不允许绝对路径");
    for (const auto& item : rel) if (item == "..") bad("禁止路径穿越 ..");
    const fs::path dest = (root / rel).lexically_normal();
    auto it = dest.begin(), begin = root.begin();
    for (; begin != root.end(); ++begin, ++it)
      if (it == dest.end() || *it != *begin) bad("路径不在工作区内");
    fs::path current = root;
    for (const auto& part : rel) {
      if (part == "." || part.empty()) continue;
      current /= part;
      auto st = fs::symlink_status(current);
      if (fs::is_symlink(st)) bad("禁止访问符号链接");
    }
    if (must_exist && !fs::exists(dest)) bad("文件或目录不存在: " + relative);
    return dest;
  }
  std::string relative(const fs::path& file) const { return file.lexically_relative(root).generic_string(); }
};

std::string read_limited(const fs::path& f) {
  if (!fs::is_regular_file(f)) bad("不是普通文件");
  if (fs::file_size(f) > kMaxFile) bad("文件超过 1 MiB 限制");
  std::ifstream in(f, std::ios::binary);
  if (!in) bad("无法读取文件");
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (content.size() > kMaxFile) bad("文件超过 1 MiB 限制");
  if (content.find('\0') != std::string::npos) bad("此示例仅处理文本文件");
  return content;
}
void write_limited(const fs::path& f, const std::string& content, bool append = false) {
  bounded(content, kMaxFile, "写入内容");
  if (content.find('\0') != std::string::npos) bad("禁止写入二进制 NUL");
  if (fs::exists(f) && !fs::is_regular_file(f)) bad("目标不是普通文件");
  auto previous = fs::exists(f) ? fs::file_size(f) : 0;
  if (append && previous + content.size() > kMaxFile) bad("追加后文件超过 1 MiB");
  if (!fs::is_directory(f.parent_path())) bad("父目录不存在");
  std::ofstream out(f, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
  if (!out) bad("无法写入文件");
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!out) bad("写入失败");
}

// 遍历时跳过符号链接，且每个入口再次经工作区路径校验；上限阻止无界搜索。
template<class Fn>
void walk(const Workspace& ws, const std::string& directory, Fn fn) {
  fs::path base = ws.path(directory, true);
  if (!fs::is_directory(base)) bad("不是目录");
  std::size_t visited = 0;
  fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied), end;
  for (; it != end; ++it) {
    if (++visited > kMaxScanned) bad("目录扫描超过 10000 项");
    if (it.depth() >= kMaxDepth) it.disable_recursion_pending();
    const auto rel = ws.relative(it->path());
    if (it->is_symlink()) { it.disable_recursion_pending(); continue; }
    (void)ws.path(rel, true);
    if (!fn(*it, rel, it.depth())) break;
  }
}

// 按名称搜索：通配符 * 和 ?，不依赖系统 shell。
bool glob_match(std::string_view pattern, std::string_view name) {
  std::size_t p=0, n=0, star=std::string::npos, backtrack=0;
  while (n<name.size()) {
    if (p<pattern.size() && (pattern[p]=='?' || pattern[p]==name[n])) {++p; ++n;}
    else if (p<pattern.size() && pattern[p]=='*') {star=p++; backtrack=n;}
    else if (star!=std::string::npos) {p=star+1; n=++backtrack;}
    else return false;
  }
  while (p<pattern.size() && pattern[p]=='*') ++p;
  return p==pattern.size();
}

// 子进程只允许事先固定的程序与参数：不使用 shell，也不接受任意命令字符串。
struct ProcessResult { int exit_code; std::string output; bool timed_out; };
#ifdef _WIN32
std::string quote_windows(const std::string& a) {
  std::string out="\""; unsigned slashes=0;
  for (char c : a) {
    if (c=='\\') { ++slashes; continue; }
    if (c=='"') {out.append(slashes*2+1,'\\'); out+='"'; slashes=0; continue;}
    out.append(slashes,'\\'); slashes=0; out+=c;
  }
  out.append(slashes*2,'\\'); return out+'"';
}
ProcessResult execute(const std::vector<std::string>& args, int timeout_s = 12) {
  if (args.empty()) bad("缺少程序名");
  SECURITY_ATTRIBUTES sec{sizeof(sec), nullptr, TRUE};
  HANDLE read_pipe=nullptr, write_pipe=nullptr;
  if (!CreatePipe(&read_pipe,&write_pipe,&sec,0)) bad("创建子进程管道失败");
  SetHandleInformation(read_pipe,HANDLE_FLAG_INHERIT,0);
  STARTUPINFOA si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
  si.hStdOutput=write_pipe; si.hStdError=write_pipe;
  si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  std::string cmd;
  for(const auto& arg:args) {if(!cmd.empty()) cmd+=' '; cmd+=quote_windows(arg);}
  std::vector<char> mutable_cmd(cmd.begin(),cmd.end()); mutable_cmd.push_back('\0');
  BOOL ok=CreateProcessA(nullptr,mutable_cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi);
  CloseHandle(write_pipe);
  if(!ok) {CloseHandle(read_pipe); bad("启动受限子进程失败");}
  std::string output; bool timed_out=false;
  auto stop=std::chrono::steady_clock::now()+std::chrono::seconds(timeout_s);
  while(true) {
    DWORD available=0;
    if(!PeekNamedPipe(read_pipe,nullptr,0,nullptr,&available,nullptr)) break;
    if(available) {
      char buf[4096];DWORD n=0;
      if(!ReadFile(read_pipe,buf,(std::min)(available,DWORD(sizeof(buf))),&n,nullptr)) break;
      output.append(buf,n);
      if(output.size()>kMaxOutput) {TerminateProcess(pi.hProcess,1); break;}
    }
    if(WaitForSingleObject(pi.hProcess,10)==WAIT_OBJECT_0 && !available) break;
    if(std::chrono::steady_clock::now()>stop) {timed_out=true;TerminateProcess(pi.hProcess,1);break;}
  }
  WaitForSingleObject(pi.hProcess,INFINITE);
  DWORD exit_code=1;GetExitCodeProcess(pi.hProcess,&exit_code);
  CloseHandle(read_pipe);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
  if(output.size()>kMaxOutput) output.resize(kMaxOutput);
  return {static_cast<int>(exit_code),output,timed_out};
}
#else
ProcessResult execute(const std::vector<std::string>& args, int timeout_s = 12) {
  if(args.empty()) bad("缺少程序名");
  int fds[2]; if(pipe(fds)!=0) bad("创建子进程管道失败");
  const pid_t pid=fork();
  if(pid<0) {close(fds[0]);close(fds[1]);bad("创建子进程失败");}
  if(pid==0) {
    close(fds[0]);
    if(dup2(fds[1],STDOUT_FILENO)<0||dup2(fds[1],STDERR_FILENO)<0) _exit(127);
    close(fds[1]);
    std::vector<char*> argv;for(const auto& arg:args)argv.push_back(const_cast<char*>(arg.c_str()));argv.push_back(nullptr);
    execvp(argv[0],argv.data());_exit(127);
  }
  close(fds[1]);fcntl(fds[0],F_SETFL,fcntl(fds[0],F_GETFL)|O_NONBLOCK);
  std::string output; bool timed_out=false, eof=false; int state=0;
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(timeout_s);
  while(true) {
    char buf[4096];ssize_t n=read(fds[0],buf,sizeof(buf));
    if(n>0) output.append(buf,static_cast<std::size_t>(n));
    if(n==0) eof=true;
    if(output.size()>kMaxOutput || std::chrono::steady_clock::now()>until) {
      timed_out=std::chrono::steady_clock::now()>until;kill(pid,SIGKILL);break;
    }
    if(eof) break;
    pollfd item{fds[0],POLLIN,0};(void)poll(&item,1,20);
  }
  close(fds[0]);while(waitpid(pid,&state,0)<0 && errno==EINTR){}
  if(output.size()>kMaxOutput)output.resize(kMaxOutput);
  return {WIFEXITED(state)?WEXITSTATUS(state):128,output,timed_out};
}
#endif

Json process_json(ProcessResult p) {
  return object{{"exitCode",p.exit_code},{"output",p.output},{"timedOut",p.timed_out}};
}

// HTTP 仅允许精确 HTTPS 域名 example.com，不跟随重定向、不接受用户提供任意主机。
std::string http_fetch(const std::string& url) {
  if (url != "https://example.com/") bad("演示仅允许访问 https://example.com/，阻止 SSRF");
  httplib::Client client("https://example.com");
  client.set_follow_location(false);
  client.enable_server_certificate_verification(true);
  client.set_connection_timeout(3,0);
  client.set_read_timeout(5,0);
  std::string data;
  auto reply=client.Get("/",[&](const char* chunk,std::size_t n) {
    if(data.size()+n>kMaxFile) return false;
    data.append(chunk,n);return true;
  });
  if(!reply) bad("HTTPS 请求失败或响应超过限制");
  if(reply->status!=200) bad("HTTP 状态码: "+std::to_string(reply->status));
  return data;
}

// 简化的统一补丁：仅支持单文件、一个或多个 @@ 行号 hunk，严格匹配上下文。
std::string patch_text(const std::string& original, const std::string& patch) {
  bounded(patch, kMaxFile, "补丁");
  std::vector<std::string> lines, newlines;
  bool ends_newline=original.empty()||original.back()=='\n';
  std::istringstream in(original);std::string line;
  while(std::getline(in,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();lines.push_back(line);}
  std::istringstream diff(patch);std::size_t cursor=0;bool saw_hunk=false;
  std::regex header(R"(^@@ -([0-9]+)(?:,([0-9]+))? \+([0-9]+)(?:,([0-9]+))? @@.*$)");
  int old_count=0,new_count=0,seen_old=0,seen_new=0;bool in_hunk=false;
  auto finish=[&](){if(in_hunk&&(seen_old!=old_count||seen_new!=new_count))bad("补丁 hunk 行数不匹配");};
  while(std::getline(diff,line)) {
    if(!line.empty()&&line.back()=='\r')line.pop_back();
    if(line.rfind("--- ",0)==0||line.rfind("+++ ",0)==0) continue;
    if(line.rfind("\\ No newline at end of file",0)==0) continue;
    std::smatch match;
    if(std::regex_match(line,match,header)){
      finish();saw_hunk=true;in_hunk=true;
      std::size_t old_line=std::stoull(match[1]);
      old_count=match[2].matched?std::stoi(match[2]):1;
      new_count=match[4].matched?std::stoi(match[4]):1;
      seen_old=seen_new=0;
      std::size_t target=old_line==0?0:old_line-1;
      if(target<cursor||target>lines.size())bad("补丁 hunk 顺序或位置错误");
      newlines.insert(newlines.end(),lines.begin()+static_cast<std::ptrdiff_t>(cursor),lines.begin()+static_cast<std::ptrdiff_t>(target));
      cursor=target;continue;
    }
    if(!in_hunk||line.empty()||(line[0]!=' '&&line[0]!='-'&&line[0]!='+'))bad("不支持的补丁格式");
    char op=line[0];std::string content=line.substr(1);
    if(op!='+'){
      if(cursor>=lines.size()||lines[cursor]!=content)bad("补丁原文或上下文不匹配");
      ++cursor;++seen_old;
    }
    if(op!='-'){newlines.push_back(content);++seen_new;}
    if(seen_old>old_count||seen_new>new_count)bad("补丁 hunk 行数溢出");
  }
  finish();if(!saw_hunk)bad("没有有效 @@ hunk");
  newlines.insert(newlines.end(),lines.begin()+static_cast<std::ptrdiff_t>(cursor),lines.end());
  std::string output;
  for(std::size_t i=0;i<newlines.size();++i){output+=newlines[i];if(i+1<newlines.size()||ends_newline)output+='\n';}
  bounded(output,kMaxFile,"补丁后的文件");return output;
}
} // namespace

int run_agent_tools_demo() {
  try {
    Workspace ws;
    mymcp::Server server("agent-tools-demo", "1.0.0");
    server.instructions("工具只操作 demo_workspace/；路径均相对工作区。命令仅允许白名单，HTTP 只允许 https://example.com/。写入、替换、下载和编译应先取得用户同意。所有结果均设有大小限制。");

    // 统一注册函数：把错误作为工具执行失败（isError），避免整个服务因单次工具失败退出。
    auto reg = [&](std::string name, std::string description, Json input,
                   std::function<Json(const Json&)> action) {
      server.add_tool(std::move(name), std::move(description), std::move(input),
        [action=std::move(action)](const Json& args) -> Json {
          try { return result(action(args)); }
          catch(const std::exception& e) {
            return object{{"content",array{object{{"type","text"},{"text",std::string("工具执行失败: ")+e.what()}}}},
                          {"isError",true}};
          }
        });
    };

    // ① 文件读取：含大小限制、路径验证和元信息查看。
    reg("read_file","读取工作区内的 UTF-8/文本文件（最大 1 MiB）",
      schema({{"path","string"}}, {"path"}),[&](const Json& a) -> Json {
        const auto path=str(a,"path");
        const auto content=read_limited(ws.path(path,true));
        return object{{"path",path},{"content",content},{"bytes",content.size()}};
      });
    reg("get_file_info","查询文件或目录的类型、大小和修改时间",
      schema({{"path","string"}}, {"path"}),[&](const Json& a) -> Json {
        const auto relative=str(a,"path");const auto path=ws.path(relative,true);
        const bool directory=fs::is_directory(path);
        return object{{"path",relative},{"type",directory?"directory":"file"},
          {"bytes",directory?0:static_cast<std::uint64_t>(fs::file_size(path))},
          {"lastWriteTicks",static_cast<std::int64_t>(fs::last_write_time(path).time_since_epoch().count())}};
      });

    // ② 文件写入：默认拒绝覆盖，追加也有总容量限制。
    reg("write_file","创建或覆盖文本文件；默认禁止覆盖已有文件",
      schema({{"path","string"},{"content","string"},{"overwrite","boolean"}},{"path","content"}),
      [&](const Json& a) -> Json {
        const auto path=ws.path(str(a,"path"));
        if(fs::exists(path)&&!bool_arg(a,"overwrite"))bad("文件已存在，覆盖需要 overwrite=true");
        const auto content=str(a,"content");write_limited(path,content);
        return object{{"path",ws.relative(path)},{"bytesWritten",content.size()}};
      });
    reg("append_file","向已有文本文件末尾追加内容（总大小最大 1 MiB）",
      schema({{"path","string"},{"content","string"}},{"path","content"}),[&](const Json& a) -> Json {
        const auto path=ws.path(str(a,"path"),true);
        const auto content=str(a,"content");write_limited(path,content,true);
        return object{{"path",ws.relative(path)},{"bytesAppended",content.size()}};
      });

    // ③ 目录查询、创建和目录树：遍历有层数、数量上限。
    reg("list_directory","列出目录的第一层文件和子目录",
      schema({{"path","string"}},{"path"}),[&](const Json& a) -> Json {
        auto folder=ws.path(str(a,"path"),true);
        if(!fs::is_directory(folder))bad("不是目录");
        array items;std::size_t visited=0;
        for(const auto& it:fs::directory_iterator(folder)) {
          if(++visited>kMaxScanned)bad("目录项目过多");
          if(it.is_symlink())continue;
          const auto rel=ws.relative(it.path());(void)ws.path(rel,true);
          if(items.size()<kMaxResults)items.push_back(object{{"path",rel},{"directory",it.is_directory()}});
        }
        return object{{"items",items},{"truncated",visited>kMaxResults}};
      });
    reg("create_directory","在工作区内创建目录及其父目录",
      schema({{"path","string"}},{"path"}),[&](const Json& a) -> Json {
        const auto folder=ws.path(str(a,"path"));fs::create_directories(folder);
        return object{{"path",ws.relative(folder)},{"created",true}};
      });
    reg("directory_tree","递归查询目录树（最多 100 项，深度最多 12 层）",
      schema({{"path","string"}},{"path"}),[&](const Json& a) -> Json {
        array items;walk(ws,str(a,"path"),[&](const auto& entry,const std::string& rel,int depth){
          items.push_back(object{{"path",rel},{"depth",depth},{"directory",entry.is_directory()}});
          return items.size()<kMaxResults;
        });return object{{"items",items},{"truncated",items.size()>=kMaxResults}};
      });

    // ④ 按文件名/扩展名查询，不使用 shell 的通配符展开。
    reg("search_files","按文件名通配符查找文件，例如 *.cpp、test_?.h",
      schema({{"directory","string"},{"pattern","string"}},{"directory","pattern"}),
      [&](const Json& a) -> Json {
        const auto pattern=str(a,"pattern");bounded(pattern,128,"通配符");
        array found;walk(ws,str(a,"directory"),[&](const auto& it,const std::string& rel,int){
          if(it.is_regular_file()&&glob_match(pattern,it.path().filename().string()))found.push_back(rel);
          return found.size()<kMaxResults;
        });return object{{"files",found},{"truncated",found.size()>=kMaxResults}};
      });
    reg("find_by_extension","按文件扩展名查找，例如 .cpp 或 .h",
      schema({{"directory","string"},{"extension","string"}},{"directory","extension"}),
      [&](const Json& a) -> Json {
        auto ext=str(a,"extension");bounded(ext,32,"扩展名");
        if(ext.empty()||ext.front()!='.')bad("扩展名必须以 . 开头");
        array found;walk(ws,str(a,"directory"),[&](const auto& it,const std::string& rel,int){
          if(it.is_regular_file()&&it.path().extension().string()==ext)found.push_back(rel);
          return found.size()<kMaxResults;
        });return object{{"files",found},{"truncated",found.size()>=kMaxResults}};
      });

    // ⑤ 搜索文本：返回文件位置、行号和截断后的命中行。
    auto search_content=[&](const Json& a, bool regex_mode) -> Json {
      const auto query=str(a,regex_mode?"pattern":"text");bounded(query,regex_mode?80:256,"搜索内容");
      if(query.empty())bad("搜索内容不能为空");
      std::regex pattern;
      if(regex_mode){try{pattern=std::regex(query,std::regex::ECMAScript);}catch(const std::regex_error&){bad("正则表达式无效");}}
      array found;
      walk(ws,str(a,"directory"),[&](const auto& it,const std::string& rel,int){
        if(!it.is_regular_file()||it.file_size()>kMaxFile)return true;
        std::ifstream input(it.path(),std::ios::binary);if(!input)return true;
        std::string line;std::size_t line_no=0;
        while(std::getline(input,line)&&found.size()<kMaxResults){
          ++line_no;if(line.size()>4096)continue; // 防止单行异常大或无界正则匹配
          const bool matches=regex_mode?std::regex_search(line,pattern):line.find(query)!=std::string::npos;
          if(matches)found.push_back(object{{"path",rel},{"line",line_no},
            {"text",line.substr(0,240)}});
        }
        return found.size()<kMaxResults;
      });return object{{"matches",found},{"truncated",found.size()>=kMaxResults}};
    };
    reg("search_text","跨文件查找固定文本，返回路径和行号",
      schema({{"directory","string"},{"text","string"}},{"directory","text"}),
      [&](const Json& a)->Json{return search_content(a,false);});
    reg("grep_regex","跨文件按正则表达式搜索（限制表达式长度和扫描规模）",
      schema({{"directory","string"},{"pattern","string"}},{"directory","pattern"}),
      [&](const Json& a)->Json{return search_content(a,true);});

    // ⑥ 精确编辑：要求旧文本只匹配一次，补丁必须验证完整上下文。
    reg("replace_text","精确替换一次指定文本；有多个匹配时拒绝修改",
      schema({{"path","string"},{"old_text","string"},{"new_text","string"}},
        {"path","old_text","new_text"}),[&](const Json& a) -> Json {
        const auto path=ws.path(str(a,"path"),true);const auto old=str(a,"old_text"),repl=str(a,"new_text");
        if(old.empty())bad("old_text 不能为空");
        auto data=read_limited(path);const auto pos=data.find(old);
        if(pos==std::string::npos)bad("未找到待替换文本");
        if(data.find(old,pos+old.size())!=std::string::npos)bad("存在多个匹配，为防误改已拒绝");
        data.replace(pos,old.size(),repl);bounded(data,kMaxFile,"修改后的文件");
        write_limited(path,data);return object{{"path",ws.relative(path)},{"replacements",1}};
      });
    reg("apply_patch","对指定文件应用带 @@ 行号的严格统一补丁（单文件）",
      schema({{"path","string"},{"patch","string"}},{"path","patch"}),[&](const Json& a) -> Json {
        const auto path=ws.path(str(a,"path"),true);auto data=read_limited(path);
        auto changed=patch_text(data,str(a,"patch"));
        write_limited(path,changed);return object{{"path",ws.relative(path)},{"bytes",changed.size()}};
      });

    // ⑦ Git 查询：只读取工作区自己的 git 状态、差异和提交历史。
    auto git_query=[&](const std::string& subcommand)->Json {
      std::vector<std::string> args{"git","-C",ws.root.string()};
      if(subcommand=="status")args.insert(args.end(),{"status","--short","--untracked-files=normal"});
      else if(subcommand=="diff")args.insert(args.end(),{"diff","--no-ext-diff","--no-color"});
      else args.insert(args.end(),{"log","-5","--pretty=format:%h %s"});
      return process_json(execute(args));
    };
    reg("git_status","查询工作区 Git 状态（仓库未初始化时返回错误状态码）",schema({}),
      [&](const Json&)->Json{return git_query("status");});
    reg("git_diff","查看工作区 Git 未提交改动",schema({}),
      [&](const Json&)->Json{return git_query("diff");});
    reg("git_log","查看工作区最近五条 Git 提交",schema({}),
      [&](const Json&)->Json{return git_query("log");});

    // ⑧ 受限命令：只允许两个版本查询，绝不把用户提供的字符串交给系统 shell。
    reg("run_command","仅执行白名单 git_version / cmake_version，拒绝任意 shell 命令",
      schema({{"command","string"}},{"command"}),[&](const Json& a) -> Json {
        const auto name=str(a,"command");
        if(name=="git_version")return process_json(execute({"git","--version"}));
        if(name=="cmake_version")return process_json(execute({"cmake","--version"}));
        bad("命令不在白名单：仅允许 git_version、cmake_version");
      });
    reg("run_build","只编译 Demo 自动生成并校验过的无害 CMake 样例项目",
      schema({{"confirm","boolean"}},{"confirm"}),[&](const Json& a) -> Json {
        if(!bool_arg(a,"confirm"))bad("编译前必须显式传入 confirm=true 并得到用户授权");
        const auto sample=ws.path("agent_build_sample");
        if(!fs::exists(sample))fs::create_directories(sample);
        constexpr const char* cmake="cmake_minimum_required(VERSION 3.20)\nproject(agent_demo LANGUAGES CXX)\nadd_executable(agent_demo main.cpp)\n";
        constexpr const char* main_cpp="int main() { return 0; }\n";
        const auto configuration=ws.path("agent_build_sample/CMakeLists.txt");
        const auto source=ws.path("agent_build_sample/main.cpp");
        if(!fs::exists(configuration))write_limited(configuration,cmake);
        if(!fs::exists(source))write_limited(source,main_cpp);
        if(read_limited(configuration)!=cmake||read_limited(source)!=main_cpp)
          bad("构建样例源文件已被修改，拒绝执行不可信 CMake 脚本");
        const auto builddir=ws.path("agent_build_sample/build");
        auto config=execute({"cmake","-S",sample.string(),"-B",builddir.string()},15);
        if(config.exit_code!=0||config.timed_out)return process_json(config);
        auto build=execute({"cmake","--build",builddir.string()},25);
        return object{{"configure",process_json(config)},{"build",process_json(build)}};
      });

    // ⑨ HTTP：强制 HTTPS、域名白名单、无重定向和 1 MiB 响应限额。
    reg("http_get","只允许 GET https://example.com/，防止任意 URL 引发 SSRF",
      schema({{"url","string"}},{"url"}),[&](const Json& a) -> Json {
        auto data=http_fetch(str(a,"url"));
        return object{{"status",200},{"bytes",data.size()},{"body",data.substr(0,kMaxOutput)}};
      });
    reg("download_file","只允许从 https://example.com/ 下载到工作区的指定文件",
      schema({{"url","string"},{"path","string"}},{"url","path"}),[&](const Json& a) -> Json {
        const auto dest=ws.path(str(a,"path"));
        if(fs::exists(dest))bad("禁止覆盖已有文件");
        auto data=http_fetch(str(a,"url"));write_limited(dest,data);
        return object{{"path",ws.relative(dest)},{"bytes",data.size()}};
      });

    // ⑩ 工程分析：构建系统识别、文件统计、C++ 代码符号搜索。
    auto detect_build=[&]()->Json {
      array names;
      for(const auto& file:{"CMakeLists.txt","meson.build","Makefile","package.json","Cargo.toml"}) {
        if(fs::exists(ws.path(file)))names.push_back(file);
      }
      return object{{"buildFiles",names},{"known",!names.empty()}};
    };
    reg("detect_build_system","识别工作区根目录的 CMake/Meson/Make 等构建系统",schema({}),
      [&](const Json&)->Json{return detect_build();});
    reg("project_info","统计工作区中的普通文件与常见源码文件数量（有遍历上限）",schema({}),
      [&](const Json&)->Json {
        std::size_t files=0,cpp=0,dirs=0;
        walk(ws,".",[&](const auto& entry,const std::string&,int){
          if(entry.is_directory())++dirs;
          else if(entry.is_regular_file()){
            ++files;auto ext=entry.path().extension().string();
            if(ext==".cpp"||ext==".cc"||ext==".c"||ext==".hpp"||ext==".h")++cpp;
          }return true;
        });
        return object{{"root",ws.root.string()},{"files",files},{"directories",dirs},
                      {"sourceFiles",cpp},{"buildSystem",detect_build()}};
      });
    reg("find_symbols","在 C/C++ 源码中查找指定符号标识符及其位置（文本级查找）",
      schema({{"symbol","string"}},{"symbol"}),[&](const Json& a) -> Json {
        const auto symbol=str(a,"symbol");bounded(symbol,80,"符号名");
        if(symbol.empty()||!std::all_of(symbol.begin(),symbol.end(),[](unsigned char c){
          return std::isalnum(c)||c=='_';}))bad("symbol 只能包含字母、数字和下划线");
        const std::regex pattern("\\b"+symbol+"\\b");array found;
        walk(ws,".",[&](const auto& entry,const std::string& rel,int){
          if(!entry.is_regular_file()||entry.file_size()>kMaxFile)return true;
          const auto ext=entry.path().extension().string();
          if(ext!=".cpp"&&ext!=".cc"&&ext!=".c"&&ext!=".hpp"&&ext!=".h")return true;
          std::ifstream in(entry.path());std::string line;std::size_t n=0;
          while(std::getline(in,line)&&found.size()<kMaxResults){
            ++n;if(line.size()>4096)continue;
            if(std::regex_search(line,pattern))found.push_back(object{{"path",rel},{"line",n},
                {"text",line.substr(0,240)}});
          }
          return found.size()<kMaxResults;
        });return object{{"matches",found},{"truncated",found.size()>=kMaxResults}};
      });

    // stdio 是 Codex、Claude Code 等本地智能体最普遍的接入方式。
    // 切记 stdout 只输出 JSON-RPC 协议数据，帮助信息写入 stderr。
    std::cerr << "agent-tools-demo: sandbox=" << ws.root.string() << " (stdio MCP ready)\n";
    server.run_stdio();
    return 0;
  } catch(const std::exception& e) {
    std::cerr << "run_agent_tools_demo: " << e.what() << '\n';return 1;
  }
}

int main() { return run_agent_tools_demo(); }
