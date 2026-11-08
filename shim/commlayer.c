// Replacement for EXStar's CommunicateLayer.dll under Wine/Proton.
//
// The Windows version talks to SH3DU3Driver.sys, a KMDF driver Wine cannot load.
// This one forwards to usb-bridge, a native Linux process that owns the scanner
// through libusb, over a loopback socket authenticated with commshim.token.
//
// Exports keep the MSVC-mangled names through commlayer.def. Types below mirror
// the MSVC x64 layouts the callers use.

#include <winsock2.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bridge_proto.h"

typedef unsigned long ul;
typedef unsigned char u8;
typedef unsigned int u32;

// std::string, 32 bytes: 16-byte small buffer or pointer, size, capacity.
typedef struct {
    union {
        char buf[16];
        char *ptr;
    } u;
    size_t size;
    size_t cap;
} MsvcString;

typedef struct {
    MsvcString *first, *last, *end;
} MsvcStringVector;

typedef struct {
    int count;
    char serial[60][60];
    char name[60][60];
} SHNDEVINFO;
_Static_assert(sizeof(SHNDEVINFO) == 0x1c24, "SHNDEVINFO layout");

// One connection per channel, so a long bulk or image wait can't stall commands.
enum { CONN_CMD, CONN_BULK, CONN_STREAM, CONN_COUNT };

static struct {
    CRITICAL_SECTION cs;
    SOCKET s;
} g_conn[CONN_COUNT];

static const u8 g_pipes[] = {0x01, 0x81, 0x02, 0x82, 0x83};
static CRITICAL_SECTION g_cmd_channel, g_data_channel;
static HMODULE g_self;
static int g_device;
static char g_serial[32], g_name[128];

// The callers allocate through ucrtbase; mingw's malloc is msvcrt's, a different heap.
static void *crt_fn(const char *name)
{
    HMODULE m = GetModuleHandleA("ucrtbase.dll");
    if (!m)
        m = LoadLibraryA("ucrtbase.dll");
    return m ? (void *)GetProcAddress(m, name) : NULL;
}

static void *crt_malloc(size_t n)
{
    static void *(*fn)(size_t);
    if (!fn)
        fn = (void *(*)(size_t))crt_fn("malloc");
    return fn(n);
}

static void crt_free(void *p)
{
    static void (*fn)(void *);
    if (!fn)
        fn = (void (*)(void *))crt_fn("free");
    fn(p);
}

static void sibling_path(char *path, const char *name)
{
    char *slash;

    GetModuleFileNameA(g_self, path, MAX_PATH);
    slash = strrchr(path, '\\');
    if (slash)
        strcpy(slash + 1, name);
}

static void log_line(const char *fmt, ...)
{
    static HANDLE h = INVALID_HANDLE_VALUE;
    char line[1024];
    SYSTEMTIME t;
    DWORD written;
    va_list ap;
    int n, m;

    if (h == INVALID_HANDLE_VALUE) {
        char path[MAX_PATH + 32];
        sibling_path(path, "log\\commshim.log");
        h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    }
    GetLocalTime(&t);
    n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [%5lu/%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                 GetCurrentProcessId(), GetCurrentThreadId());
    va_start(ap, fmt);
    m = vsnprintf(line + n, sizeof line - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0)
        m = 0;
    n += m;
    if (n > (int)sizeof line - 2)
        n = sizeof line - 2;
    line[n++] = '\r';
    line[n++] = '\n';
    WriteFile(h, line, n, &written, NULL);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    int i;

    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        for (i = 0; i < CONN_COUNT; i++) {
            InitializeCriticalSection(&g_conn[i].cs);
            g_conn[i].s = INVALID_SOCKET;
        }
        InitializeCriticalSection(&g_cmd_channel);
        InitializeCriticalSection(&g_data_channel);
    }
    return TRUE;
}

static int send_all(SOCKET s, const void *p, unsigned n)
{
    const char *c = p;

    while (n) {
        int r = send(s, c, n, 0);
        if (r <= 0)
            return -1;
        c += r;
        n -= r;
    }
    return 0;
}

static int recv_all(SOCKET s, void *p, unsigned n)
{
    char *c = p;

    while (n) {
        int r = recv(s, c, n, 0);
        if (r <= 0)
            return -1;
        c += r;
        n -= r;
    }
    return 0;
}

static void conn_close(int c)
{
    if (g_conn[c].s != INVALID_SOCKET)
        closesocket(g_conn[c].s);
    g_conn[c].s = INVALID_SOCKET;
}

