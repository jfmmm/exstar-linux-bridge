// Native Linux side of the CommunicateLayer stand-in: owns the scanner through
// libusb and does the work of Windows' SH3DU3Driver.sys.
//
// Usage: usb-bridge <token-file>
//
// Endpoints: 0x01/0x81 interrupt commands, 0x02/0x82 bulk data, 0x83 image stream.
// Only allowlisted command opcodes are forwarded and bulk writes are refused,
// except flash page reads, because the bulk channel can reach the scanner's flash.

#include <arpa/inet.h>
#include <errno.h>
#include <libusb-1.0/libusb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "bridge_proto.h"

#define VID 0x3267
#define PID 0x0003
#define EP_CMD_OUT 0x01
#define EP_CMD_IN 0x81
#define EP_BULK_OUT 0x02
#define EP_BULK_IN 0x82
#define EP_IMAGE_IN 0x83
#define IN_PACKET_MIN 1024
#define PENDING_READS 8

typedef uint8_t u8;
typedef uint32_t u32;

// Opcodes (bytes 2 and 3) EXStar sent during a full connect, plus the live settings
// its log names: 1023 exposure, 1027 gain, 1062 indicator, 1068 and 1070 luminance.
static const uint16_t ALLOWED_OPS[] = {0x0000, 0x0001, 0x0004, 0x0005, 0x0007, 0x1000, 0x1001, 0x1016,
                                       0x1017, 0x1020, 0x1021, 0x1023, 0x1024, 0x1025, 0x1027, 0x102e,
                                       0x1041, 0x1049, 0x1051, 0x105d, 0x1062, 0x1068, 0x1070};

static char g_token[128];
static libusb_context *g_usb;
static libusb_device_handle *g_dev;
static pthread_rwlock_t g_dev_lock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t g_cmd_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_bulk_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_serial[32], g_name[128];

// Mirrors the driver's device context fields for the image cache.
static struct {
    pthread_mutex_t lock;
    pthread_mutex_t switch_lock;
    pthread_cond_t cond;
    unsigned attempt, count, units, wait_us;
    int on, inflight;
    u8 *cache, *busy;
    unsigned write_unit, write_pkg, read_unit;
    unsigned long dropped, bad_header;
    struct libusb_transfer *xfer[PENDING_READS];
    pthread_t thread;
} g_s = {.lock = PTHREAD_MUTEX_INITIALIZER, .switch_lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};

