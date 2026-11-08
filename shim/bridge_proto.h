// Wire protocol between the stand-in DLL (Wine side) and usb-bridge (Linux side).
// Request:  u32 op, u32 a, u32 b, u32 len, then len bytes of payload.
// Response: u32 status, u32 len, then len bytes of payload.
#pragma once

#define BRIDGE_PORT 47931
#define BRIDGE_MAX_PAYLOAD (16u * 1024 * 1024)

#define E_OK 0
#define E_IO 1
#define E_NO_DEVICE 4
#define E_BAD_ARG 5
#define E_BLOCKED 6
#define E_TIMEOUT 7
#define E_FAILED 0xff

enum {
    OP_HELLO = 1,      // payload: token
    OP_IDENTIFY,       // reply: serial NUL name NUL
    OP_CMD_EXCHANGE,   // a=timeout ms, b=reply capacity, payload: command; reply: response
    OP_CMD_SEND,       // a=timeout ms, payload: command
    OP_CMD_RECV,       // a=timeout ms, b=capacity; reply: response
    OP_BULK_EXCHANGE,  // a=timeout ms, b=reply capacity, payload: data; reply: response
    OP_BULK_SEND,      // a=timeout ms, payload: data
    OP_BULK_RECV,      // a=timeout ms, b=capacity; reply: data
    OP_RESET_PIPE,     // a=endpoint address
    OP_STREAM_SET,     // a=STREAM_*, b=value
    OP_STREAM_SWITCH,  // a=0 off, 1 on
    OP_STREAM_READ,    // b=capacity; reply: one cached unit
    OP_STREAM_DISCARD,
    OP_STREAM_UNIT_SIZE,  // reply: u32
};

enum { STREAM_ATTEMPT_SIZE, STREAM_PKG_COUNT, STREAM_CACHE_UNITS, STREAM_WAIT_US };
