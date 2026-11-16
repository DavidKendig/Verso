#include "app/McpServer.h"

#ifdef _WIN32
#include <sddl.h>
#else
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "core/TextUtil.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "verso_version.h"

namespace verso {

namespace {

constexpr size_t kBufferSize = 64 * 1024;
constexpr size_t kMaxMessage = 32u << 20;               // one JSON-RPC message
constexpr auto kCallTimeout = std::chrono::minutes(2);  // a tool call the UI never ran
const char* const kProtocolVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

#ifdef _WIN32
// A security descriptor granting access to the current user (and SYSTEM) only.
PSECURITY_DESCRIPTOR userOnlyDescriptor() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> buf(size);
  PSECURITY_DESCRIPTOR sd = nullptr;
  LPWSTR sid = nullptr;
  if (size && GetTokenInformation(token, TokenUser, buf.data(), size, &size) &&
      ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
    std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")(A;;GA;;;SY)";
    ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
    LocalFree(sid);
  }
  CloseHandle(token);
  return sd;
}

// An overlapped operation with its own event.
struct Overlapped {
  OVERLAPPED ov{};
  Overlapped() { ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
  ~Overlapped() {
    if (ov.hEvent) CloseHandle(ov.hEvent);
  }
  Overlapped(const Overlapped&) = delete;
  Overlapped& operator=(const Overlapped&) = delete;

  // Waits for the operation `started` began; false when it failed or `stop` fired.
  bool finish(HANDLE h, BOOL started, DWORD& bytes, HANDLE stop) {
    if (!started && GetLastError() != ERROR_IO_PENDING) return false;
    HANDLE events[2] = {ov.hEvent, stop};
    if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0) {
      CancelIoEx(h, &ov);
      GetOverlappedResult(h, &ov, &bytes, TRUE);
      return false;
    }
    return GetOverlappedResult(h, &ov, &bytes, FALSE) != FALSE;
  }
};
#else
// The folder for the socket: $XDG_RUNTIME_DIR (the user's own 0700 folder), else a
// private folder in /tmp. Empty when neither is usable.
std::string socketDir() {
  if (const char* x = getenv("XDG_RUNTIME_DIR"); x && *x) return x;
  std::string dir = "/tmp/verso-" + std::to_string(getuid());
  mkdir(dir.c_str(), 0700);
  struct stat st {};
  if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) return {};
  return dir;
}

// Waits until `fd` is ready for `events`; false when `stopFd` fired or polling failed.
bool waitFor(int fd, short events, int stopFd) {
  for (;;) {
    pollfd p[2] = {{fd, events, 0}, {stopFd, POLLIN, 0}};
    int n = poll(p, 2, -1);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 || (p[1].revents & POLLIN)) return false;
    if (p[0].revents) return true;
  }
}
#endif

json::Value rpcResult(const json::Value& id, json::Value result) {
  json::Value r = json::Value::makeObject();
  r.set("jsonrpc", "2.0");
  r.set("id", id);
  r.set("result", std::move(result));
  return r;
}

json::Value rpcError(const json::Value& id, int code, const std::string& message) {
  json::Value e = json::Value::makeObject();
  e.set("code", code);
  e.set("message", message);
  json::Value r = json::Value::makeObject();
  r.set("jsonrpc", "2.0");
  r.set("id", id);
  r.set("error", std::move(e));
  return r;
}

}  // namespace

struct McpServer::Call {
  std::string tool;
  json::Value args;
  json::Value result;
  bool done = false;
};

struct McpServer::Shared {
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::shared_ptr<Call>> queue;
  std::atomic<bool> stopping{false};
#ifdef _WIN32
  HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // ends every wait in the server
#else
  int stopFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);  // readable once stop() runs
  std::string path;                                     // the socket file, removed on stop
#endif
  std::atomic<HWND> notify{nullptr};
  UINT message = 0;
  json::Value tools;
  std::string instructions;
  std::wstring name;
#ifdef _WIN32
  PSECURITY_DESCRIPTOR sd = nullptr;
#endif
  std::atomic<int> clientCount{0};

#ifdef _WIN32
  ~Shared() {
    if (sd) LocalFree(sd);
    if (stopEvent) CloseHandle(stopEvent);
  }

  HANDLE createInstance(bool first) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    return CreateNamedPipeW(name.c_str(),
                            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, kBufferSize, kBufferSize, 0, &sa);
  }
#else
  ~Shared() {
    if (stopFd >= 0) close(stopFd);
  }