static void logf_(const char *fmt, ...)
{
    struct timespec ts;
    struct tm tm;
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    pthread_mutex_lock(&g_log_lock);
    printf("%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
    pthread_mutex_unlock(&g_log_lock);
}

static void hexstr(char *out, const u8 *data, unsigned n)
{
    unsigned i, shown = n < 24 ? n : 24;

    out[0] = 0;
    for (i = 0; i < shown; i++)
        sprintf(out + i * 3, "%02x ", data[i]);
}

static int op_allowed(const u8 *cmd, unsigned n)
{
    unsigned i, op;

    if (n < 4)
        return 0;
    op = cmd[2] << 8 | cmd[3];
    for (i = 0; i < sizeof ALLOWED_OPS / sizeof *ALLOWED_OPS; i++)
        if (ALLOWED_OPS[i] == op)
            return 1;
    return 0;
}

// Opcode 0x1057 reads one numbered flash page: a 2-byte page index and nothing else
// in the 5120-byte buffer. EXStar's ReadData and the read half of WriteData use it.
static int is_flash_page_read(const u8 *d, unsigned n)
{
    unsigned i;

    if (n != 5120 || d[2] != 0x10 || d[3] != 0x57 || d[4] || d[5] || d[6] || d[7] != 2)
        return 0;
    for (i = 10; i < n; i++)
        if (d[i])
            return 0;
    return 1;
}

static u32 map_err(int r)
{
    if (r == LIBUSB_ERROR_TIMEOUT)
        return E_TIMEOUT;
    if (r == LIBUSB_ERROR_NO_DEVICE)
        return E_NO_DEVICE;
    return E_IO;
}

static u32 intr_out(const u8 *data, unsigned n, unsigned timeout)
{
    int done = 0, r = libusb_interrupt_transfer(g_dev, EP_CMD_OUT, (u8 *)data, n, &done, timeout);
    return r ? map_err(r) : E_OK;
}

// The endpoint sends whole packets, so read into at least a full packet and trim.
static u32 intr_in(u8 *out, unsigned *n, unsigned timeout)
{
    unsigned want = *n < IN_PACKET_MIN ? IN_PACKET_MIN : *n;
    u8 *tmp = malloc(want);
    int done = 0, r;

    if (!tmp)
        return E_IO;
    r = libusb_interrupt_transfer(g_dev, EP_CMD_IN, tmp, want, &done, timeout);
    if (!r) {
        if ((unsigned)done > *n)
            done = *n;
        memcpy(out, tmp, done);
        *n = done;
    }
    free(tmp);
    return r ? map_err(r) : E_OK;
}

static u32 bulk_in(u8 *out, unsigned *n, unsigned timeout)
{
    int done = 0, r = libusb_bulk_transfer(g_dev, EP_BULK_IN, out, *n, &done, timeout);

    if (!r)
        *n = done;
    return r ? map_err(r) : E_OK;
}

// Same exchange the original layer uses to read the scanner's identity:
// replies are masked unless byte 1 is 2, and echo bytes 0, 2 and 3 of the command.
static int identity_exchange(u8 *buf, unsigned send, unsigned cap)
{
    u8 cmd[100];
    int attempt;

    memcpy(cmd, buf, send);
    for (attempt = 0; attempt < 5; attempt++) {
        unsigned got = cap, i;
        u32 st = intr_out(cmd, send, 1000);

        if (st == E_OK) {
            st = intr_in(buf, &got, 1000);
            if (st == E_OK && buf[1] != 2 && got) {
                u8 key = buf[1] >> 4;
                key = key << 4 | key;
                for (i = 0; i < got; i++)
                    buf[i] ^= key;
            }
            if (st == E_OK && buf[0] == cmd[0] && buf[2] == cmd[2] && buf[3] == cmd[3])
                return 1;
        }
        libusb_clear_halt(g_dev, EP_CMD_OUT);
        libusb_clear_halt(g_dev, EP_CMD_IN);
    }
    return 0;
}

static void stream_off_locked(void);

static void dev_close(void)
{
    if (!g_dev)
        return;
    stream_off_locked();
    libusb_release_interface(g_dev, 0);
    libusb_close(g_dev);
    g_dev = NULL;
    g_serial[0] = g_name[0] = 0;
}

// Caller holds g_dev_lock for writing.
static u32 dev_open(void)
{
    u8 buf[1024];
    unsigned i, n;
    int r;

    dev_close();
    g_dev = libusb_open_device_with_vid_pid(g_usb, VID, PID);
    if (!g_dev)
        return E_NO_DEVICE;
    r = libusb_claim_interface(g_dev, 0);
    if (r) {
        logf_("claim failed: %s", libusb_strerror(r));
        libusb_close(g_dev);
        g_dev = NULL;
        return E_IO;
    }
    for (i = 0; i < 12; i++) {
        n = sizeof buf;
        if (intr_in(buf, &n, 50) != E_OK)
            break;
    }
    for (i = 0; i < 12; i++) {
        n = sizeof buf;
        if (bulk_in(buf, &n, 50) != E_OK)
            break;
    }

    memset(buf, 0, sizeof buf);
    buf[0] = 0xff;
    buf[3] = 0x04;
    if (!identity_exchange(buf, 30, 100)) {
        logf_("identity: serial read failed");
        dev_close();
        return E_IO;
    }
    for (i = 0; i < 8; i++)
        sprintf(g_serial + i * 2, "%02X", buf[9 + i]);

    memset(buf, 0, sizeof buf);
    buf[0] = 0xfe;
    buf[3] = 0x01;
    if (!identity_exchange(buf, 100, 100)) {
        logf_("identity: name read failed");
        dev_close();
        return E_IO;
    }
    n = (u32)buf[5] << 24 | buf[6] << 16 | buf[7] << 8 | buf[8];
    if (n > sizeof g_name - 1 || n > 91)
        n = 0;
    memcpy(g_name, buf + 9, n);
    g_name[n] = 0;
    logf_("scanner opened, product name: %s", g_name);
    return E_OK;
}

static int dev_alive(void)
{
    int cfg;
    return g_dev && libusb_get_configuration(g_dev, &cfg) == 0;
}

// --- image stream -----------------------------------------------------------

static void store_packet(const u8 *data, unsigned len)
{
    unsigned stride = g_s.attempt + 4, offset;

    if (len <= 0x20)
        return;
    if (data[0] != 0x3d || data[1] != 0x20) {
        g_s.bad_header++;
        return;
    }
    if (g_s.busy[g_s.write_unit] == 2) {
        g_s.dropped++;
        return;
    }
    offset = (g_s.write_unit * g_s.count + g_s.write_pkg) * stride;
    g_s.cache[offset] = len >> 24;
    g_s.cache[offset + 1] = len >> 16;
    g_s.cache[offset + 2] = len >> 8;
    g_s.cache[offset + 3] = len;
    memcpy(g_s.cache + offset + 4, data, len);
    if (g_s.write_pkg == g_s.count - 1 || data[8] == 1) {
        g_s.busy[g_s.write_unit] = 2;
        g_s.write_unit = (g_s.write_unit + 1) % g_s.units;
        g_s.write_pkg = 0;
        pthread_cond_broadcast(&g_s.cond);
    } else {
        g_s.write_pkg++;
    }
}

static void LIBUSB_CALL stream_cb(struct libusb_transfer *t)
{
    pthread_mutex_lock(&g_s.lock);
    if (t->status == LIBUSB_TRANSFER_COMPLETED && g_s.on)
        store_packet(t->buffer, t->actual_length);
    else if (t->status != LIBUSB_TRANSFER_CANCELLED && t->status != LIBUSB_TRANSFER_COMPLETED)
        logf_("stream transfer status %d", t->status);
    if (!(g_s.on && t->status == LIBUSB_TRANSFER_COMPLETED && libusb_submit_transfer(t) == 0)) {
        g_s.inflight--;
        pthread_cond_broadcast(&g_s.cond);
    }
    pthread_mutex_unlock(&g_s.lock);
}

static void *stream_events(void *arg)
{
    struct timeval tv = {0, 100000};

    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_s.lock);
        int more = g_s.on || g_s.inflight > 0;
        pthread_mutex_unlock(&g_s.lock);
        if (!more)
            return NULL;
        libusb_handle_events_timeout(g_usb, &tv);
    }
}

