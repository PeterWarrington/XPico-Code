/* xpcode-termd - the terminal helper for XPico Code on Mac OS X.
 *
 * XPico Code is a Carbon application. The same CFM binary runs on Mac OS 9 and,
 * through LaunchCFMApp, on PowerPC Mac OS X -- but CarbonLib exposes no POSIX,
 * so the editor cannot fork a shell itself. This daemon does it instead: it
 * listens on the loopback interface, and the editor connects with OpenTransport
 * (which CarbonLib does provide) and drives a real pty through it.
 *
 * On Mac OS 9 there is nothing to connect to, so the editor's Terminal tab
 * simply reports that it is unavailable.
 *
 * Build (inside the target OS X install, any gcc will do):
 *     cc -O2 -o xpcode-termd xpcode-termd.c        # add -lutil on Linux
 *
 * Protocol: a byte stream of frames, each
 *     1 byte opcode, 2 bytes big-endian length, then that many payload bytes.
 * Big-endian because the Carbon client is PowerPC and writes its fields that
 * way; it is also the obvious choice on the wire.
 *
 *   client -> server       server -> client
 *     'd' data to the pty    'd' output from the pty
 *     'r' resize: cols,rows  'x' shell exited: 2-byte status
 *     'c' set cwd, respawn
 *     'k' kill the shell
 */
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#define DEFAULT_PORT 50505
#define BUFSZ 4096

static int   g_master = -1;      /* pty master */
static pid_t g_child = -1;
static int   g_verbose = 0;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (!g_verbose) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---------------------------------------------------------------- framing */

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (k == 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}

static int send_frame(int fd, char op, const void *data, size_t n)
{
    unsigned char hdr[3];
    if (n > 0xFFFF) n = 0xFFFF;
    hdr[0] = (unsigned char)op;
    hdr[1] = (unsigned char)(n >> 8);
    hdr[2] = (unsigned char)(n & 0xFF);
    if (write_all(fd, hdr, 3) < 0) return -1;
    if (n && write_all(fd, data, n) < 0) return -1;
    return 0;
}

/* ---------------------------------------------------------------- the shell */

static void kill_shell(void)
{
    if (g_child > 0) {
        kill(g_child, SIGHUP);
        waitpid(g_child, NULL, WNOHANG);
        g_child = -1;
    }
    if (g_master >= 0) { close(g_master); g_master = -1; }
}

static int spawn_shell(const char *cwd, int cols, int rows)
{
    struct winsize ws;
    pid_t pid;

    kill_shell();

    memset(&ws, 0, sizeof ws);
    ws.ws_col = (unsigned short)(cols > 0 ? cols : 80);
    ws.ws_row = (unsigned short)(rows > 0 ? rows : 24);

    pid = forkpty(&g_master, NULL, NULL, &ws);
    if (pid < 0) return -1;

    if (pid == 0) {
        const char *shell = getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/sh";
        if (cwd && *cwd) {
            if (chdir(cwd) != 0)
                fprintf(stderr, "xpcode-termd: cannot enter %s: %s\r\n",
                        cwd, strerror(errno));
        }
        setenv("TERM", "vt100", 1);
        execl(shell, shell, "-l", (char *)NULL);
        /* only reached if exec failed */
        fprintf(stderr, "xpcode-termd: cannot run %s: %s\r\n", shell, strerror(errno));
        _exit(127);
    }

    g_child = pid;
    fcntl(g_master, F_SETFL, O_NONBLOCK);
    logf_("spawned shell pid %d", (int)pid);
    return 0;
}

/* ---------------------------------------------------------------- session */

/* Relay between one client and the pty until either goes away. */
static void serve(int cfd)
{
    unsigned char inbuf[BUFSZ];
    size_t have = 0;

    if (spawn_shell(NULL, 80, 24) < 0) {
        const char *msg = "cannot start a shell\r\n";
        send_frame(cfd, 'd', msg, strlen(msg));
        return;
    }

    for (;;) {
        fd_set rd;
        int maxfd = cfd;
        int n;

        FD_ZERO(&rd);
        FD_SET(cfd, &rd);
        if (g_master >= 0) {
            FD_SET(g_master, &rd);
            if (g_master > maxfd) maxfd = g_master;
        }

        n = select(maxfd + 1, &rd, NULL, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* shell -> client */
        if (g_master >= 0 && FD_ISSET(g_master, &rd)) {
            char out[BUFSZ];
            ssize_t k = read(g_master, out, sizeof out);
            if (k > 0) {
                if (send_frame(cfd, 'd', out, (size_t)k) < 0) break;
            } else if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) {
                int status = 0;
                unsigned char st[2];
                if (g_child > 0) waitpid(g_child, &status, WNOHANG);
                st[0] = (unsigned char)((status >> 8) & 0xFF);
                st[1] = (unsigned char)(status & 0xFF);
                send_frame(cfd, 'x', st, 2);
                kill_shell();
            }
        }

        /* client -> shell */
        if (FD_ISSET(cfd, &rd)) {
            ssize_t k = read(cfd, inbuf + have, sizeof inbuf - have);
            if (k <= 0) break;                 /* client closed */
            have += (size_t)k;

            /* consume whole frames */
            for (;;) {
                size_t len;
                char op;
                if (have < 3) break;
                op = (char)inbuf[0];
                len = ((size_t)inbuf[1] << 8) | inbuf[2];
                if (have < 3 + len) break;

                switch (op) {
                case 'd':
                    if (g_master >= 0) write_all(g_master, inbuf + 3, len);
                    break;
                case 'r':
                    if (len >= 4 && g_master >= 0) {
                        struct winsize ws;
                        memset(&ws, 0, sizeof ws);
                        ws.ws_col = (unsigned short)((inbuf[3] << 8) | inbuf[4]);
                        ws.ws_row = (unsigned short)((inbuf[5] << 8) | inbuf[6]);
                        ioctl(g_master, TIOCSWINSZ, &ws);
                    }
                    break;
                case 'c': {
                    char cwd[1024];
                    size_t cn = len < sizeof cwd - 1 ? len : sizeof cwd - 1;
                    memcpy(cwd, inbuf + 3, cn);
                    cwd[cn] = 0;
                    spawn_shell(cwd, 80, 24);
                    break;
                }
                case 'k':
                    kill_shell();
                    break;
                default:
                    logf_("unknown opcode '%c'", op);
                    break;
                }

                memmove(inbuf, inbuf + 3 + len, have - (3 + len));
                have -= 3 + len;
            }
        }
    }

    kill_shell();
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;
    int sfd, opt = 1, i;
    struct sockaddr_in addr;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) g_verbose = 1;
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [-p port] [-v]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    /* loopback only: this hands out a shell, and must never be reachable
     * from off the machine. */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(sfd, 1) < 0) { perror("listen"); return 1; }

    fprintf(stderr, "xpcode-termd listening on 127.0.0.1:%d\n", port);

    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        logf_("client connected");
        serve(cfd);
        close(cfd);
        logf_("client gone");
    }

    close(sfd);
    return 0;
}
