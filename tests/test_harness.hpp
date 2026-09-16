// Fabric Epoch 1.0.0 - Summon Software Labs
// Minimal, dependency-free test harness.
//
// No framework watchdog or timeout is used: a hanging test is a defect and must
// surface as a hang, not be masked by a timer. Bounded waits exist only where a
// real external process must be observed; exceeding such a bound produces an
// explicit failed assertion with a diagnostic, never a silent pass.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fe_test {

class Failure : public std::runtime_error {
 public:
  explicit Failure(const std::string& message) : std::runtime_error(message) {}
};

[[noreturn]] void fail(const char* file, int line, const std::string& expression,
                       const std::string& detail);

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body);
};

/// Runs every registered case, optionally filtered by substring and optionally
/// skipping cases whose "suite.name" contains the skip substring. Returns the
/// number of failures. Running zero cases is a failure.
int run_all(const std::string& filter, const std::string& skip = std::string());

/// Unique-per-process temporary directory under the operating-system temp
/// area. Identity uses operating-system entropy so that concurrent processes
/// never collide and a process-local counter is never the sole identity.
class TempDirectory {
 public:
  TempDirectory();
  ~TempDirectory();

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path file(const std::string& name) const;

 private:
  std::filesystem::path path_;
};

struct ChildProcessOptions {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
};

/// Real operating-system child process with captured stdout. The destructor
/// always terminates and reaps the child, so a failing assertion cannot leave
/// an orphan process behind.
class ChildProcess {
 public:
  explicit ChildProcess(const ChildProcessOptions& options);
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  [[nodiscard]] bool running();
  [[nodiscard]] std::uint32_t process_id() const noexcept { return process_id_; }

  /// Waits up to the budget for one complete line of stdout. Returns
  /// std::nullopt when the budget expires or the child exits without producing
  /// a line. Callers must turn std::nullopt into an explicit assertion failure.
  [[nodiscard]] std::optional<std::string> try_read_line(std::uint32_t budget_milliseconds);

  /// Terminates the child with an unconditional operating-system termination
  /// path (no graceful shutdown, no cooperative signal).
  void terminate();

  /// Waits for natural exit and returns the exit code, or std::nullopt when the
  /// budget expires.
  [[nodiscard]] std::optional<int> try_wait(std::uint32_t budget_milliseconds);

 private:
  void close_handles() noexcept;

  void* process_ = nullptr;
  void* thread_ = nullptr;
  void* stdout_read_ = nullptr;
  std::uint32_t process_id_ = 0;
  bool exited_ = false;
  int exit_code_ = 0;
  std::string pending_;
  bool eof_ = false;
};

}  // namespace fe_test

#define FE_TEST(suite, name)                                                            static void fe_test_body_##suite##_##name();                                          static const ::fe_test::Registrar fe_test_registrar_##suite##_##name(                     #suite, #name, fe_test_body_##suite##_##name);                                    static void fe_test_body_##suite##_##name()

#define FE_REQUIRE(expression)                                                          do {                                                                                    if (!(expression)) {                                                                    ::fe_test::fail(__FILE__, __LINE__, #expression, "");                               }                                                                                   } while (false)

#define FE_REQUIRE_MSG(expression, detail)                                              do {                                                                                    if (!(expression)) {                                                                    ::fe_test::fail(__FILE__, __LINE__, #expression, (detail));                         }                                                                                   } while (false)

#define FE_REQUIRE_EQ(lhs, rhs)                                                         do {                                                                                    const auto& fe_lhs = (lhs);                                                           const auto& fe_rhs = (rhs);                                                           if (!(fe_lhs == fe_rhs)) {                                                              ::fe_test::fail(__FILE__, __LINE__, #lhs " == " #rhs, "");                          }                                                                                   } while (false)

#define FE_REQUIRE_THROWS(expression)                                                   do {                                                                                    bool fe_threw = false;                                                                try {                                                                                   (void)(expression);                                                                 } catch (const std::exception&) {                                                       fe_threw = true;                                                                    }                                                                                     if (!fe_threw) {                                                                        ::fe_test::fail(__FILE__, __LINE__, #expression " throws", "no exception");         }                                                                                   } while (false)

#define FE_REQUIRE_NO_THROW(expression)                                                 do {                                                                                    try {                                                                                   (void)(expression);                                                                 } catch (const std::exception& fe_error) {                                              ::fe_test::fail(__FILE__, __LINE__, #expression " does not throw", fe_error.what()); \
    }                                                                                   } while (false)

namespace fe_test {

/// Deterministic, seeded generator used by property and adversarial suites.
class DeterministicRandom {
 public:
  explicit DeterministicRandom(std::uint64_t seed)
      : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed), seed_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  [[nodiscard]] std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

  [[nodiscard]] std::uint64_t next_below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }

  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

 private:
  std::uint64_t state_;
  std::uint64_t seed_ = 0;
};

}  // namespace fe_test