static void stream_free(void)
{
    int i;

    for (i = 0; i < PENDING_READS; i++) {
        if (g_s.xfer[i]) {
            free(g_s.xfer[i]->buffer);
            libusb_free_transfer(g_s.xfer[i]);
            g_s.xfer[i] = NULL;
        }
    }
    free(g_s.cache);
    free(g_s.busy);
    g_s.cache = g_s.busy = NULL;
}

static void stream_off_locked(void)
{
    int i;

    pthread_mutex_lock(&g_s.switch_lock);
    pthread_mutex_lock(&g_s.lock);
    if (!g_s.on) {
        pthread_mutex_unlock(&g_s.lock);
        pthread_mutex_unlock(&g_s.switch_lock);
        return;
    }
    g_s.on = 0;
    for (i = 0; i < PENDING_READS; i++)
        if (g_s.xfer[i])
            libusb_cancel_transfer(g_s.xfer[i]);
    while (g_s.inflight > 0)
        pthread_cond_wait(&g_s.cond, &g_s.lock);
    pthread_cond_broadcast(&g_s.cond);
    pthread_mutex_unlock(&g_s.lock);
    pthread_join(g_s.thread, NULL);
    logf_("stream off, dropped=%lu bad_header=%lu", g_s.dropped, g_s.bad_header);
    stream_free();
    libusb_clear_halt(g_dev, EP_IMAGE_IN);
    pthread_mutex_unlock(&g_s.switch_lock);
}

