// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/store.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>

#include "codec.hpp"
#include "fabric_epoch/detail/sha256.hpp"
#include "fabric_epoch/error.hpp"
#include "fabric_epoch/identity.hpp"
#include "fabric_epoch/version.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace fabric_epoch {
namespace {

constexpr std::uint32_t kStoreMagic = 0x46424550u;  // "FBEP"
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kDigestSize = 32;
/// Defence-in-depth ceiling on a declared payload length, applied before the
/// image is trusted. It is deliberately generous: real images are sized by the
/// durable participant population, not by a transport-oriented limit.
constexpr std::uint32_t kAbsoluteMaxPayloadLength = 1u << 30;  // 1 GiB

[[nodiscard]] std::uint32_t read_u32_le(const std::byte* data) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[0])) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[3])) << 24);
}

void write_u32_le(std::byte* data, std::uint32_t value) noexcept {
  data[0] = static_cast<std::byte>(value & 0xFFu);
  data[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
  data[2] = static_cast<std::byte>((value >> 16) & 0xFFu);
  data[3] = static_cast<std::byte>((value >> 24) & 0xFFu);
}

[[nodiscard]] bool file_exists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

[[nodiscard]] std::vector<std::byte> read_all(const std::filesystem::path& path) {
  std::vector<std::byte> out;
#if defined(_WIN32)
  const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw io_failure("cannot open durable store for reading");
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(handle, &size) == 0) {
    ::CloseHandle(handle);
    throw io_failure("cannot size durable store");
  }
  if (size.QuadPart < 0) {
    ::CloseHandle(handle);
    throw corrupt("durable store reports a negative size");
  }
  out.resize(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < out.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(out.size() - offset, 1u << 20));
    DWORD read = 0;
    if (::ReadFile(handle, out.data() + offset, chunk, &read, nullptr) == 0 || read == 0) {
      ::CloseHandle(handle);
      throw io_failure("short read while loading durable store");
    }
    offset += read;
  }
  ::CloseHandle(handle);
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    throw io_failure("cannot open durable store for reading");
  }
  std::byte buffer[65536];
  for (;;) {
    const ssize_t got = ::read(descriptor, buffer, sizeof(buffer));
    if (got < 0) {
      ::close(descriptor);
      throw io_failure("read failure while loading durable store");
    }
    if (got == 0) {
      break;
    }
    out.insert(out.end(), buffer, buffer + got);
  }
  ::close(descriptor);
#endif
  return out;
}

void write_all_durable(const std::filesystem::path& path, std::span<const std::byte> data) {
#if defined(_WIN32)
  const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw io_failure("cannot create temporary durable store file");
  }
  std::size_t offset = 0;
  while (offset < data.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 1u << 20));
    DWORD written = 0;
    if (::WriteFile(handle, data.data() + offset, chunk, &written, nullptr) == 0 || written == 0) {
      ::CloseHandle(handle);
      throw io_failure("short write while persisting durable store");
    }
    offset += written;
  }
  if (::FlushFileBuffers(handle) == 0) {
    ::CloseHandle(handle);
    throw io_failure("durability barrier failed while persisting durable store");
  }
  ::CloseHandle(handle);
#else
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (descriptor < 0) {
    throw io_failure("cannot create temporary durable store file");
  }
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t written = ::write(descriptor, data.data() + offset, data.size() - offset);
    if (written <= 0) {
      ::close(descriptor);
      throw io_failure("short write while persisting durable store");
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    ::close(descriptor);
    throw io_failure("durability barrier failed while persisting durable store");
  }
  ::close(descriptor);
#endif
}

void atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
#if defined(_WIN32)
  if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    throw io_failure("atomic replacement of the durable store failed");
  }
#else
  if (::rename(from.c_str(), to.c_str()) != 0) {
    throw io_failure("atomic replacement of the durable store failed");
  }
#endif
}

void remove_quietly(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
  ::DeleteFileW(path.c_str());
#else
  ::unlink(path.c_str());
#endif
}

