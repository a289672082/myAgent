// 独立 Codex 风格测试控制台。
// 仅依赖 FTXUI + SDK 公开 API，不向 myAgent 静态库引入任何 TUI 代码。
// 宿主负责按键、密码遮挡、工具审批、渲染、工作线程；SDK 仍然通用。
#include "session.hpp"
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using namespace ftxui;
using namespace myagent_console;

enum class Role { System, User, Assistant, Error, Tool };
struct Entry {Role role;std::string text;};

#ifdef _WIN32
class ConsoleInputMode {
 public:
  ConsoleInputMode() : handle_(GetStdHandle(STD_INPUT_HANDLE)) {
    if(handle_!=INVALID_HANDLE_VALUE && GetConsoleMode(handle_,&original_))
      changed_=SetConsoleMode(handle_,original_ & ~ENABLE_PROCESSED_INPUT)!=0;
  }
  ~ConsoleInputMode() {if(changed_)SetConsoleMode(handle_,original_);}
  ConsoleInputMode(const ConsoleInputMode&)=delete;
  ConsoleInputMode& operator=(const ConsoleInputMode&)=delete;
 private:
  HANDLE handle_;
  DWORD original_=0;
  bool changed_=false;
};
#endif

bool copy_selection(const std::string& value) {
#ifdef _WIN32
  const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),
                                     static_cast<int>(value.size()),nullptr,0);
  if(size<=0)return false;
  HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,(static_cast<size_t>(size)+1)*sizeof(wchar_t));
  if(!memory)return false;
  auto* text=static_cast<wchar_t*>(GlobalLock(memory));
  if(!text) {GlobalFree(memory);return false;}
  MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),
                      static_cast<int>(value.size()),text,size);
  text[size]=L'\0';GlobalUnlock(memory);
  if(!OpenClipboard(nullptr)) {GlobalFree(memory);return false;}
  const bool ok=EmptyClipboard() && SetClipboardData(CF_UNICODETEXT,memory);
  CloseClipboard();
  if(!ok)GlobalFree(memory);
  return ok;
#else
  (void)value;
  return false;
#endif
}

std::string visible(std::string text,std::size_t limit=24*1024) {
  if(text.size()>limit)text=text.substr(0,limit)+"\n[内容已截断]";
  for(char& c:text)if(static_cast<unsigned char>(c)<32 &&
                      c!='\n' && c!='\t')c='?';
  return text;
}

// 长回复只在正在流式生成时展示最新部分，避免超过上限后屏幕永远停在开头。
std::string visible_latest(std::string value,std::size_t limit=24*1024) {
  if(value.size()>limit) {
    std::size_t start=value.size()-limit;
    // 不从 UTF-8 多字节字符中间截断，保持 FTXUI 中文显示正确。
    while(start<value.size() &&
          (static_cast<unsigned char>(value[start])&0xc0)==0x80)++start;
    value="[前文已省略，实时显示最新内容]\n"+value.substr(start);
  }
  return visible(std::move(value),limit+128);
}

std::string label(Role role) {
  switch(role) {
    case Role::System:return "• System";
    case Role::User:return "› You";
    case Role::Assistant:return "• myAgent";
    case Role::Error:return "× Error";
    case Role::Tool:return "⚙ Tool";
  }
  return "•";
}
Color label_color(Role role) {
  switch(role) {
    case Role::System:return Color::GrayDark;
    case Role::User:return Color::Cyan;
    case Role::Assistant:return Color::Green;
    case Role::Error:return Color::Red;
    case Role::Tool:return Color::Yellow;
  }
  return Color::White;
}

struct ApprovalGate {
  std::mutex mutex;
  std::condition_variable cv;
  std::optional<myagent::ToolApprovalRequest> request;
  std::optional<bool> answer;
  // 当任务被取消时唤醒等待审批的工作线程。
  void respond(bool approve) {
    std::lock_guard lock(mutex);
    if(request)answer=approve;
    cv.notify_all();
  }
};