#endif

  // A tools/call: queued for the UI thread, which fills in the result.
  json::Value callTool(const std::string& tool, const json::Value& args) {
    auto call = std::make_shared<Call>();
    call->tool = tool;
    call->args = args;
    HWND h = notify.load();
    {
      std::unique_lock<std::mutex> l(m);
      if (!h || stopping) return json::Value();
      queue.push_back(call);
    }
    PostMessageW(h, message, 0, 0);
    std::unique_lock<std::mutex> l(m);
    if (!cv.wait_for(l, kCallTimeout, [&] { return call->done || stopping.load(); })) return json::Value();
    return call->done ? call->result : json::Value();
  }

  // The response line for one request, or "" for a notification.
  std::string handle(const std::string& line) {
    std::string err;
    auto msg = json::parse(line, &err);
    if (!msg) return json::serialize(rpcError(json::Value(), -32700, "Parse error: " + err), 0);
    if (!msg->isObject()) return json::serialize(rpcError(json::Value(), -32600, "Invalid request"), 0);
    const json::Value* idp = msg->get("id");
    const std::string method = msg->get("method") ? msg->get("method")->asString() : "";
    if (!idp) return {};  // notifications (initialized, cancelled) need no answer
    const json::Value id = *idp;
    const json::Value* params = msg->get("params");
    if (method == "initialize") {
      std::string version = kProtocolVersions[0];
      if (params) {
        const std::string asked = params->get("protocolVersion") ? params->get("protocolVersion")->asString() : "";
        for (const char* v : kProtocolVersions)
          if (asked == v) version = asked;
      }
      json::Value caps = json::Value::makeObject();
      json::Value toolCaps = json::Value::makeObject();
      toolCaps.set("listChanged", false);
      caps.set("tools", toolCaps);
      json::Value info = json::Value::makeObject();
      info.set("name", "verso");
      info.set("title", "Verso");
      info.set("version", VERSO_VERSION_STRING);
      json::Value result = json::Value::makeObject();
      result.set("protocolVersion", version);
      result.set("capabilities", caps);
      result.set("serverInfo", info);
      result.set("instructions", instructions);
      return json::serialize(rpcResult(id, result), 0);
    }
    if (method == "ping") return json::serialize(rpcResult(id, json::Value::makeObject()), 0);
    if (method == "tools/list") return json::serialize(rpcResult(id, tools), 0);
    if (method == "tools/call") {
      const std::string tool = params && params->get("name") ? params->get("name")->asString() : "";
      bool known = false;
      if (const json::Value* list = tools.get("tools"))
        for (const auto& t : list->array) known = known || (t.get("name") && t.get("name")->asString() == tool);
      if (!known) return json::serialize(rpcError(id, -32602, "Unknown tool: " + tool), 0);
      const json::Value* args = params->get("arguments");
      json::Value result = callTool(tool, args && args->isObject() ? *args : json::Value::makeObject());
      if (result.type == json::Value::Type::Null)
        return json::serialize(rpcError(id, -32603, "Verso did not complete the call (it is closing or busy)."), 0);
      return json::serialize(rpcResult(id, result), 0);
    }
    return json::serialize(rpcError(id, -32601, "Method not found: " + method), 0);
  }

#ifdef _WIN32
  static void serve(std::shared_ptr<Shared> self, HANDLE pipe) {
    ++self->clientCount;
    std::string buf;
    std::vector<char> chunk(kBufferSize);
    Overlapped rd, wr;
    for (;;) {
      DWORD got = 0;
      ResetEvent(rd.ov.hEvent);
      if (!rd.finish(pipe, ReadFile(pipe, chunk.data(), kBufferSize, nullptr, &rd.ov), got, self->stopEvent) || got == 0)
        break;
      buf.append(chunk.data(), got);
      bool ok = true;
      size_t nl;
      while (ok && (nl = buf.find('\n')) != std::string::npos) {
        std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        std::string reply = self->handle(line);
        if (reply.empty()) continue;
        reply += '\n';
        DWORD wrote = 0;
        ResetEvent(wr.ov.hEvent);
        ok = wr.finish(pipe, WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), nullptr, &wr.ov), wrote,
                       self->stopEvent);
      }
      if (!ok || buf.size() > kMaxMessage) break;  // a closed client, or a message without an end
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    --self->clientCount;
  }

  static void listen(std::shared_ptr<Shared> self, HANDLE first) {
    HANDLE pipe = first;
    Overlapped conn;
    while (!self->stopping) {
      if (!pipe) pipe = self->createInstance(false);
      if (pipe == INVALID_HANDLE_VALUE) break;
      ResetEvent(conn.ov.hEvent);
      DWORD unused = 0;
      BOOL started = ConnectNamedPipe(pipe, &conn.ov);
      const DWORD err = started ? 0 : GetLastError();
      bool connected = err == ERROR_PIPE_CONNECTED ||  // the client connected before the call
                       (!self->stopping && conn.finish(pipe, started, unused, self->stopEvent));
      if (self->stopping) {
        CloseHandle(pipe);
        break;
      }
      if (!connected) {
        CloseHandle(pipe);
        pipe = nullptr;
        continue;
      }
      std::thread(&Shared::serve, self, pipe).detach();
      pipe = nullptr;
    }
  }