static int read_token(char *token, DWORD size)
{
    char path[MAX_PATH + 32];
    DWORD got = 0;
    HANDLE h;

    sibling_path(path, "commshim.token");
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    ReadFile(h, token, size - 1, &got, NULL);
    CloseHandle(h);
    token[got] = 0;
    token[strcspn(token, "\r\n")] = 0;
    return got ? 0 : -1;
}

static int exchange(int c, u32 op, u32 a, u32 b, const void *in, u32 len, u32 *status, void *out, u32 cap, u32 *outlen)
{
    u32 hdr[4] = {op, a, b, len}, resp[2];

    if (send_all(g_conn[c].s, hdr, sizeof hdr) || (len && send_all(g_conn[c].s, in, len)))
        return -1;
    if (recv_all(g_conn[c].s, resp, sizeof resp))
        return -1;
    *status = resp[0];
    if (resp[1]) {
        u32 keep = resp[1] < cap ? resp[1] : cap;
        if (keep && recv_all(g_conn[c].s, out, keep))
            return -1;
        if (resp[1] > keep) {
            char sink[4096];
            u32 rest = resp[1] - keep;
            while (rest) {
                u32 n = rest < sizeof sink ? rest : sizeof sink;
                if (recv_all(g_conn[c].s, sink, n))
                    return -1;
                rest -= n;
            }
        }
        *outlen = keep;
    } else {
        *outlen = 0;
    }
    return 0;
}

static int conn_open(int c)
{
    static LONG started;
    struct sockaddr_in addr;
    char token[128];
    DWORD timeout = 15000;
    u32 status, n;
    BOOL one = TRUE;
    SOCKET s;

    if (!InterlockedExchange(&started, 1)) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    if (read_token(token, sizeof token))
        return -1;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return -1;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(BRIDGE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&addr, sizeof addr)) {
        closesocket(s);
        return -1;
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    g_conn[c].s = s;
    if (exchange(c, OP_HELLO, 0, 0, token, (u32)strlen(token), &status, NULL, 0, &n) || status != E_OK) {
        conn_close(c);
        return -1;
    }
    return 0;
}

static ul rpc(int c, u32 op, u32 a, u32 b, const void *in, u32 len, void *out, u32 *outlen)
{
    u32 status = E_IO, got = 0, cap = outlen ? *outlen : 0;

    EnterCriticalSection(&g_conn[c].cs);
    if (g_conn[c].s == INVALID_SOCKET && conn_open(c)) {
        LeaveCriticalSection(&g_conn[c].cs);
        return E_NO_DEVICE;
    }
    if (exchange(c, op, a, b, in, len, &status, out, cap, &got)) {
        log_line("bridge connection lost (op %u)", op);
        conn_close(c);
        status = E_IO;
    }
    LeaveCriticalSection(&g_conn[c].cs);
    if (outlen && status == E_OK)
        *outlen = got;
    return status;
}

static const char *str_cstr(const MsvcString *s)
{
    return s->cap >= 16 ? s->u.ptr : s->u.buf;
}

// std::string by value is callee-destroyed in the MSVC x64 ABI.
static void str_destroy(MsvcString *s)
{
    if (s->cap >= 16) {
        char *p = s->u.ptr;
        if (s->cap + 1 >= 0x1000)
            p = ((char **)p)[-1];
        crt_free(p);
    }
    s->size = 0;
    s->cap = 15;
    s->u.buf[0] = 0;
}

static void str_init(MsvcString *s, const char *text)
{
    size_t n = strlen(text);

    if (n < 16) {
        memcpy(s->u.buf, text, n + 1);
        s->cap = 15;
    } else {
        s->cap = n | 15;
        s->u.ptr = crt_malloc(s->cap + 1);
        memcpy(s->u.ptr, text, n + 1);
    }
    s->size = n;
}

static int valid(void *h)
{
    return h == &g_device;
}

static ul identify(void)
{
    char reply[256];
    u32 n = sizeof reply - 1;
    ul st = rpc(CONN_CMD, OP_IDENTIFY, 0, 0, NULL, 0, reply, &n);

    if (st != E_OK)
        return st;
    reply[n] = 0;
    lstrcpynA(g_serial, reply, sizeof g_serial);
    lstrcpynA(g_name, reply + strlen(reply) + 1, sizeof g_name);
    return E_OK;
}

