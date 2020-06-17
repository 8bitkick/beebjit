#include "os_fault.h"

#include "util.h"

#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static void (*s_p_fault_callback)(uintptr_t*, uintptr_t, int, int, uintptr_t);

static void
linux_sigsegv_handler(int signum, siginfo_t* p_siginfo, void* p_void) {
  ucontext_t* p_context;
  uintptr_t host_fault_addr;
  uintptr_t host_exception_flags;
  uintptr_t host_rdi;

  /* Crash unless it's fault type we expected. */
  if (signum != SIGSEGV || p_siginfo->si_code != SEGV_ACCERR) {
    os_fault_bail();
  }

  host_fault_addr = (uintptr_t) p_siginfo->si_addr;
  p_context = (ucontext_t*) p_void;
  host_exception_flags = p_context->uc_mcontext.gregs[REG_ERR];
  host_rdi = p_context->uc_mcontext.gregs[REG_RDI];

  s_p_fault_callback((uintptr_t*) &p_context->uc_mcontext.gregs[REG_RIP],
                     host_fault_addr,
                     !!(host_exception_flags & 16),
                     !!(host_exception_flags & 2),
                     host_rdi);
}

void
os_fault_register_handler(
    void (*p_fault_callback)(uintptr_t* p_host_rip,
                             uintptr_t host_fault_addr,
                             int is_exec,
                             int is_write,
                             uintptr_t host_rdi)) {
  struct sigaction sa;
  struct sigaction sa_prev;
  int ret;

  s_p_fault_callback = p_fault_callback;

  (void) memset(&sa, '\0', sizeof(sa));
  (void) memset(&sa_prev, '\0', sizeof(sa_prev));

  sa.sa_sigaction = linux_sigsegv_handler;
  sa.sa_flags = (SA_SIGINFO | SA_NODEFER);
  ret = sigaction(SIGSEGV, &sa, &sa_prev);
  if (ret != 0) {
    util_bail("sigaction failed");
  }
  if ((sa_prev.sa_sigaction != NULL) &&
      (sa_prev.sa_sigaction != linux_sigsegv_handler)) {
    util_bail("conflicting SIGSEGV handler");
  }
}

void
os_fault_bail(void) {
  struct sigaction sa;

  (void) memset(&sa, '\0', sizeof(sa));
  sa.sa_handler = SIG_DFL;
  (void) sigaction(SIGSEGV, &sa, NULL);

  (void) raise(SIGSEGV);
  _exit(1);
}
