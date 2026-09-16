// Fabric Epoch 1.0.0 - Summon Software Labs
#include "test_harness.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

#include "fabric_epoch/error.hpp"
#include "fabric_epoch/identity.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
extern char** environ;
#endif

namespace fe_test {
namespace {

std::string quote(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '"') {
      out += "\\\"";
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

[[nodiscard]] std::uint64_t entropy_identifier() {
  std::array<std::byte, 16> bytes{};
  fabric_epoch::detail::fill_random_16(bytes.data());
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i]));
  }
  return value;
}

}  // namespace

void fail(const char* file, int line, const std::string& expression, const std::string& detail) {
  std::ostringstream message;
  message << file << ':' << line << ": assertion failed: " << expression;
  if (!detail.empty()) {
    message << " [" << detail << ']';
  }
  throw Failure(message.str());
}

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Registrar::Registrar(const char* suite, const char* name, std::function<void()> body) {
  registry().push_back(TestCase{suite, name, std::move(body)});
}

int run_all(const std::string& filter, const std::string& skip) {
  std::vector<TestCase> cases = registry();
  std::stable_sort(cases.begin(), cases.end(), [](const TestCase& lhs, const TestCase& rhs) {
    if (lhs.suite != rhs.suite) {
      return lhs.suite < rhs.suite;
    }
    return lhs.name < rhs.name;
  });

  int executed = 0;
  int failures = 0;
  for (const auto& test : cases) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    if (!skip.empty() && full.find(skip) != std::string::npos) {
      continue;
    }
    ++executed;
    try {
      test.body();
      std::cout << "[ PASS ] " << full << std::endl;
    } catch (const Failure& error) {
      ++failures;
      std::cout << "[ FAIL ] " << full << ": " << error.what() << std::endl;
    } catch (const fabric_epoch::EpochError& error) {
      ++failures;
      std::cout << "[ FAIL ] " << full << ": unexpected EpochError " << error.render() << std::endl;
    } catch (const std::exception& error) {
      ++failures;
      std::cout << "[ FAIL ] " << full << ": unexpected exception " << error.what() << std::endl;
    }
  }
  std::cout << "executed=" << executed << " failures=" << failures << std::endl;
  if (executed == 0) {
    std::cout << "[ FAIL ] no test cases were executed" << std::endl;
    return 1;
  }
  return failures;
}

TempDirectory::TempDirectory() {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  if (error) {
    throw Failure("cannot resolve the system temporary directory");
  }
  std::ostringstream name;
  name << "fabric_epoch_test_" << entropy_identifier();
  path_ = base / name.str();
  std::filesystem::create_directories(path_, error);
  if (error) {
    throw Failure("cannot create the test temporary directory");
  }
}

TempDirectory::~TempDirectory() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::filesystem::path TempDirectory::file(const std::string& name) const { return path_ / name; }

#if defined(_WIN32)

ChildProcess::ChildProcess(const ChildProcessOptions& options) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE child_stdout = nullptr;
  HANDLE parent_read = nullptr;
  if (::CreatePipe(&parent_read, &child_stdout, &attributes, 0) == 0) {
    throw Failure("CreatePipe failed");
  }
  if (::SetHandleInformation(parent_read, HANDLE_FLAG_INHERIT, 0) == 0) {
    ::CloseHandle(parent_read);
    ::CloseHandle(child_stdout);
    throw Failure("SetHandleInformation failed");
  }

  std::string command_line = quote(options.executable.string());
  for (const auto& argument : options.arguments) {
    command_line += " ";
    command_line += quote(argument);
  }
  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = child_stdout;
  startup.hStdError = child_stdout;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION information{};
  const BOOL created = ::CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information);
  ::CloseHandle(child_stdout);
  if (created == 0) {
    ::CloseHandle(parent_read);
    throw Failure("CreateProcess failed for " + options.executable.string());
  }
  ::CloseHandle(information.hThread);
  process_ = information.hProcess;
  process_id_ = information.dwProcessId;
  stdout_read_ = parent_read;
}

void ChildProcess::close_handles() noexcept {
  if (stdout_read_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(stdout_read_));
    stdout_read_ = nullptr;
  }
  if (process_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
}

ChildProcess::~ChildProcess() {
  if (process_ != nullptr) {
    terminate();
  }
  close_handles();
}

bool ChildProcess::running() {
  if (exited_) {
    return false;
  }
  const DWORD status = ::WaitForSingleObject(static_cast<HANDLE>(process_), 0);
  if (status == WAIT_OBJECT_0) {
    DWORD code = 0;
    ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    exit_code_ = static_cast<int>(code);
    exited_ = true;
    return false;
  }
  return true;
}

