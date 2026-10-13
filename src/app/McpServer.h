#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

#include "util/Json.h"

namespace verso {

// The Model Context Protocol server that lets AI tools control Verso.
//
// It listens on a named pipe that only the current Windows user can open (remote
// connections are refused). Each connection speaks MCP's stdio framing: one JSON-RPC
// message per line. verso-mcp.exe relays an MCP client's stdin/stdout to this pipe.
// initialize, ping and tools/list are answered on the connection's thread; tools/call
// runs on the UI thread (the owner is told by a posted message and calls runPending()).
class McpServer {
 public:
  using ToolRunner = std::function<json::Value(const std::string& tool, const json::Value& args)>;

  McpServer();
  ~McpServer();
  McpServer(const McpServer&) = delete;
  McpServer& operator=(const McpServer&) = delete;

  // `tools` is the tools/list result ({"tools": [...]}). Returns false with `error` set.
  bool start(HWND notify, UINT message, json::Value tools, const std::string& instructions, std::wstring* error);
  void stop();
  bool running() const { return !pipeName_.empty(); }
  // The pipe's full name (\\.\pipe\verso-mcp-<pid>), given to terminals as VERSO_MCP_PIPE.
  const std::wstring& pipeName() const { return pipeName_; }
  // UI thread: executes queued tool calls. `run` returns an MCP CallToolResult.
  void runPending(const ToolRunner& run);
  // Number of clients currently connected.
  int clientCount() const;

 private:
  struct Call;
  struct Shared;
  std::shared_ptr<Shared> shared_;
  std::wstring pipeName_;
};

}  // namespace verso