ul cl_initializeSystem(const GUID *g)
{
    (void)g;
    log_line("initializeSystem");
    return E_OK;
}

ul cl_deleteSystem(void)
{
    int i;

    log_line("deleteSystem");
    for (i = 0; i < CONN_COUNT; i++) {
        EnterCriticalSection(&g_conn[i].cs);
        conn_close(i);
        LeaveCriticalSection(&g_conn[i].cs);
    }
    return E_OK;
}

ul cl_getOnlineDeviceSerial(MsvcStringVector *out)
{
    MsvcString *item;

    if (out->first != out->last)
        return E_FAILED;
    if (identify() != E_OK)
        return 2;
    item = crt_malloc(sizeof *item);
    str_init(item, g_serial);
    out->first = item;
    out->last = out->end = item + 1;
    return E_OK;
}

ul cl_getOnlineDeviceSerialEx(SHNDEVINFO *info)
{
    memset(info, 0, sizeof *info);
    if (identify() != E_OK)
        return 2;
    info->count = 1;
    lstrcpynA(info->serial[0], g_serial, sizeof info->serial[0]);
    lstrcpynA(info->name[0], g_name, sizeof info->name[0]);
    return E_OK;
}

ul cl_attachDevice(void **handle, MsvcString *serial)
{
    ul ret = E_OK;

    log_line("attachDevice");
    if (*handle || !serial->size)
        ret = E_BAD_ARG;
    else if (identify() != E_OK || strcmp(str_cstr(serial), g_serial))
        ret = E_NO_DEVICE;
    else
        *handle = &g_device;
    str_destroy(serial);
    return ret;
}

ul cl_detachDevice(void **handle)
{
    log_line("detachDevice");
    if (!valid(*handle))
        return E_BAD_ARG;
    *handle = NULL;
    return E_OK;
}

ul cl_getDevicePipeAddressList(void *h, u8 *list, unsigned *count)
{
    unsigned n = sizeof g_pipes;

    if (!valid(h))
        return E_BAD_ARG;
    if (list)
        memcpy(list, g_pipes, *count && *count < n ? *count : n);
    *count = n;
    return E_OK;
}

ul cl_getMaxCommandSize(void *h, unsigned *size)
{
    log_line("getMaxCommandSize");
    if (!valid(h))
        return E_BAD_ARG;
    *size = 512;
    return E_OK;
}

ul cl_getImageDataUnitSize(void *h, unsigned *size)
{
    u32 n = sizeof *size;

    if (!valid(h))
        return E_BAD_ARG;
    return rpc(CONN_CMD, OP_STREAM_UNIT_SIZE, 0, 0, NULL, 0, size, &n);
}

// The original serializes these per process and the callers rely on it across
// a send and its receive; the critical sections are recursive, as there.
static void channel(CRITICAL_SECTION *cs, u8 lock)
{
    if (lock)
        EnterCriticalSection(cs);
    else
        LeaveCriticalSection(cs);
}

ul cl_commandChannelLock(void *h, u8 lock)
{
    (void)h;
    channel(&g_cmd_channel, lock);
    return E_OK;
}

ul cl_dataStreamlock(void *h, u8 lock)
{
    (void)h;
    channel(&g_data_channel, lock);
    return E_OK;
}

ul cl_driverStreamSwitch(void *h, u8 on)
{
    if (!valid(h))
        return E_BAD_ARG;
    log_line("driverStreamSwitch %u", on);
    return rpc(CONN_CMD, OP_STREAM_SWITCH, on ? 1 : 0, 0, NULL, 0, NULL, NULL);
}

ul cl_discardDriverFrameCaches(void *h)
{
    if (!valid(h))
        return E_BAD_ARG;
    return rpc(CONN_CMD, OP_STREAM_DISCARD, 0, 0, NULL, 0, NULL, NULL);
}

static ul stream_set(void *h, u32 which, u32 value)
{
    if (!valid(h))
        return E_BAD_ARG;
    log_line("stream setting %u = %u", which, value);
    return rpc(CONN_CMD, OP_STREAM_SET, which, value, NULL, 0, NULL, NULL);
}

ul cl_setDriverAttemptPkgSize(void *h, unsigned v)
{
    return stream_set(h, STREAM_ATTEMPT_SIZE, v);
}

ul cl_setDriverPkgCountPerUnit(void *h, unsigned v)
{
    return stream_set(h, STREAM_PKG_COUNT, v);
}