class ConsoleApp {
 public:
  int run() {
    auto app=App::Fullscreen();
    app.ForceHandleCtrlC(false);
    std::string composer_text,password;
    int composer_cursor=0;
    int suggestion_index=-1;
    int scroll_offset=0;
    Element transcript_content;
    int tab=0; // 0=聊天输入；1=遮挡的 API Key 输入。
    bool approved_once=false;
    (void)approved_once;

    auto add=[&](Role role,std::string message) {
      std::lock_guard lock(ui_mutex_);
      if(entries_.size()>=120)entries_.pop_front();
      entries_.push_back({role,visible(std::move(message))});
    };
    add(Role::System,
        "myAgent Console | Codex 风格测试客户端\n"
        "输入任务并回车；使用 /help 查看命令。MCP 工具默认需要人工审批。"
        "\nAPI Key、模型和地址会保存到用户配置；配置修改会重置对话。");

    // 合并密集 token 引发的重绘事件：每轮 UI 仅保留一个待处理刷新。
    // 否则大量 SSE 片段会堆积完整布局计算，最后看起来像一次性输出。
    auto mark_dirty=[&]() {
      if(!redraw_queued_.exchange(true,std::memory_order_acq_rel))
        app.PostEvent(Event::Custom);
    };

    std::function<void()> send_input;
    std::function<void()> send_key;
    send_key=[&]() {
      if(busy_.load())return;
      auto key=std::move(password);
      password.clear();
      tab=0;
      if(key.empty()) {add(Role::System,"未输入密钥，操作已取消。");return;}
      try {
        session_.set_api_key(std::move(key));
        add(Role::System,"API Key 已设置（已隐藏）；Agent 会话已重置。");
      } catch(const std::exception& e) {
        add(Role::Error,e.what());
      }
    };

    send_input=[&]() {
      if(busy_.load() || tab!=0)return;
      scroll_offset=0;
      auto line=std::move(composer_text);
      composer_text.clear();
      const auto command=session_.handle(line);
      if(command.kind==CommandKind::SecretPrompt) {
        tab=1;
        password.clear();
        add(Role::System,command.message);
        return;
      }
      if(command.kind==CommandKind::Exit) {
        app.Exit();return;
      }
      if(command.kind==CommandKind::ClearView) {
        std::lock_guard lock(ui_mutex_);
        entries_.clear();
      }
      if(command.kind!=CommandKind::Send) {
        if(!command.message.empty())
          add(command.kind==CommandKind::Error?Role::Error:Role::System,command.message);
        return;
      }
      {
        std::lock_guard lock(ui_mutex_);
        stream_.clear();
        stream_chunks_=0;
        activity_="正在请求模型...";
      }
      history_.push_back(command.message);
      if(history_.size()>60)history_.erase(history_.begin());
      add(Role::User,command.message);
      busy_=true;
      cancelled_=false;
      if(worker_.joinable())worker_.join();
      worker_=std::jthread([&,message=command.message] {
        try {
          auto outcome=session_.chat(message,
            // 只渲染公开的文本增量，不渲染隐藏推理和工具参数。
            [&](const std::string& phase,const std::string& chunk) {
              (void)phase;
              {
                std::lock_guard lock(ui_mutex_);
                // 使用滑动窗口保存最新文本，长回复也不会在固定字节数后停止更新。
                // 达到 96 KiB 时回收旧内容至 64 KiB，减少每个 token 的内存搬移。
                stream_+=chunk;
                if(stream_.size()>96*1024) {
                  std::size_t trim=stream_.size()-64*1024;
                  while(trim<stream_.size() &&
                        (static_cast<unsigned char>(stream_[trim])&0xc0)==0x80)++trim;
                  stream_.erase(0,trim);
                }
                ++stream_chunks_;
              }
              mark_dirty();
            },
            [&](const myagent::ToolApprovalRequest& request)->bool {
              {
                std::lock_guard lock(approval_.mutex);
                approval_.request=request;
                approval_.answer.reset();
              }
              mark_dirty();
              std::unique_lock lock(approval_.mutex);
              approval_.cv.wait(lock,[&] {
                return approval_.answer.has_value() || cancelled_.load();
              });
              const bool accepted=approval_.answer.value_or(false) && !cancelled_.load();
              approval_.request.reset();
              approval_.answer.reset();
              lock.unlock();
              mark_dirty();
              return accepted;
            },
            [&](const std::string& status) {
              {
                std::lock_guard lock(ui_mutex_);
                activity_=status;
              }
              mark_dirty();
            },
            [&]() {return cancelled_.load();}
          );
          std::string incomplete_stream;
          {
            std::lock_guard lock(ui_mutex_);
            if(outcome.status!=myagent::LoopStatus::Completed)
              incomplete_stream=std::move(stream_);
            stream_.clear();
            stream_chunks_=0;
            activity_.clear();
          }
          // 网络失败或取消后保留已收到的增量文本，便于诊断断流问题。
          if(!incomplete_stream.empty())
            add(Role::Assistant,"（未完成的流式输出）\n"+incomplete_stream);
          if(outcome.status==myagent::LoopStatus::Completed)
            add(Role::Assistant,outcome.answer.empty()?"（模型未返回正文）":outcome.answer);
          else
            add(Role::Error,"任务未完成：" +
                (outcome.error.empty()?std::string("Loop 状态异常"):outcome.error));
        } catch(const std::exception& e) {
          std::string incomplete_stream;
          {
            std::lock_guard lock(ui_mutex_);
            incomplete_stream=std::move(stream_);
            stream_.clear();
            stream_chunks_=0;
            activity_.clear();
          }
          if(!incomplete_stream.empty())
            add(Role::Assistant,"（未完成的流式输出）\n"+incomplete_stream);
          add(Role::Error,std::string("请求失败：")+e.what());
        }
        busy_=false;
        mark_dirty();
      });
    };

    InputOption composer_opts;
    composer_opts.multiline=false;
    composer_opts.cursor_position=&composer_cursor;
    composer_opts.on_change=[&]{suggestion_index=-1;};
    composer_opts.on_enter=[&]{send_input();};
    auto composer=Input(&composer_text,"输入消息或 /help",composer_opts);
    InputOption secret_opts;
    secret_opts.multiline=false;
    secret_opts.password=true;
    secret_opts.on_enter=[&]{send_key();};
    auto secret=Input(&password,"API Key（已遮挡，回车提交）",secret_opts);
    auto input_tabs=Container::Tab({composer,secret},&tab);

    auto display=Renderer(input_tabs,[&]() -> Element {
      std::deque<Entry> snapshot;
      std::string partial,activity;
      std::size_t stream_chunks=0;
      {
        std::lock_guard lock(ui_mutex_);
        snapshot=entries_;
        partial=stream_;
        activity=activity_;
        stream_chunks=stream_chunks_;
      }
      std::vector<Element> log;
      log.reserve(snapshot.size()+4);
      for(const auto& entry:snapshot) {
        log.push_back(vbox({
          text(label(entry.role))|bold|color(label_color(entry.role)),
          paragraph(entry.text)
        }));
        log.push_back(text(""));
      }
      if(!partial.empty()) {
        log.push_back(vbox({
          text("• myAgent · 实时输出 ("+std::to_string(stream_chunks)+" 段)")|bold|color(Color::Green),
          paragraph(visible_latest(std::move(partial)))
        }));
      }
      if(log.empty())log.push_back(text("开始一段对话。"));
      const int content_height=transcript_content?std::max(1,transcript_content->requirement().min_y):1;
      auto content=vbox(std::move(log));
      transcript_content=content;
      Element transcript=(scroll_offset==0
        ?content | focusPositionRelative(0.0f,1.0f)
        :content | focusPosition(0,std::max(0,content_height-1-scroll_offset)))
        | vscroll_indicator | frame | flex;

      std::string status=
        session_.config().model+" · "+session_.config().base_url+
        " · MCP "+std::to_string(session_.endpoints().size())+
        (busy_.load()?" · 工作中":" · 就绪");
      std::vector<Element> bottom;
      bool pending=false;
      myagent::ToolApprovalRequest approval_request;
      {
        std::lock_guard lock(approval_.mutex);
        if(approval_.request) {
          pending=true;
          approval_request=*approval_.request;
        }
      }
      if(pending) {
        bottom.push_back(vbox({
          text("工具审批 | Y 允许一次 · N 拒绝 · Esc 取消") |
            bold | color(Color::Yellow),
          paragraph(visible(approval_request.name+
            "  "+mymcp::json::serialize(approval_request.arguments),900))
        })|border);
      } else {
        if(!activity.empty() && busy_.load())
          bottom.push_back(text("● "+activity)|color(Color::Yellow));
        if(tab==0) {
          const auto matches=session_.suggestions(composer_text);
          if(composer_text.starts_with("/") && !matches.empty()) {
            std::vector<Element> hints;
            for(std::size_t i=0;i<matches.size();++i) {
              const bool selected=static_cast<int>(i)==suggestion_index;
              hints.push_back(text(std::string(selected?" › ":"   ")+matches[i]) |
                              color(selected?Color::Cyan:Color::GrayDark));
            }
            bottom.push_back(vbox(std::move(hints)));
          }
        }
        bottom.push_back(hbox({
          text(tab==1?" API Key › ":" › ")|bold|color(Color::Cyan),
          (tab==1?secret->Render():composer->Render())|flex
        })|border);
      }
      bottom.push_back(
        text(tab==1?"输入框已遮挡 · Esc 取消":
             busy_.load()?"Esc / Ctrl+C 取消任务":
                          "Enter 发送 · ↑↓ 命令 · Tab 补全 · 滚轮/PgUp/PgDn 浏览 · Ctrl+D 退出")
        | color(Color::GrayDark)
      );
      bottom.push_back(text(status)|color(Color::GrayDark));
      return vbox({
        hbox({
          text("  >_  myAgent Console")|bold|color(Color::Cyan),
          filler(),
          text("C++20 · by二狗  ")|color(Color::GrayDark)
        }),
        separator(),
        transcript,
        separator(),
        vbox(std::move(bottom))
      });
    });

    auto root=CatchEvent(display,[&](Event event) {
      if(event==Event::Custom) {
        // 主线程已取出一次刷新请求；后续 SSE 片段现在可以再次发起刷新。
        redraw_queued_.store(false,std::memory_order_release);
        return false;
      }
      if(event==Event::CtrlC) {
        const auto selected=app.GetSelection();
        if(!selected.empty()) {
          if(!copy_selection(selected))
            add(Role::Error,"复制失败；可按住 Shift 使用终端选择并复制。");
          return true;
        }
      }
      if(event==Event::CtrlC || event==Event::Escape) {
        if(tab==1) {
          tab=0;
          password.clear();
          add(Role::System,"API Key 输入已取消。");
        } else if(busy_.load()) {
          cancelled_=true;
          approval_.respond(false);
          add(Role::System,"已请求取消当前任务；等待安全边界或网络超时。");
        } else if(event==Event::CtrlC) {
          if(!composer_text.empty()) {
            composer_text.clear();
            composer_cursor=0;
            suggestion_index=-1;
          } else {
            add(Role::System,"退出请使用 Ctrl+D 或 /exit。");
          }
        }
        return true;
      }
      if(event==Event::CtrlD) {
        if(!busy_.load())app.Exit();
        return true;
      }
      // 人工确认包含工具参数；审批结果只提交给正在等待的工作线程。
      {
        std::lock_guard lock(approval_.mutex);
        if(approval_.request && event.is_character()) {
          const auto character=event.character();
          if(character=="y" || character=="Y") {
            approval_.answer=true;
            approval_.cv.notify_all();
            return true;
          }
          if(character=="n" || character=="N") {
            approval_.answer=false;
            approval_.cv.notify_all();
            return true;
          }
        }
        if(approval_.request)return true; // 审批期间禁止其它输入
      }
      const bool scroll_up=event==Event::PageUp ||
        (event.is_mouse() && event.mouse().button==Mouse::WheelUp);
      const bool scroll_down=event==Event::PageDown ||
        (event.is_mouse() && event.mouse().button==Mouse::WheelDown);
      if(scroll_up || scroll_down) {
        const int height=transcript_content?std::max(1,transcript_content->requirement().min_y):1;
        const int step=event.is_mouse()?3:10;
        scroll_offset=std::clamp(scroll_offset+(scroll_up?step:-step),0,height);
        return true;
      }
      if(tab==0 && !busy_.load()) {
        const auto matches=session_.suggestions(composer_text);
        if(!matches.empty() && !(matches.size()==1 && matches[0]==composer_text)) {
          if(event==Event::ArrowDown || event==Event::ArrowUp) {
            const int count=static_cast<int>(matches.size());
            if(suggestion_index<0)
              suggestion_index=event==Event::ArrowDown?0:count-1;
            else
              suggestion_index=(suggestion_index+(event==Event::ArrowDown?1:count-1))%count;
            return true;
          }
          if(event==Event::Tab || (event==Event::Return && suggestion_index>=0)) {
            const int index=std::clamp(suggestion_index,0,static_cast<int>(matches.size())-1);
            composer_text=matches[index];
            composer_cursor=static_cast<int>(composer_text.size());
            suggestion_index=-1;
            return true;
          }
        }
      }
      if(event==Event::ArrowUp && tab==0 && composer_text.empty() &&
         !busy_.load() && !history_.empty()) {
        composer_text=history_.back();
        composer_cursor=static_cast<int>(composer_text.size());
        return true;
      }
      return false;
    });

#ifdef _WIN32
    // Let FTXUI receive Ctrl+C as input instead of a Windows CTRL_C_EVENT.
    // Construct before Loop: FTXUI saves and later restores the mode it sees.
    ConsoleInputMode input_mode;
#endif
    app.Loop(root);
    cancelled_=true;
    approval_.respond(false);
    if(worker_.joinable())worker_.join();
    return 0;
  }

 private:
  ConsoleSession session_;
  std::mutex ui_mutex_;
  std::deque<Entry> entries_;
  std::string stream_,activity_;
  std::size_t stream_chunks_=0; // 由 ui_mutex_ 保护，仅用于实时显示进度。
  std::atomic_bool redraw_queued_{false};
  std::vector<std::string> history_;
  std::atomic_bool busy_{false},cancelled_{false};
  ApprovalGate approval_;
  std::jthread worker_;
};
} // namespace

int main() {
  try {return ConsoleApp{}.run();}
  catch(const std::exception& e) {
    std::cerr<<"myAgent Console 错误："<<e.what()<<'\n';
    return 1;
  }
}
