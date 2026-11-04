// 独立 Codex 风格测试控制台。
// 仅依赖 FTXUI + SDK 公开 API，不向 myAgent 静态库引入任何 TUI 代码。
// 宿主负责按键、密码遮挡、工具审批、渲染、工作线程；SDK 仍然通用。
#include "session.hpp"
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
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

namespace {
using namespace ftxui;
using namespace myagent_console;

enum class Role { System, User, Assistant, Error, Tool };
struct Entry {Role role;std::string text;};

std::string visible(std::string text,std::size_t limit=24*1024) {
  if(text.size()>limit)text=text.substr(0,limit)+"\n[内容已截断]";
  for(char& c:text)if(static_cast<unsigned char>(c)<32 &&
                      c!='\n' && c!='\t')c='?';
  return text;
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
        "\nAPI Key 不显示、不写入文件；配置修改会重置对话。");

    auto mark_dirty=[&]() {app.PostEvent(Event::Custom);};

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
                if(stream_.size()<100*1024)stream_+=chunk;
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
          {
            std::lock_guard lock(ui_mutex_);
            stream_.clear();
            activity_.clear();
          }
          // 不将失败状态误表示为已完成；用户可以看见具体的 Loop 错误。
          if(outcome.status==myagent::LoopStatus::Completed)
            add(Role::Assistant,outcome.answer.empty()?"（模型未返回正文）":outcome.answer);
          else
            add(Role::Error,"任务未完成：" +
                (outcome.error.empty()?std::string("Loop 状态异常"):outcome.error));
        } catch(const std::exception& e) {
          {
            std::lock_guard lock(ui_mutex_);
            stream_.clear();
            activity_.clear();
          }
          add(Role::Error,std::string("请求失败：")+e.what());
        }
        busy_=false;
        mark_dirty();
      });
    };

    InputOption composer_opts;
    composer_opts.multiline=false;
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
      {
        std::lock_guard lock(ui_mutex_);
        snapshot=entries_;
        partial=stream_;
        activity=activity_;
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
          text("• myAgent · streaming")|bold|color(Color::Green),
          paragraph(visible(std::move(partial)))
        }));
      }
      if(log.empty())log.push_back(text("开始一段对话。"));
      Element transcript=vbox(std::move(log))
        | focusPositionRelative(0.0f,1.0f)
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
        bottom.push_back(hbox({
          text(tab==1?" API Key › ":" › ")|bold|color(Color::Cyan),
          (tab==1?secret->Render():composer->Render())|flex
        })|border);
        if(tab==0) {
          const auto matches=session_.suggestions(composer_text);
          if(composer_text.starts_with("/") && !matches.empty()) {
            std::vector<Element> hints;
            for(const auto& command:matches)
              hints.push_back(text(" "+command)|color(Color::GrayDark));
            bottom.push_back(vbox(std::move(hints)));
          }
        }
      }
      bottom.push_back(
        text(tab==1?"输入框已遮挡 · Esc 取消":
             busy_.load()?"Esc / Ctrl+C 取消任务":
                          "Enter 发送 · /help 命令 · /permissions 设置工具审批 · Ctrl+D 退出")
        | color(Color::GrayDark)
      );
      bottom.push_back(text(status)|color(Color::GrayDark));
      return vbox({
        hbox({
          text("  >_  myAgent Console")|bold|color(Color::Cyan),
          filler(),
          text("C++20 · TEST CLIENT  ")|color(Color::GrayDark)
        }),
        separator(),
        transcript,
        separator(),
        vbox(std::move(bottom))
      });
    });

    auto root=CatchEvent(display,[&](Event event) {
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
          app.Exit();
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
      if(event==Event::ArrowUp && tab==0 && composer_text.empty() &&
         !busy_.load() && !history_.empty()) {
        composer_text=history_.back();
        return true;
      }
      return false;
    });

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