#else
  static void serve(std::shared_ptr<Shared> self, int fd) {
    ++self->clientCount;
    std::string buf;
    std::vector<char> chunk(kBufferSize);
    for (;;) {
      if (!waitFor(fd, POLLIN, self->stopFd)) break;
      ssize_t got = recv(fd, chunk.data(), chunk.size(), 0);
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) break;
      buf.append(chunk.data(), static_cast<size_t>(got));
      bool ok = true;
      size_t nl;
      while (ok && (nl = buf.find('\n')) != std::string::npos) {
        std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        std::string reply = self->handle(line);
        if (reply.empty()) continue;
        reply += '\n';
        for (size_t off = 0; ok && off < reply.size();) {
          ssize_t n = send(fd, reply.data() + off, reply.size() - off, MSG_NOSIGNAL);
          if (n > 0)
            off += static_cast<size_t>(n);
          else if (n < 0 && errno == EINTR)
            continue;
          else
            ok = false;
        }
      }
      if (!ok || buf.size() > kMaxMessage) break;  // a closed client, or a message without an end
    }
    close(fd);
    --self->clientCount;
  }

  static void listen(std::shared_ptr<Shared> self, int listenFd) {
    while (!self->stopping) {
      if (!waitFor(listenFd, POLLIN, self->stopFd)) break;
      int fd = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd < 0) continue;
      // Only the user running Verso may drive it (the socket's folder is private too).
      ucred cred{};
      socklen_t len = sizeof cred;
      if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != getuid()) {
        close(fd);
        continue;
      }
      std::thread(&Shared::serve, self, fd).detach();
    }
    close(listenFd);
    unlink(self->path.c_str());
  }
#endif
};

McpServer::McpServer() = default;

McpServer::~McpServer() { stop(); }

bool McpServer::start(HWND notify, UINT message, json::Value tools, const std::string& instructions,
                      std::wstring* error) {
  stop();
  auto shared = std::make_shared<Shared>();
  shared->notify = notify;
  shared->message = message;
  shared->tools = std::move(tools);
  shared->instructions = instructions;
#ifdef _WIN32
  shared->sd = userOnlyDescriptor();
  if (!shared->sd || !shared->stopEvent) {
    if (error) *error = L"Could not restrict the MCP pipe to the current user.";
    return false;
  }
  shared->name = L"\\\\.\\pipe\\verso-mcp-" + std::to_wstring(GetCurrentProcessId());
  HANDLE first = shared->createInstance(true);  // fails if another process already holds the name
  if (first == INVALID_HANDLE_VALUE) {
    if (error) *error = L"Could not create the MCP pipe " + shared->name + L".";
    return false;
  }
  shared_ = shared;
  pipeName_ = shared->name;
  std::thread(&Shared::listen, shared, first).detach();
  return true;
#else
  const std::string dir = socketDir();
  if (dir.empty() || shared->stopFd < 0) {
    if (error) *error = L"Could not find a private folder for the MCP socket.";
    return false;
  }
  shared->path = dir + "/verso-mcp-" + std::to_string(getpid()) + ".sock";
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (shared->path.size() >= sizeof addr.sun_path) {
    if (error) *error = L"The MCP socket path is too long.";
    return false;
  }
  memcpy(addr.sun_path, shared->path.c_str(), shared->path.size() + 1);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  unlink(shared->path.c_str());  // left by an earlier process that had this pid
  const mode_t oldMask = umask(077);
  const bool bound = fd >= 0 && bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
  umask(oldMask);
  if (!bound || ::listen(fd, 8) != 0) {
    if (fd >= 0) close(fd);
    if (error) *error = L"Could not create the MCP socket " + utf8ToWide(shared->path) + L".";
    return false;
  }
  shared->name = utf8ToWide(shared->path);
  shared_ = shared;
  pipeName_ = shared->name;
  std::thread(&Shared::listen, shared, fd).detach();
  return true;
#endif
}

void McpServer::stop() {
  if (!shared_) return;
  auto shared = shared_;
  shared_.reset();
  pipeName_.clear();
  shared->notify = nullptr;
  {
    std::lock_guard<std::mutex> l(shared->m);
    shared->stopping = true;
  }
#ifdef _WIN32
  SetEvent(shared->stopEvent);  // ends the listener's and every connection's wait
#else
  const uint64_t one = 1;
  if (write(shared->stopFd, &one, sizeof one) < 0) {
    // The eventfd cannot overflow on a single write; nothing else to do.
  }
#endif
  shared->cv.notify_all();
}

void McpServer::runPending(const ToolRunner& run) {
  if (!shared_) return;
  for (;;) {
    std::shared_ptr<Call> call;
    {
      std::lock_guard<std::mutex> l(shared_->m);
      if (shared_->queue.empty()) break;
      call = shared_->queue.front();
      shared_->queue.pop_front();
    }
    json::Value result = run(call->tool, call->args);
    {
      std::lock_guard<std::mutex> l(shared_->m);
      call->result = std::move(result);
      call->done = true;
    }
    shared_->cv.notify_all();
  }
}

int McpServer::clientCount() const { return shared_ ? shared_->clientCount.load() : 0; }

}  // namespace verso
