/* Terminal transport for the Cocoa build: login shells in ptys, in process.
 *
 * This is where the Carbon port has to go out to a helper daemon over TCP --
 * CarbonLib exposes no POSIX, so the editor cannot start a process at all. Mac
 * OS X can, so there is nothing between the editor and the shell here: forkpty
 * gives us the master side, mac-cocoa/panel_cocoa.c renders what comes out of
 * it, and the descriptors are drained from the shim's idle hook so the editor
 * never stalls waiting on one.
 *
 * Several shells can run at once, as they can in the Windows build; each is
 * identified by the small integer term_pty_open hands back.
 *
 * fork() in a process that has already used CoreFoundation is only safe if the
 * child goes straight to exec, which is what happens below: nothing between
 * the fork and execl touches Objective-C or any framework. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <util.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#define TERM_MAX 8

/* mac-cocoa/panel_cocoa.c */
void term_on_data(int id, const char *data, int n);
void term_on_exit(int id, int status);

typedef struct { int used, master; pid_t child; } Pty;

static Pty  s_ptys[TERM_MAX];
static char s_err[160];

const char *term_pty_error(void) { return s_err; }

int term_pty_alive(int id)
{
    return id >= 0 && id < TERM_MAX && s_ptys[id].used && s_ptys[id].master >= 0;
}

/* The shell's own name, for the panel to label its terminals with. */
const char *term_pty_shell_name(void)
{
    const char *shell = getenv("SHELL");
    const char *slash;
    if (!shell || !*shell) return "sh";
    slash = strrchr(shell, '/');
    return slash ? slash + 1 : shell;
}

void term_pty_close(int id)
{
    Pty *p;
    if (id < 0 || id >= TERM_MAX) return;
    p = &s_ptys[id];
    if (p->child > 0) {
        kill(p->child, SIGHUP);
        waitpid(p->child, NULL, WNOHANG);
        p->child = -1;
    }
    if (p->master >= 0) { close(p->master); p->master = -1; }
    p->used = 0;
}

void term_pty_close_all(void)
{
    int i;
    for (i = 0; i < TERM_MAX; i++) term_pty_close(i);
}

int term_pty_open(const char *cwd, int cols, int rows)
{
    struct winsize ws;
    pid_t pid;
    int master = -1, id;

    s_err[0] = 0;
    for (id = 0; id < TERM_MAX; id++) if (!s_ptys[id].used) break;
    if (id == TERM_MAX) {
        snprintf(s_err, sizeof s_err, "No more than %d terminals at once.", TERM_MAX);
        return -1;
    }

    memset(&ws, 0, sizeof ws);
    ws.ws_col = (unsigned short)(cols > 0 ? cols : 80);
    ws.ws_row = (unsigned short)(rows > 0 ? rows : 24);

    pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        snprintf(s_err, sizeof s_err, "Cannot open a pty: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        const char *shell = getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/sh";
        if (cwd && *cwd) {
            if (chdir(cwd) != 0)
                fprintf(stderr, "cannot enter %s: %s\r\n", cwd, strerror(errno));
        }
        /* The panel's ANSI handling is deliberately small, so ask for a
         * terminal that will not send anything clever. */
        setenv("TERM", "vt100", 1);
        execl(shell, shell, "-l", (char *)NULL);
        fprintf(stderr, "cannot run %s: %s\r\n", shell, strerror(errno));
        _exit(127);
    }

    s_ptys[id].used = 1;
    s_ptys[id].master = master;
    s_ptys[id].child = pid;
    /* Non-blocking: the poll below runs on the editor's event loop. */
    fcntl(master, F_SETFL, O_NONBLOCK);
    return id;
}

void term_pty_send(int id, const char *data, int n)
{
    if (!term_pty_alive(id) || n <= 0) return;
    while (n > 0) {
        ssize_t k = write(s_ptys[id].master, data, (size_t)n);
        if (k < 0) {
            if (errno == EINTR) continue;
            return;                      /* EAGAIN: the shell is not reading */
        }
        data += k;
        n -= (int)k;
    }
}

void term_pty_resize(int id, int cols, int rows)
{
    struct winsize ws;
    if (!term_pty_alive(id)) return;
    memset(&ws, 0, sizeof ws);
    ws.ws_col = (unsigned short)(cols > 0 ? cols : 80);
    ws.ws_row = (unsigned short)(rows > 0 ? rows : 24);
    ioctl(s_ptys[id].master, TIOCSWINSZ, &ws);
}

void term_pty_kill(int id)
{
    if (term_pty_alive(id) && s_ptys[id].child > 0) kill(s_ptys[id].child, SIGINT);
}

/* Drain whatever has arrived on every shell. Called once per pass of the event
 * loop, so it must never block -- and must not read one of them forever
 * either: a command that floods its output would keep the editor out of its
 * own event loop. */
#define POLL_READS 8

static void poll_one(int id)
{
    char buf[4096];
    int reads;
    Pty *p = &s_ptys[id];

    for (reads = 0; reads < POLL_READS; reads++) {
        ssize_t k = read(p->master, buf, sizeof buf);
        if (k > 0) {
            term_on_data(id, buf, (int)k);
            if (k < (ssize_t)sizeof buf) return;    /* drained */
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EINTR)) return;

        /* end of file, or the pty has gone: the shell has exited */
        {
            int status = 0;
            if (p->child > 0) waitpid(p->child, &status, WNOHANG);
            close(p->master);
            p->master = -1;
            p->child = -1;
            p->used = 0;
            term_on_exit(id, WIFEXITED(status) ? WEXITSTATUS(status) : 0);
        }
        return;
    }
}

void term_pty_poll(void)
{
    int i;
    for (i = 0; i < TERM_MAX; i++)
        if (s_ptys[i].used && s_ptys[i].master >= 0) poll_one(i);
}