[[nodiscard]] std::uint64_t process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

/// RAII guard that removes a temporary file if it is still present when the
/// scope exits, so a failed persist never leaves debris behind.
class TempFileGuard {
 public:
  explicit TempFileGuard(std::filesystem::path path) : path_(std::move(path)) {}
  ~TempFileGuard() {
    if (!released_) {
      remove_quietly(path_);
    }
  }
  TempFileGuard(const TempFileGuard&) = delete;
  TempFileGuard& operator=(const TempFileGuard&) = delete;
  void release() noexcept { released_ = true; }

 private:
  std::filesystem::path path_;
  bool released_ = false;
};

/// Fresh random suffix for a temporary file name. Uses operating-system
/// entropy so that concurrent processes in the same directory never collide
/// and a process-local counter is never the sole identity of a temp name.
[[nodiscard]] std::string temp_suffix() {
  std::array<std::byte, 16> entropy{};
  detail::fill_random_16(entropy.data());
  std::string out = "-";
  out += std::to_string(process_id());
  out += "-";
  out += detail::to_hex(entropy.data(), 4);
  return out;
}

[[nodiscard]] std::string trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\r' || text[begin] == '\n')) {
    ++begin;
  }
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\r' || text[end - 1] == '\n')) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

}  // namespace

EpochStore::EpochStore(StoreOptions options) : options_(std::move(options)) {
  if (!is_viable(options_.limits)) {
    throw invalid_argument("store limits are not viable");
  }
  if (options_.path.empty()) {
    throw invalid_argument("store path must not be empty");
  }
}

bool EpochStore::exists() const { return file_exists(options_.path); }

std::filesystem::path EpochStore::watermark_path() const {
  std::filesystem::path path = options_.path;
  path += ".watermark";
  return path;
}

DurableState EpochStore::initial_state() const {
  DurableState state;
  state.current_epoch = CoordinatorEpoch(0);
  state.coordinator_boot = CoordinatorBootId{};
  state.generation = EpochGeneration(0);
  state.registration_floor = RegistrationGeneration(0);
  state.fence_floor = FencingGeneration(0);
  return state;
}

std::vector<std::byte> EpochStore::encode(const DurableState& state) const {
  validate_durable_state(state);
  const std::vector<std::byte> payload = detail::encode_durable_payload(state);
  if (payload.size() > static_cast<std::size_t>(UINT32_MAX)) {
    throw resource_limit("durable payload exceeds the representable length");
  }
  std::vector<std::byte> image;
  image.resize(kHeaderSize);
  write_u32_le(image.data(), kStoreMagic);
  write_u32_le(image.data() + 4, persistence_format_version);
  write_u32_le(image.data() + 8, static_cast<std::uint32_t>(payload.size()));
  image.insert(image.end(), payload.begin(), payload.end());
  const auto digest = detail::Sha256::hash(std::span<const std::byte>(image));
  image.insert(image.end(), digest.begin(), digest.end());
  return image;
}

DurableState EpochStore::decode(std::span<const std::byte> image) const {
  if (image.size() < kHeaderSize) {
    throw corrupt("durable image is shorter than its header");
  }
  if (read_u32_le(image.data()) != kStoreMagic) {
    throw corrupt("durable image magic does not match Fabric Epoch");
  }
  const std::uint32_t version = read_u32_le(image.data() + 4);
  if (version != persistence_format_version) {
    throw EpochError(ErrorCategory::UnsupportedVersion, "durable image format version is not supported");
  }
  const std::uint32_t declared_length = read_u32_le(image.data() + 8);
  if (declared_length > kAbsoluteMaxPayloadLength) {
    throw resource_limit("durable payload length exceeds the absolute ceiling");
  }
  const std::size_t expected_size = kHeaderSize + static_cast<std::size_t>(declared_length) + kDigestSize;
  if (image.size() != expected_size) {
    throw corrupt("durable image length does not match its declared payload length");
  }
  const auto computed = detail::Sha256::hash(image.first(image.size() - kDigestSize));
  if (std::memcmp(computed.data(), image.data() + image.size() - kDigestSize, kDigestSize) != 0) {
    throw EpochError(ErrorCategory::IntegrityFailure, "durable image integrity digest does not match");
  }
  DurableState state = detail::decode_durable_payload(
      image.subspan(kHeaderSize, static_cast<std::size_t>(declared_length)), options_.limits);
  validate_durable_state(state);
  return state;
}

