#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <unordered_set>

#include "child.h"
#include "log.h"
#include "util/paths.h"
#include "util/strings.h"

namespace bpftrace {

constexpr unsigned int maxargs = 256;
constexpr uint64_t CHILD_GO = 'g';
constexpr uint64_t CHILD_PTRACE = 'p';
constexpr unsigned int STACK_SIZE = (64 * 1024UL);

std::system_error SYS_ERROR(std::string msg)
{
  return { errno, std::generic_category(), msg };
}

static void report_status(int wstatus)
{
  std::stringstream msg;
  if (WIFSTOPPED(wstatus))
    msg << "Child stopped unexpectedly, signal: " << WSTOPSIG(wstatus);
  else if (WIFEXITED(wstatus))
    msg << "Child exited unexpectedly";
  else if (WIFSIGNALED(wstatus)) {
    if (WCOREDUMP(wstatus))
      msg << "Child core dumped";
    else
      msg << "Child aborted by signal: " << WTERMSIG(wstatus);
  }
  throw std::runtime_error(msg.str());
}

static int childfn(void* arg)
{
  auto* args = static_cast<struct child_args*>(arg);

  // Receive SIGTERM if parent dies
  if (prctl(PR_SET_PDEATHSIG, SIGTERM)) {
    perror("child: prctl(PR_SET_PDEATHSIG)");
    return 10;
  }

  // Convert vector of strings into raw array of C-strings for execve(2)
  char* argv[maxargs];
  int idx = 0;
  for (const auto& a : args->cmd) {
    argv[idx++] = const_cast<char*>(a.c_str());
  }
  argv[idx] = nullptr; // must be null terminated

  uint64_t bf;
  int ret = read(args->event_fd, &bf, sizeof(bf));
  if (ret < 0) {
    perror("child: failed to read 'go' event fd");
    return 11;
  }

  close(args->event_fd);

  if (bf == CHILD_PTRACE) {
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0)
      perror("child: ptrace(traceme) failed");
    if (kill(getpid(), SIGSTOP))
      perror("child: failed to stop");
  }

  execve(argv[0], argv, environ);

  auto err = "child: failed to execve: " + std::string(argv[0]);
  perror(err.c_str());
  return 12;
}

static void validate_cmd(std::vector<std::string>& cmd)
{
  auto paths = util::resolve_binary_path(cmd[0]);
  switch (paths.size()) {
    case 0:
      throw std::runtime_error("path '" + cmd[0] +
                               "' does not exist or is not executable");
    case 1:
      cmd[0] = paths.front();
      break;
    default:
      // /bin maybe is a symbolic link to /usr/bin (/bin -> /usr/bin), there
      // may be worse cases like:
      // $ realpath /usr/bin/ping /bin/ping /usr/sbin/ping /sbin/ping
      // /usr/bin/ping
      // /usr/bin/ping
      // /usr/bin/ping
      // /usr/bin/ping
      std::unordered_set<std::string> uniq_abs_path;
      for (const auto& path : paths) {
        auto absolute = util::abs_path(path);
        if (!absolute.has_value())
          continue;
        uniq_abs_path.insert(*absolute);
      }

      if (uniq_abs_path.size() == 1) {
        cmd[0] = paths.front();
        break;
      } else {
        throw std::runtime_error(
            "path '" + cmd[0] + "' must refer to a unique binary but matched " +
            std::to_string(paths.size()) + " binaries");
      }
  }

  if (cmd.size() >= (maxargs - 1)) {
    throw std::runtime_error("Too many arguments for command (" +
                             std::to_string(cmd.size()) + " > " +
                             std::to_string(maxargs - 1) + ")");
  }
}

ChildProc::ChildProc(std::string cmd)
{
  auto child_args = std::make_unique<struct child_args>();
  auto child_stack = std::make_unique<char[]>(STACK_SIZE);

  child_args->cmd = util::split_string(cmd, ' ');
  validate_cmd(child_args->cmd);

  int event_fd = eventfd(0, EFD_CLOEXEC);
  if (event_fd < 0) {
    SYS_ERROR("Failed to create event fd");
  }

  child_args->event_fd = event_fd;
  child_event_fd_ = event_fd;

  pid_t cpid = clone(
      childfn, child_stack.get() + STACK_SIZE, SIGCHLD, child_args.get());

  if (cpid <= 0) {
    close(event_fd);
    throw SYS_ERROR("Failed to clone child");
  }

  child_pid_ = cpid;
  state_ = State::FORKED;
}