ul cl_setDriverCacheUnitCount(void *h, unsigned v)
{
    return stream_set(h, STREAM_CACHE_UNITS, v);
}

ul cl_setMaxGrabWaitTime(void *h, unsigned v)
{
    return stream_set(h, STREAM_WAIT_US, v);
}

static ul reset_pipe(u8 ep)
{
    return rpc(CONN_CMD, OP_RESET_PIPE, ep, 0, NULL, 0, NULL, NULL);
}

ul cl_resetPipe(void *h, u8 ep)
{
    if (!valid(h))
        return E_BAD_ARG;
    return reset_pipe(ep);
}

ul cl_resetCommandPipe(void *h, int unused)
{
    ul st;

    (void)unused;
    if (!valid(h))
        return E_BAD_ARG;
    st = reset_pipe(0x01);
    return st ? st : reset_pipe(0x81);
}

ul cl_resetBulkPipe(void *h, int unused)
{
    ul st;

    (void)unused;
    if (!valid(h))
        return E_BAD_ARG;
    st = reset_pipe(0x02);
    return st ? st : reset_pipe(0x82);
}

// Re-enumerating the scanner under EXStar isn't supported by the bridge.
ul cl_resetDeviceInDriver(void *h)
{
    log_line("resetDeviceInDriver ignored");
    return valid(h) ? E_OK : E_BAD_ARG;
}

ul cl_sendCommand(void *h, u8 *data, unsigned size)
{
    if (!valid(h))
        return E_NO_DEVICE;
    rpc(CONN_CMD, OP_CMD_SEND, 1000, 0, data, size, NULL, NULL);
    return E_OK;
}

ul cl_receiveCommandReply(void *h, u8 *data, unsigned *size)
{
    u32 n = *size;
    ul st;

    if (!valid(h))
        return E_NO_DEVICE;
    st = rpc(CONN_CMD, OP_CMD_RECV, 1000, n, NULL, 0, data, &n);
    if (st != E_OK)
        return E_FAILED;
    *size = n;
    return E_OK;
}

// Sends *size bytes and reads up to capacity back into the same buffer.
static ul command_exchange(void *h, u8 *data, unsigned *size, unsigned capacity)
{
    u32 n = capacity;
    ul st;

    if (!valid(h))
        return E_NO_DEVICE;
    EnterCriticalSection(&g_cmd_channel);
    st = rpc(CONN_CMD, OP_CMD_EXCHANGE, 1000, capacity, data, *size, data, &n);
    LeaveCriticalSection(&g_cmd_channel);
    if (st != E_OK)
        return E_FAILED;
    *size = n;
    return E_OK;
}

ul cl_sendCommandAndWaitReply(void *h, u8 *data, unsigned *size)
{
    return command_exchange(h, data, size, *size);
}

ul cl_sendCommandAndWaitReplyEx(void *h, u8 *data, unsigned *size, unsigned capacity)
{
    return command_exchange(h, data, size, capacity);
}

ul cl_sendBulkData(void *h, u8 *data, unsigned size)
{
    if (!valid(h))
        return E_NO_DEVICE;
    return rpc(CONN_BULK, OP_BULK_SEND, 5000, 0, data, size, NULL, NULL);
}

ul cl_receiveBulkReply(void *h, u8 *data, unsigned *size)
{
    u32 n = *size;
    ul st;

    if (!valid(h))
        return E_NO_DEVICE;
    st = rpc(CONN_BULK, OP_BULK_RECV, 5000, n, NULL, 0, data, &n);
    if (st != E_OK)
        return E_FAILED;
    *size = n;
    return E_OK;
}

ul cl_sendDataAndWaitReply(void *h, u8 *data, unsigned *size, unsigned capacity)
{
    u32 n = capacity;
    ul st;

    if (!valid(h))
        return E_NO_DEVICE;
    EnterCriticalSection(&g_data_channel);
    st = rpc(CONN_BULK, OP_BULK_EXCHANGE, 5000, capacity, data, *size, data, &n);
    LeaveCriticalSection(&g_data_channel);
    return st != E_OK ? E_FAILED : E_OK;
}

ul cl_receiveImageStream(void *h, u8 *data, unsigned *size)
{
    u32 n = *size;
    ul st;

    if (!valid(h))
        return E_NO_DEVICE;
    st = rpc(CONN_STREAM, OP_STREAM_READ, 0, n, NULL, 0, data, &n);
    if (st == E_OK)
        *size = n;
    return st;
}