DurableState EpochStore::load() const {
  if (!exists()) {
    if (!options_.create_if_missing) {
      throw io_failure("durable store does not exist and create_if_missing is false");
    }
    return initial_state();
  }
  const std::vector<std::byte> image = read_all(options_.path);
  DurableState state = decode(std::span<const std::byte>(image));
  if (options_.enable_rollback_watermark) {
    const CoordinatorEpoch watermark = highest_persisted_epoch();
    if (state.current_epoch < watermark) {
      throw corrupt("durable epoch is lower than the persisted watermark: rollback detected");
    }
  }
  return state;
}

void EpochStore::save(const DurableState& state) const {
  // The watermark is read before anything is written: a malformed watermark
  // must fail the operation before the primary image is replaced, never after.
  const std::uint64_t previous_watermark = read_watermark();
  const std::vector<std::byte> image = encode(state);
  const std::filesystem::path directory = options_.path.parent_path();
  std::error_code error;
  if (!directory.empty() && !std::filesystem::exists(directory, error)) {
    std::filesystem::create_directories(directory, error);
    if (error) {
      throw io_failure("cannot create the durable store directory");
    }
  }

  std::filesystem::path temporary = options_.path;
  temporary += ".tmp";
  temporary += temp_suffix();
  TempFileGuard guard(temporary);

  write_all_durable(temporary, std::span<const std::byte>(image));
  atomic_replace(temporary, options_.path);
  guard.release();

  if (options_.enable_rollback_watermark && state.current_epoch.value() > previous_watermark) {
    write_watermark(state.current_epoch);
  }
}

std::uint64_t EpochStore::read_watermark() const {
  if (!options_.enable_rollback_watermark) {
    return 0;
  }
  const std::filesystem::path path = watermark_path();
  if (!file_exists(path)) {
    return 0;
  }
  const std::vector<std::byte> raw = read_all(path);
  const std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
  std::size_t first_newline = text.find('\n');
  if (first_newline == std::string::npos) {
    throw corrupt("rollback watermark file is malformed");
  }
  const std::string header = trim(text.substr(0, first_newline));
  if (header != "FabricEpochWatermark") {
    throw corrupt("rollback watermark file has an unexpected header");
  }
  const std::string value = trim(text.substr(first_newline + 1));
  if (value.empty()) {
    throw corrupt("rollback watermark file has no epoch value");
  }
  std::uint64_t epoch = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') {
      throw corrupt("rollback watermark file has a non-numeric epoch value");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (epoch > (UINT64_MAX - digit) / 10u) {
      throw overflow("rollback watermark epoch overflows");
    }
    epoch = epoch * 10u + digit;
  }
  return epoch;
}

void EpochStore::write_watermark(CoordinatorEpoch epoch) const {
  const std::string text = "FabricEpochWatermark\n" + epoch.to_string() + "\n";
  const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
  const std::span<const std::byte> payload(bytes, text.size());

  const std::filesystem::path target = watermark_path();
  std::filesystem::path temporary = target;
  temporary += ".tmp";
  temporary += temp_suffix();
  TempFileGuard guard(temporary);
  write_all_durable(temporary, payload);
  atomic_replace(temporary, target);
  guard.release();
}

CoordinatorEpoch EpochStore::highest_persisted_epoch() const {
  return CoordinatorEpoch(read_watermark());
}

}  // namespace fabric_epoch