std::optional<std::string> ChildProcess::try_read_line(std::uint32_t budget_milliseconds) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_milliseconds);
  for (;;) {
    const std::size_t newline = pending_.find('\n');
    if (newline != std::string::npos) {
      std::string line = pending_.substr(0, newline);
      pending_.erase(0, newline + 1);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
      }
      return line;
    }
    if (eof_) {
      return std::nullopt;
    }
    DWORD available = 0;
    if (::PeekNamedPipe(static_cast<HANDLE>(stdout_read_), nullptr, 0, nullptr, &available, nullptr) == 0) {
      eof_ = true;
      return std::nullopt;
    }
    if (available > 0) {
      std::array<char, 4096> buffer{};
      DWORD read = 0;
      if (::ReadFile(static_cast<HANDLE>(stdout_read_), buffer.data(),
                     static_cast<DWORD>(std::min<std::size_t>(available, buffer.size())), &read, nullptr) == 0 ||
          read == 0) {
        eof_ = true;
        return std::nullopt;
      }
      pending_.append(buffer.data(), read);
      continue;
    }
    if (!running()) {
      // Drain anything still buffered before reporting end of stream.
      DWORD remaining = 0;
      if (::PeekNamedPipe(static_cast<HANDLE>(stdout_read_), nullptr, 0, nullptr, &remaining, nullptr) != 0 &&
          remaining > 0) {
        continue;
      }
      eof_ = true;
      return std::nullopt;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return std::nullopt;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void ChildProcess::terminate() {
  if (process_ == nullptr) {
    return;
  }
  if (!exited_) {
    ::TerminateProcess(static_cast<HANDLE>(process_), 1);
    ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    exited_ = true;
  }
}

std::optional<int> ChildProcess::try_wait(std::uint32_t budget_milliseconds) {
  if (exited_) {
    return exit_code_;
  }
  const DWORD status = ::WaitForSingleObject(static_cast<HANDLE>(process_), budget_milliseconds);
  if (status != WAIT_OBJECT_0) {
    return std::nullopt;
  }
  DWORD code = 0;
  ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  exit_code_ = static_cast<int>(code);
  exited_ = true;
  return exit_code_;
}

#else

ChildProcess::ChildProcess(const ChildProcessOptions& options) {
  int descriptors[2];
  if (::pipe(descriptors) != 0) {
    throw Failure("pipe failed");
  }
  std::vector<std::string> storage;
  storage.push_back(options.executable.string());
  for (const auto& argument : options.arguments) {
    storage.push_back(argument);
  }
  std::vector<char*> argv;
  for (auto& entry : storage) {
    argv.push_back(entry.data());
  }
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, descriptors[0]);
  posix_spawn_file_actions_addclose(&actions, descriptors[1]);

  pid_t pid = 0;
  const int result = ::posix_spawn(&pid, storage[0].c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(descriptors[1]);
  if (result != 0) {
    ::close(descriptors[0]);
    throw Failure("posix_spawn failed for " + options.executable.string());
  }
  process_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
  process_id_ = static_cast<std::uint32_t>(pid);
  stdout_read_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptors[0]));
}

void ChildProcess::close_handles() noexcept {
  if (stdout_read_ != nullptr) {
    ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_)));
    stdout_read_ = nullptr;
  }
  process_ = nullptr;
}

ChildProcess::~ChildProcess() {
  if (process_ != nullptr) {
    terminate();
  }
  close_handles();
}

bool ChildProcess::running() {
  if (exited_) {
    return false;
  }
  int status = 0;
  const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
  const pid_t result = ::waitpid(pid, &status, WNOHANG);
  if (result == pid) {
    exited_ = true;
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return false;
  }
  return true;
}

std::optional<std::string> ChildProcess::try_read_line(std::uint32_t budget_milliseconds) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_milliseconds);
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_));
  for (;;) {
    const std::size_t newline = pending_.find('\n');
    if (newline != std::string::npos) {
      std::string line = pending_.substr(0, newline);
      pending_.erase(0, newline + 1);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
      }
      return line;
    }
    if (eof_) {
      return std::nullopt;
    }
    char buffer[4096];
    const ssize_t read = ::read(descriptor, buffer, sizeof(buffer));
    if (read > 0) {
      pending_.append(buffer, static_cast<std::size_t>(read));
      continue;
    }
    if (read == 0) {
      eof_ = true;
      return std::nullopt;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      eof_ = true;
      return std::nullopt;
    }
    if (!running()) {
      eof_ = true;
      return std::nullopt;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return std::nullopt;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void ChildProcess::terminate() {
  if (process_ == nullptr) {
    return;
  }
  if (!exited_) {
    const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    exited_ = true;
  }
}

std::optional<int> ChildProcess::try_wait(std::uint32_t budget_milliseconds) {
  if (exited_) {
    return exit_code_;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_milliseconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!running()) {
      return exit_code_;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return std::nullopt;
}

#endif

}  // namespace fe_test