static u32 stream_on(void)
{
    size_t total;
    int i, submitted = 0;

    stream_off_locked();
    pthread_mutex_lock(&g_s.switch_lock);
    if (!g_s.attempt || !g_s.count || !g_s.units) {
        pthread_mutex_unlock(&g_s.switch_lock);
        return E_BAD_ARG;
    }
    total = (size_t)(g_s.attempt + 4) * g_s.count * g_s.units;
    g_s.cache = malloc(total);
    g_s.busy = calloc(g_s.units, 1);
    if (!g_s.cache || !g_s.busy) {
        stream_free();
        pthread_mutex_unlock(&g_s.switch_lock);
        return E_IO;
    }
    g_s.write_unit = g_s.write_pkg = g_s.read_unit = 0;
    g_s.dropped = g_s.bad_header = 0;

    pthread_mutex_lock(&g_s.lock);
    g_s.on = 1;
    g_s.inflight = 0;
    for (i = 0; i < PENDING_READS; i++) {
        u8 *buf = malloc(g_s.attempt);
        struct libusb_transfer *t = libusb_alloc_transfer(0);
        if (!buf || !t) {
            free(buf);
            libusb_free_transfer(t);
            break;
        }
        libusb_fill_bulk_transfer(t, g_dev, EP_IMAGE_IN, buf, g_s.attempt, stream_cb, NULL, 0);
        g_s.xfer[i] = t;
        if (libusb_submit_transfer(t))
            break;
        g_s.inflight++;
        submitted++;
    }
    pthread_mutex_unlock(&g_s.lock);
    pthread_create(&g_s.thread, NULL, stream_events, NULL);
    logf_("stream on: pkg=%u per-unit=%u units=%u wait=%uus pending=%d", g_s.attempt, g_s.count, g_s.units, g_s.wait_us,
          submitted);
    pthread_mutex_unlock(&g_s.switch_lock);
    if (!submitted) {
        stream_off_locked();
        return E_IO;
    }
    return E_OK;
}

static u32 stream_read(u8 *out, unsigned *n)
{
    struct timespec deadline;
    unsigned unit, size;
    u32 st = E_OK;

    clock_gettime(CLOCK_REALTIME, &deadline);
    pthread_mutex_lock(&g_s.lock);
    deadline.tv_nsec += (long)g_s.wait_us * 1000;
    deadline.tv_sec += deadline.tv_nsec / 1000000000;
    deadline.tv_nsec %= 1000000000;
    size = (g_s.attempt + 4) * g_s.count;
    if (!g_s.on || *n < size) {
        pthread_mutex_unlock(&g_s.lock);
        return g_s.on ? E_BAD_ARG : E_IO;
    }
    unit = g_s.read_unit;
    while (g_s.on && g_s.busy[unit] != 2 && st == E_OK)
        if (pthread_cond_timedwait(&g_s.cond, &g_s.lock, &deadline) == ETIMEDOUT)
            st = E_TIMEOUT;
    if (g_s.on && g_s.busy[unit] == 2) {
        memcpy(out, g_s.cache + (size_t)unit * g_s.count * (g_s.attempt + 4), size);
        g_s.busy[unit] = 0;
        g_s.read_unit = (unit + 1) % g_s.units;
        *n = size;
        st = E_OK;
    } else if (st == E_OK) {
        st = E_IO;
    }
    pthread_mutex_unlock(&g_s.lock);
    return st;
}

static u32 stream_discard(void)
{
    pthread_mutex_lock(&g_s.lock);
    if (g_s.busy)
        memset(g_s.busy, 0, g_s.units);
    g_s.write_unit = g_s.write_pkg = g_s.read_unit = 0;
    pthread_mutex_unlock(&g_s.lock);
    return E_OK;
}