ChildProc::~ChildProc()
{
  if (child_event_fd_ >= 0) {
    close(child_event_fd_);
  }

  if (is_alive())
    terminate(true);
}

bool ChildProc::is_alive()
{
  if (!died())
    check_child();
  return !died();
}

void ChildProc::terminate(bool force)
{
  // Make sure child didn't terminate in mean time
  check_child();
  if (died())
    return;

  if (child_pid_ <= 1)
    LOG(BUG) << "child_pid <= 1";

  int sig = force ? SIGKILL : SIGTERM;

  kill(child_pid_, sig);

  if (state_ == State::PTRACE_PAUSE)
    ptrace(PTRACE_DETACH, child_pid_, nullptr, 0);

  check_child(force);
}

void ChildProc::resume()
{
  assert(state_ == State::PTRACE_PAUSE);
  ptrace(PTRACE_DETACH, child_pid_, nullptr, 0);
}

void ChildProc::run(bool pause)
{
  if (!is_alive()) {
    throw std::runtime_error("Child died unexpectedly");
  }

  assert(state_ == State::FORKED);

  const auto* data = pause ? &CHILD_PTRACE : &CHILD_GO;
  if (write(child_event_fd_, data, sizeof(*data)) < 0) {
    close(child_event_fd_);
    terminate(true);
    throw SYS_ERROR("Failed to write 'go' event fd");
  }

  close(child_event_fd_);

  if (!pause) {
    state_ = State::RUNNING;
    return;
  }

  state_ = State::PTRACE_PAUSE;

  // After receiving the ptrace message the child will setup
  // ptrace and SIGSTOP itself.
  // we can then setup ptrace to stop the child right after execve
  // and let the child run until that point
  int wstatus;
  if (waitpid(child_pid_, &wstatus, 0) < 0) {
    if (errno == ECHILD)
      throw std::runtime_error("Child died unexpectedly");
  }

  if (!WIFSTOPPED(wstatus) || WSTOPSIG(wstatus) != SIGSTOP)
    report_status(wstatus);

  try {
    if (ptrace(PTRACE_SETOPTIONS, child_pid_, nullptr, PTRACE_O_TRACEEXEC) < 0)
      throw SYS_ERROR("Failed to PTRACE_SETOPTIONS child");

    if (ptrace(PTRACE_CONT, child_pid_, nullptr, 0) < 0)
      throw SYS_ERROR("Failed to PTRACE_CONT child");

    if (waitpid(child_pid_, &wstatus, 0) < 0)
      throw SYS_ERROR("Error while waiting for child");

    if (WIFSTOPPED(wstatus) &&
        wstatus >> 8 == (SIGTRAP | (PTRACE_EVENT_EXEC << 8)))
      return;

    report_status(wstatus);
  } catch (const std::runtime_error& e) {
    ptrace(PTRACE_DETACH, child_pid_, nullptr, 0);
    terminate(true);
    throw SYS_ERROR("Failed to write 'go' event fd");
  }
}

// private
void ChildProc::check_wstatus(int wstatus)
{
  if (WIFEXITED(wstatus))
    exit_code_ = WEXITSTATUS(wstatus);
  else if (WIFSIGNALED(wstatus))
    term_signal_ = WTERMSIG(wstatus);
  // Ignore STOP and CONT
  else
    return;

  state_ = State::DIED;
}

void ChildProc::check_child(bool block)
{
  int status = 0;

  int flags = WNOHANG;
  if (block)
    flags &= ~WNOHANG;

  pid_t ret;
  while ((ret = waitpid(child_pid_, &status, flags)) < 0 && errno == EINTR)
    ;

  if (ret < 0) {
    if (errno == EINVAL)
      LOG(BUG) << "waitpid() EINVAL";
    else {
      LOG(WARNING) << "waitpid(" << child_pid_
                   << ") returned unexpected error: " << errno
                   << ". Marking the child as dead";
      state_ = State::DIED;
      return;
    }
  }

  if (ret == 0)
    return;

  check_wstatus(status);
}

} // namespace bpftrace
