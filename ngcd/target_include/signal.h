#ifndef NGCD_TARGET_SIGNAL_H
#define NGCD_TARGET_SIGNAL_H

typedef int sig_atomic_t;
typedef struct {
    unsigned long words[16];
} sigset_t;

typedef void (*ngcd_signal_handler)(int);

struct sigaction {
    ngcd_signal_handler sa_handler;
    sigset_t sa_mask;
    int sa_flags;
    int padding;
    void (*sa_restorer)(void);
};

#define SIGINT 2
#define SIGILL 4
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGTERM 15
#define SIG_IGN ((ngcd_signal_handler)1)

extern int sigaction(int signal_number, const struct sigaction *action,
                     struct sigaction *old_action);
extern int sigemptyset(sigset_t *set);

#endif