static u32 stream_set(u32 which, u32 v)
{
    u32 st = E_OK;

    pthread_mutex_lock(&g_s.lock);
    if (g_s.on && which != STREAM_WAIT_US)
        st = E_IO;
    else if (which == STREAM_ATTEMPT_SIZE && v && !(v & 0x3ff))
        g_s.attempt = v;
    else if (which == STREAM_PKG_COUNT && v >= 1 && v <= 10000)
        g_s.count = v;
    else if (which == STREAM_CACHE_UNITS && v >= 1 && v <= 10000)
        g_s.units = v;
    else if (which == STREAM_WAIT_US)
        g_s.wait_us = v;
    else
        st = E_BAD_ARG;
    pthread_mutex_unlock(&g_s.lock);
    return st;
}

// --- request handling -------------------------------------------------------

static int read_all(int fd, void *p, size_t n)
{
    char *c = p;

    while (n) {
        ssize_t r = recv(fd, c, n, 0);
        if (r <= 0)
            return -1;
        c += r;
        n -= r;
    }
    return 0;
}

static int write_all(int fd, const void *p, size_t n)
{
    const char *c = p;

    while (n) {
        ssize_t r = send(fd, c, n, MSG_NOSIGNAL);
        if (r <= 0)
            return -1;
        c += r;
        n -= r;
    }
    return 0;
}

static u32 cmd_exchange(const u8 *in, unsigned len, u8 *out, unsigned *cap, unsigned timeout)
{
    char hex[80], rhex[80];
    u32 st;

    hexstr(hex, in, len);
    if (!op_allowed(in, len)) {
        logf_("BLOCKED cmd len=%u %s", len, hex);
        return E_BLOCKED;
    }
    pthread_mutex_lock(&g_cmd_lock);
    st = intr_out(in, len, timeout);
    if (st == E_OK)
        st = intr_in(out, cap, timeout);
    pthread_mutex_unlock(&g_cmd_lock);
    hexstr(rhex, out, st == E_OK ? *cap : 0);
    logf_("cmd len=%u %s-> st=%u reply=%u %s", len, hex, st, st == E_OK ? *cap : 0, rhex);
    return st;
}

