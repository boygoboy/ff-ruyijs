#ifndef _WIN32
#include "resource_network/necko_helper.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace ruyijs::resource_network {
namespace {
static_assert(sizeof(wchar_t) >= 4, "POSIX Necko paths use one wchar per code point");

std::wstring Widen(const std::string& text) {
  std::wstring wide;
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
  const size_t size = text.size();
  for (size_t i = 0; i < size;) {
    const unsigned char lead = bytes[i];
    uint32_t code = 0;
    size_t need = 1;
    if (lead < 0x80) code = lead;
    else if ((lead & 0xe0) == 0xc0) { need = 2; code = lead & 0x1f; }
    else if ((lead & 0xf0) == 0xe0) { need = 3; code = lead & 0x0f; }
    else if ((lead & 0xf8) == 0xf0) { need = 4; code = lead & 0x07; }
    else return std::wstring();
    if (i + need > size) return std::wstring();
    for (size_t extra = 1; extra < need; ++extra) {
      if ((bytes[i + extra] & 0xc0) != 0x80) return std::wstring();
      code = (code << 6) | (bytes[i + extra] & 0x3f);
    }
    wide.push_back(wchar_t(code));
    i += need;
  }
  return wide;
}
std::string Narrow(const std::wstring& wide) {
  std::string text;
  for (wchar_t unit : wide) {
    const uint32_t code = uint32_t(unit);
    if (code < 0x80) text.push_back(char(code));
    else if (code < 0x800) {
      text.push_back(char(0xc0 | (code >> 6)));
      text.push_back(char(0x80 | (code & 0x3f)));
    } else if (code < 0x10000) {
      text.push_back(char(0xe0 | (code >> 12)));
      text.push_back(char(0x80 | ((code >> 6) & 0x3f)));
      text.push_back(char(0x80 | (code & 0x3f)));
    } else if (code < 0x110000) {
      text.push_back(char(0xf0 | (code >> 18)));
      text.push_back(char(0x80 | ((code >> 12) & 0x3f)));
      text.push_back(char(0x80 | ((code >> 6) & 0x3f)));
      text.push_back(char(0x80 | (code & 0x3f)));
    } else return std::string();
  }
  return text;
}
bool Executable(const std::string& path) {
  return !path.empty() && access(path.c_str(), X_OK) == 0;
}
bool RepoRoot(std::string* root) {
  char storage[PATH_MAX];
#if defined(__APPLE__)
  uint32_t size = sizeof(storage);
  if (_NSGetExecutablePath(storage, &size) != 0) return false;
#else
  const ssize_t linked = readlink("/proc/self/exe", storage, sizeof(storage) - 1);
  if (linked <= 0) return false;
  storage[linked] = '\0';
#endif
  char resolved[PATH_MAX];
  if (!realpath(storage, resolved)) return false;
  std::string path(resolved);
  for (int depth = 0; depth < 2; ++depth) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return false;
    path.erase(slash);
  }
  *root = std::move(path);
  return true;
}
}
bool NeckoPythonPath(std::wstring* python) {
  std::string chosen;
  if (const char* overridePath = std::getenv("RUYIJS_NECKO_PYTHON")) {
    if (Executable(overridePath)) chosen = overridePath;
  }
  std::string root;
  if (chosen.empty() && RepoRoot(&root)) {
    const std::string bundled = root + "/vendor/necko-derived/python/bin/python3";
    const std::string plain = root + "/vendor/necko-derived/python/python3";
    if (Executable(bundled)) chosen = bundled;
    else if (Executable(plain)) chosen = plain;
  }
  if (!Executable(chosen)) return false;
  std::wstring wide = Widen(chosen);
  if (wide.empty()) return false;
  *python = std::move(wide);
  return true;
}
NeckoHelper::~NeckoHelper() {
  std::lock_guard<std::mutex> guard(lock_);
  KillLocked();
}
void NeckoHelper::KillLocked() {
  if (pid_ <= 0) return;
  kill(-pid_, SIGKILL);
  kill(pid_, SIGKILL);
  int status = 0;
  for (int spin = 0; spin < 500; ++spin) {
    const pid_t got = waitpid(pid_, &status, WNOHANG);
    if (got == pid_ || (got < 0 && errno == ECHILD)) {
      pid_ = 0;
      return;
    }
    usleep(10000);
  }
  waitpid(pid_, &status, WNOHANG);
  pid_ = 0;
}
bool NeckoHelper::Start(const std::wstring& python, NeckoPipe& input, NeckoPipe& output,
                        const wchar_t* mode, const TransportPartition* partition) {
  std::lock_guard<std::mutex> guard(lock_);
  if (pid_ > 0) return false;
  const int in = input.Child();
  const int out = output.Child();
  if (in < 0 || out < 0) return false;
  const std::string program = Narrow(python);
  if (!Executable(program)) return false;
  std::vector<std::string> args{program, "-I", "-B", "-m", "ruyijs_necko.oneshot"};
  if (mode && *mode) {
    std::string text = Narrow(std::wstring(mode));
    std::string token;
    for (char unit : text) {
      if (unit == ' ') {
        if (!token.empty()) {
          args.push_back(token);
          token.clear();
        }
      } else token.push_back(unit);
    }
    if (!token.empty()) args.push_back(token);
  }
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (std::string& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) return false;
  // stderr stays inherited so a failed import is visible on the shell log.
  // stdin and stdout are the framed pipes, same as the Windows helper.
  bool armed = posix_spawn_file_actions_adddup2(&actions, in, STDIN_FILENO) == 0 &&
      posix_spawn_file_actions_adddup2(&actions, out, STDOUT_FILENO) == 0;
  if (in > STDERR_FILENO)
    armed = armed && posix_spawn_file_actions_addclose(&actions, in) == 0;
  if (out > STDERR_FILENO && out != in)
    armed = armed && posix_spawn_file_actions_addclose(&actions, out) == 0;
  // Every other descriptor closes in the child as it starts: another session's
  // thread may be opening pipes or sockets at this moment, and marking them
  // close-on-exec beforehand would race it.
#if defined(__linux__)
  armed = armed && posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1) == 0;
