/* Terminal transport for the Carbon build: an OpenTransport TCP client that
 * talks to tools/termd/xpcode-termd over the loopback interface.
 *
 * CarbonLib gives the editor no way to start a process, but it does give it
 * OpenTransport. On Mac OS X the helper runs the pty and this connects to it;
 * on Mac OS 9 nothing is listening, the connect fails, and the panel says the
 * terminal is unavailable. One binary, both systems.
 *
 * Everything here is synchronous-but-non-blocking: the endpoint is polled from
 * the shim's event loop, so the editor never stalls waiting on the helper. */
#include <Carbon.h>
#include "win32.h"
#include <string.h>
#include <stdio.h>

#define TERM_PORT 50505

static OTClientContextPtr s_ctx;
static EndpointRef s_ep;
static int s_state;            /* 0 idle, 1 connected, 2 failed */
static char s_err[128];

/* mac-carbon/panel_mac.c */
void term_on_data(const char *data, int n);
void term_on_exit(int status);

static int send_frame(char op, const void *data, int n)
{
    unsigned char hdr[3];
    OTResult r;
    if (s_state != 1 || !s_ep) return 0;
    if (n < 0) n = 0;
    hdr[0] = (unsigned char)op;
    hdr[1] = (unsigned char)((n >> 8) & 0xFF);
    hdr[2] = (unsigned char)(n & 0xFF);
    r = OTSnd(s_ep, hdr, 3, 0);
    if (r < 0) return 0;
    if (n) {
        r = OTSnd(s_ep, (void *)data, (OTByteCount)n, 0);
        if (r < 0) return 0;
    }
    return 1;
}

const char *term_transport_error(void) { return s_err; }
int term_transport_state(void) { return s_state; }

/* Connect to the helper. Returns 0 and fills the error string on failure. */
int term_transport_open(void)
{
    OSStatus err;
    InetAddress addr;
    TCall call;

    if (s_state == 1) return 1;
    s_err[0] = 0;

    err = InitOpenTransportInContext(kInitOTForApplicationMask, &s_ctx);
    if (err != noErr) {
        snprintf(s_err, sizeof s_err, "OpenTransport is not available (%d).", (int)err);
        s_state = 2;
        return 0;
    }

    s_ep = OTOpenEndpointInContext(OTCreateConfiguration(kTCPName), 0, NULL, &err, s_ctx);
    if (err != noErr || !s_ep) {
        snprintf(s_err, sizeof s_err, "Cannot open a TCP endpoint (%d).", (int)err);
        s_state = 2;
        return 0;
    }

    /* Connect synchronously; reads are switched to non-blocking afterwards so
     * polling never stalls the editor. */
    OTSetSynchronous(s_ep);
    OTSetBlocking(s_ep);

    err = OTBind(s_ep, NULL, NULL);
    if (err != noErr) {
        snprintf(s_err, sizeof s_err, "Cannot bind a TCP endpoint (%d).", (int)err);
        OTCloseProvider(s_ep); s_ep = NULL;
        s_state = 2;
        return 0;
    }

    OTInitInetAddress(&addr, TERM_PORT, 0x7F000001);   /* 127.0.0.1 */
    memset(&call, 0, sizeof call);
    call.addr.buf = (UInt8 *)&addr;
    call.addr.len = sizeof addr;

    err = OTConnect(s_ep, &call, NULL);
    if (err != noErr) {
        snprintf(s_err, sizeof s_err,
                 "No terminal helper on port %d. Start xpcode-termd.", TERM_PORT);
        OTUnbind(s_ep);
        OTCloseProvider(s_ep); s_ep = NULL;
        s_state = 2;
        return 0;
    }

    OTSetNonBlocking(s_ep);
    s_state = 1;
    return 1;
}

void term_transport_close(void)
{
    if (s_ep) {
        OTSndOrderlyDisconnect(s_ep);
        OTUnbind(s_ep);
        OTCloseProvider(s_ep);
        s_ep = NULL;
    }
    s_state = 0;
}

void term_transport_send(const char *data, int n) { send_frame('d', data, n); }
void term_transport_resize(int cols, int rows)
{
    unsigned char p[4];
    p[0] = (unsigned char)((cols >> 8) & 0xFF); p[1] = (unsigned char)(cols & 0xFF);
    p[2] = (unsigned char)((rows >> 8) & 0xFF); p[3] = (unsigned char)(rows & 0xFF);
    send_frame('r', p, 4);
}
void term_transport_cwd(const char *path) { send_frame('c', path, (int)strlen(path)); }
void term_transport_kill(void) { send_frame('k', NULL, 0); }

/* Drain whatever has arrived and dispatch complete frames. Called from the
 * shim's idle hook, so it must never block. */
void term_transport_poll(void)
{
    static unsigned char buf[8192];
    static int have;
    OTFlags flags;
    OTResult r;

    if (s_state != 1 || !s_ep) return;

    for (;;) {
        if (have >= (int)sizeof buf) break;
        r = OTRcv(s_ep, buf + have, sizeof buf - have, &flags);
        if (r > 0) {
            have += r;
            continue;
        }
        if (r == kOTNoDataErr) break;          /* nothing pending */
        if (r == kOTLookErr) {                 /* disconnect or other event */
            OTResult look = OTLook(s_ep);
            if (look == T_DISCONNECT) { OTRcvDisconnect(s_ep, NULL); term_on_exit(0); term_transport_close(); }
            else if (look == T_ORDREL) { OTRcvOrderlyDisconnect(s_ep); term_on_exit(0); term_transport_close(); }
            break;
        }
        break;
    }

    /* consume whole frames */
    for (;;) {
        int len;
        char op;
        if (have < 3) break;
        op = (char)buf[0];
        len = (buf[1] << 8) | buf[2];
        if (have < 3 + len) break;

        if (op == 'd') term_on_data((const char *)buf + 3, len);
        else if (op == 'x') term_on_exit(len >= 2 ? ((buf[3] << 8) | buf[4]) : 0);

        memmove(buf, buf + 3 + len, (size_t)(have - (3 + len)));
        have -= 3 + len;
    }
}