static void *client_main(void *arg)
{
    int fd = (intptr_t)arg, authed = 0;
    u32 hdr[4], resp[2];
    u8 *in = NULL, *out = NULL;

    for (;;) {
        u32 op, a, b, len, st = E_BAD_ARG, cap, outlen = 0;
        unsigned n;

        if (read_all(fd, hdr, sizeof hdr))
            break;
        op = hdr[0], a = hdr[1], b = hdr[2], len = hdr[3];
        if (len > BRIDGE_MAX_PAYLOAD || b > BRIDGE_MAX_PAYLOAD)
            break;
        cap = b < 256 ? 256 : b;
        free(in);
        free(out);
        in = malloc(len + 1);
        out = malloc(cap);
        if (!in || !out || read_all(fd, in, len))
            break;
        in[len] = 0;

        if (!authed) {
            if (op != OP_HELLO || strcmp((char *)in, g_token))
                break;
            authed = 1;
            st = E_OK;
            goto reply;
        }

        if (op == OP_IDENTIFY) {
            pthread_rwlock_rdlock(&g_dev_lock);
            int ok = dev_alive() && g_serial[0];
            pthread_rwlock_unlock(&g_dev_lock);
            st = E_OK;
            if (!ok) {
                pthread_rwlock_wrlock(&g_dev_lock);
                st = dev_open();
                pthread_rwlock_unlock(&g_dev_lock);
            }
            if (st == E_OK) {
                size_t s = strlen(g_serial) + 1, m = strlen(g_name) + 1;
                memcpy(out, g_serial, s);
                memcpy(out + s, g_name, m);
                outlen = s + m;
            }
            goto reply;
        }

        pthread_rwlock_rdlock(&g_dev_lock);
        if (!g_dev && op != OP_STREAM_SET) {
            st = E_NO_DEVICE;
            pthread_rwlock_unlock(&g_dev_lock);
            goto reply;
        }
        n = b;
        switch (op) {
        case OP_CMD_EXCHANGE:
            st = cmd_exchange(in, len, out, &n, a);
            if (st == E_OK)
                outlen = n;
            break;
        case OP_CMD_SEND:
            if (!op_allowed(in, len)) {
                char hex[80];
                hexstr(hex, in, len);
                logf_("BLOCKED cmd send len=%u %s", len, hex);
                st = E_BLOCKED;
            } else {
                pthread_mutex_lock(&g_cmd_lock);
                st = intr_out(in, len, a);
                pthread_mutex_unlock(&g_cmd_lock);
            }
            break;
        case OP_CMD_RECV:
            pthread_mutex_lock(&g_cmd_lock);
            st = intr_in(out, &n, a);
            pthread_mutex_unlock(&g_cmd_lock);
            if (st == E_OK)
                outlen = n;
            break;
        case OP_BULK_EXCHANGE:
        case OP_BULK_SEND: {
            char hex[80];
            hexstr(hex, in, len);
            if (op == OP_BULK_EXCHANGE && is_flash_page_read(in, len)) {
                int done = 0, r;
                pthread_mutex_lock(&g_bulk_lock);
                r = libusb_bulk_transfer(g_dev, EP_BULK_OUT, in, len, &done, a);
                st = r ? map_err(r) : bulk_in(out, &n, a);
                pthread_mutex_unlock(&g_bulk_lock);
                logf_("flash read %s-> st=%u reply=%u", hex, st, st == E_OK ? n : 0);
                if (st == E_OK)
                    outlen = n;
                break;
            }
            logf_("BLOCKED bulk write len=%u %s", len, hex);
            st = E_BLOCKED;
            break;
        }
        case OP_BULK_RECV:
            pthread_mutex_lock(&g_bulk_lock);
            st = bulk_in(out, &n, a);
            pthread_mutex_unlock(&g_bulk_lock);
            if (st == E_OK)
                outlen = n;
            break;
        case OP_RESET_PIPE:
            st = libusb_clear_halt(g_dev, (u8)a) ? E_IO : E_OK;
            break;
        case OP_STREAM_SET:
            st = stream_set(a, b);
            break;
        case OP_STREAM_SWITCH:
            if (a) {
                st = stream_on();
            } else {
                stream_off_locked();
                st = E_OK;
            }
            break;
        case OP_STREAM_READ:
            st = stream_read(out, &n);
            if (st == E_OK)
                outlen = n;
            break;
        case OP_STREAM_DISCARD:
            st = stream_discard();
            break;
        case OP_STREAM_UNIT_SIZE: {
            u32 unit = (g_s.attempt + 4) * g_s.count;
            memcpy(out, &unit, 4);
            outlen = 4;
            st = E_OK;
            break;
        }
        }
        pthread_rwlock_unlock(&g_dev_lock);

    reply:
        resp[0] = st, resp[1] = outlen;
        if (write_all(fd, resp, sizeof resp) || (outlen && write_all(fd, out, outlen)))
            break;
    }
    free(in);
    free(out);
    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(BRIDGE_PORT)};
    int srv, one = 1, r;
    FILE *f;

    if (argc != 2 || !(f = fopen(argv[1], "r")) || !fgets(g_token, sizeof g_token, f)) {
        fprintf(stderr, "usage: usb-bridge <token-file>\n");
        return 2;
    }
    fclose(f);
    g_token[strcspn(g_token, "\r\n")] = 0;

    r = libusb_init(&g_usb);
    if (r) {
        fprintf(stderr, "libusb_init: %s\n", libusb_strerror(r));
        return 1;
    }

    srv = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) || listen(srv, 16)) {
        perror("bind");
        return 1;
    }
    logf_("usb-bridge listening on 127.0.0.1:%d", BRIDGE_PORT);

    for (;;) {
        pthread_t th;
        int fd = accept(srv, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (pthread_create(&th, NULL, client_main, (void *)(intptr_t)fd)) {
            close(fd);
            continue;
        }
        pthread_detach(th);
    }
    return 0;
}
