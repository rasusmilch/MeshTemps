#ifndef SERIAL_CONSOLE_H_
#define SERIAL_CONSOLE_H_

#include <Arduino.h>
#include <vector>

class SerialConsole {
public:
  // Handler receives a user-provided context, argc, argv tokens (argv[0] is the
  // command name). Handlers print to 'out'.
  typedef void (*CommandHandler)(void* ctx, int argc, const String argv[],
                                 Print& out);

  struct Command {
    const char* name;        // required, case-sensitive
    const char* help;        // optional (may be nullptr)
    CommandHandler handler;  // required
    void* user_ctx;          // optional (may be nullptr)
  };

  explicit SerialConsole(size_t max_line_len = 256)
    : max_line_len_(max_line_len) {
    buffer_.reserve(max_line_len_);
  }

  // Register a command (e.g., "debug", "units", etc.).
  bool RegisterCommand(const char* name,
                       CommandHandler handler,
                       const char* help = nullptr,
                       void* user_ctx = nullptr) {
    if (name == nullptr || handler == nullptr) return false;
    // Reject duplicates.
    for (const auto& cmd : commands_) {
      if (strcmp(cmd.name, name) == 0) return false;
    }
    commands_.push_back(Command{ name, help, handler, user_ctx });
    return true;
  }

  // Non-blocking: feed from 'in' (e.g., Serial), write responses to 'out'.
  // On blank line: prints "ok".
  void Poll(Stream& in = Serial, Print& out = Serial) {
    while (in.available() > 0) {
      const char ch = static_cast<char>(in.read());
      if (ch == '\r') continue;

      if (ch != '\n') {
        buffer_ += ch;
        if (buffer_.length() > max_line_len_) {
          // Keep tail within limit (simple ring-behavior).
          const int excess =
            static_cast<int>(buffer_.length()) - static_cast<int>(max_line_len_);
          buffer_.remove(0, excess);
        }
        continue;
      }

      // Got a line.
      buffer_.trim();
      if (buffer_.length() == 0) {
        out.println(F("ok"));
        buffer_ = "";
        continue;
      }

      // Tokenize and dispatch.
      tokens_.clear();
      Tokenize(buffer_, tokens_);
      Dispatch(tokens_, out);
      buffer_ = "";
    }
  }

  // Print "help" for all registered commands.
  void PrintHelp(Print& out = Serial) const {
    out.println(F("Commands:"));
    for (const auto& cmd : commands_) {
      out.print(F("  "));
      out.print(cmd.name);
      if (cmd.help && cmd.help[0]) {
        out.print(F("  - "));
        out.print(cmd.help);
      }
      out.println();
    }
  }

private:
  static void Tokenize(const String& line, std::vector<String>& out_tokens) {
    out_tokens.clear();
    String current;
    current.reserve(32);

    enum State { kDefault,
                 kInQuote } state = kDefault;

    for (size_t i = 0; i < line.length(); ++i) {
      char c = line[i];

      if (state == kDefault) {
        if (isspace(static_cast<unsigned char>(c))) {
          if (current.length()) {
            out_tokens.push_back(current);
            current = "";
          }
          continue;
        }
        if (c == '"') {
          state = kInQuote;
          continue;
        }
        if (c == '\\' && (i + 1) < line.length()) {
          char n = line[i + 1];
          if (n == '"' || n == '\\' || n == ' ') {
            current += n;
            ++i;
            continue;
          }
        }
        current += c;
      } else {  // kInQuote
        if (c == '"') {
          state = kDefault;
          continue;
        }
        if (c == '\\' && (i + 1) < line.length()) {
          char n = line[i + 1];
          if (n == '"' || n == '\\') {
            current += n;
            ++i;
            continue;
          }
        }
        current += c;
      }
    }
    if (current.length()) out_tokens.push_back(current);
  }

  void Dispatch(const std::vector<String>& tokens, Print& out) {
    if (tokens.empty()) {
      out.println(F("ok"));
      return;
    }

    const String& cmd_name = tokens[0];

    // Built-ins: help / ?
    if (cmd_name == "help" || cmd_name == "?") {
      PrintHelp(out);
      return;
    }

    for (const auto& cmd : commands_) {
      if (cmd_name.equals(cmd.name)) {
        // Prepare argv array (argv[0] is command name).
        // We reuse tokens vector storage to avoid copies.
        const int argc = static_cast<int>(tokens.size());
        argv_cache_.clear();
        argv_cache_.reserve(argc);
        for (const auto& t : tokens) argv_cache_.push_back(t);

        // Provide a stable pointer array expected by many C-style handlers.
        argv_ptrs_.clear();
        argv_ptrs_.reserve(argc);
        for (const auto& s : argv_cache_) argv_ptrs_.push_back(s);

        cmd.handler(cmd.user_ctx, argc, argv_ptrs_.data(), out);
        return;
      }
    }

    out.println(F("ERR (help)"));
  }

  // Storage
  std::vector<Command> commands_;
  String buffer_;
  size_t max_line_len_;

  // Token and argv caches reused per line to avoid heap churn.
  std::vector<String> tokens_;
  std::vector<String> argv_cache_;
  std::vector<String> argv_ptrs_;
};

#endif  // SERIAL_CONSOLE_H_