#else
  armed = armed && posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO) == 0;
#endif
  posix_spawnattr_t attr;
  if (posix_spawnattr_init(&attr) != 0) {
    posix_spawn_file_actions_destroy(&actions);
    return false;
  }
#if defined(__linux__)
  armed = armed && posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP) == 0 &&
#else
  armed = armed && posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT) == 0 &&
#endif
      posix_spawnattr_setpgroup(&attr, 0) == 0;
  // A session's partition replaces the inherited proxy / TLS release.
  std::vector<std::string> environment;
  std::vector<char*> envp;
  if (partition && Overrides(*partition)) {
    environment = EnvironmentStrings(*partition);
    for (std::string& entry : environment) envp.push_back(entry.data());
    envp.push_back(nullptr);
  }
  pid_t child = 0;
  const int spawned = armed ? posix_spawn(&child, program.c_str(), &actions, &attr,
                                           argv.data(), envp.empty() ? environ : envp.data()) : -1;
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0 || child <= 0) return false;
  pid_ = child;
  return true;
}
void NeckoHelper::Cancel() {
  std::lock_guard<std::mutex> guard(lock_);
  KillLocked();
}
bool NeckoHelper::Finish(ULONGLONG deadline) {
  std::unique_lock<std::mutex> guard(lock_);
  const pid_t pid = pid_;
  if (pid <= 0) return false;
  guard.unlock();
  int status = 0;
  for (;;) {
    const pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid || (got < 0 && errno == ECHILD)) {
      guard.lock();
      if (pid_ == pid) pid_ = 0;
      return got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    if (!NeckoRemaining(deadline)) return false;
    usleep(10000);
  }
}
}
#else
#include "resource_network/necko_helper.h"

namespace ruyijs::resource_network {
bool NeckoPythonPath(std::wstring* python) {
  wchar_t self[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
  if (!length || length >= MAX_PATH) return false;
  python->assign(self, length);
  auto slash = python->find_last_of(L"\\/");
  if (slash == std::wstring::npos || !slash) return false;
  slash = python->find_last_of(L"\\/", slash - 1);
  if (slash == std::wstring::npos) return false;
  *python = python->substr(0, slash) + L"\\vendor\\necko-derived\\python\\python.exe";
  return true;
}
}

#endif
