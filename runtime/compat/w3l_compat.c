/*
 * W3L / PvPGN / rubattle compatibility helpers.
 *
 * Independent reimplementation of the effects observed in wl27.dll 1.27 and
 * the public W3L/PvPGN behaviour.  No wl27 code is copied.  This file is used
 * only when WC3_W3L_COMPAT is enabled; the exact baseline does not contain
 * these symbols.
 */
#include "w3l_compat.h"

/* wl27 0x10001B51 reads the packet object through the Game.dll base at
 * 0x6F000000 + 0xD77CB8.  The lifted corpus uses the same fixed image base. */
#define WC3_W3L_PACKET_OBJECT_GVA 0x6FD77CB8u

/* wl27 uses an address inside wl27.dll as the return address of its send
 * wrapper.  There is no equivalent guest address, so use a valid Game.dll
 * code address; the stdcall send path ignores this slot after ret 8. */
#define WC3_W3L_DUMMY_RETURN_GVA 0x6F8DCB6Fu

static uint32_t w3l_rotl32(uint32_t value, unsigned count)
{
    count &= 31u;
    if (count == 0u) {
        return value;
    }
    return (value << count) | (value >> (32u - count));
}

/* W3L/PvPGN expansion rotates the constant 1 by the XOR value, not
 * ROTL32(xor, 1).  The x86 rotate uses count mod 32. */
static uint32_t w3l_rotl_one(uint32_t count)
{
    unsigned shift = count & 31u;
    if (shift == 0u) {
        return 1u;
    }
    return (1u << shift) | (1u >> (32u - shift));
}

uint32_t wc3_w3l_pvpgn_hash(CPU *c, uint32_t dst, uint32_t src)
{
    uint8_t account[16];
    uint8_t block[64];
    uint32_t w[80];
    uint32_t a, b, cc, d, e;
    uint32_t i;
    uint32_t count = 0u;
    uint32_t state[5];

    (void)c;

    /* wl27 wrapper 0x100018E0 copies exactly 16 bytes from src+0x20 into a
     * zeroed 64-byte buffer, lowercases A..Z, then calls strlen on that
     * buffer.  Bytes after the first NUL never enter the message. */
    for (i = 0u; i < 16u; i++) {
        uint8_t ch = rd8(src + 0x20u + i);
        if (ch >= (uint8_t)'A' && ch <= (uint8_t)'Z') {
            ch = (uint8_t)(ch | 0x20u);
        }
        account[i] = ch;
    }
    while (count < 16u && account[count] != 0u) {
        count++;
    }

    for (i = 0u; i < 64u; i++) {
        block[i] = 0u;
    }
    for (i = 0u; i < count; i++) {
        block[i] = account[i];
    }

    /* 16 little-endian words from the zero-padded 64-byte block. */
    for (i = 0u; i < 16u; i++) {
        w[i] = (uint32_t)block[i * 4u]
             | ((uint32_t)block[i * 4u + 1u] << 8)
             | ((uint32_t)block[i * 4u + 2u] << 16)
             | ((uint32_t)block[i * 4u + 3u] << 24);
    }
    for (i = 0u; i < 64u; i++) {
        w[i + 16u] = w3l_rotl_one(w[i] ^ w[i + 8u] ^ w[i + 2u] ^ w[i + 13u]);
    }

    a = 0x67452301u;
    b = 0xEFCDAB89u;
    cc = 0x98BADCFEu;
    d = 0x10325476u;
    e = 0xC3D2E1F0u;

    for (i = 0u; i < 20u; i++) {
        uint32_t f = (b & cc) | ((~b) & d);
        uint32_t g = w[i] + w3l_rotl32(a, 5u) + e + f + 0x5A827999u;
        e = d;
        d = cc;
        cc = w3l_rotl32(b, 30u);
        b = a;
        a = g;
    }
    for (; i < 40u; i++) {
        uint32_t f = b ^ cc ^ d;
        uint32_t g = w[i] + w3l_rotl32(a, 5u) + e + f + 0x6ED9EBA1u;
        e = d;
        d = cc;
        cc = w3l_rotl32(b, 30u);
        b = a;
        a = g;
    }
    for (; i < 60u; i++) {
        uint32_t f = (b & cc) | (b & d) | (cc & d);
        uint32_t g = w[i] + w3l_rotl32(a, 5u) + e + f + 0x8F1BBCDCu;
        e = d;
        d = cc;
        cc = w3l_rotl32(b, 30u);
        b = a;
        a = g;
    }
    for (; i < 80u; i++) {
        uint32_t f = b ^ cc ^ d;
        uint32_t g = w[i] + w3l_rotl32(a, 5u) + e + f + 0xCA62C1D6u;
        e = d;
        d = cc;
        cc = w3l_rotl32(b, 30u);
        b = a;
        a = g;
    }

    state[0] = 0x67452301u + a;
    state[1] = 0xEFCDAB89u + b;
    state[2] = 0x98BADCFEu + cc;
    state[3] = 0x10325476u + d;
    state[4] = 0xC3D2E1F0u + e;

    /* wl27 writes the state to src+0xA8, then wrapper 0x100018B0 copies
     * exactly 20 bytes to dst.  It never writes dst+0x14. */
    for (i = 0u; i < 5u; i++) {
        wr32(src + 0xA8u + i * 4u, state[i]);
        wr32(dst + i * 4u, state[i]);
    }

    return 1u;
}

/* wl27 worker 0x10001B5C/0x10001B6C use `test reg,reg; jle`, i.e. zero or
 * the sign bit set.  Sign-extending the comparison matters for exactness. */
static int w3l_ptr_not_positive(uint32_t value)
{
    return value == 0u || (value & 0x80000000u) != 0u;
}

static void w3l_finish_0x83(
    CPU *c,
    uint32_t old_ebp,
    uint32_t saved_esi,
    uint32_t saved_ebx,
    uint32_t saved_edi,
    uint32_t result
)
{
    /* Worker 0x10001B30 restores ebx/esi/edi/ebp from its prologue stack;
     * handler 0x10001B10 then pops the esi/ebp saved by L_6F8DC630.  The net
     * result restores all four to their entry values. */
    c->ebx = saved_ebx;
    c->esi = saved_esi;
    c->edi = saved_edi;
    c->ebp = rd32(old_ebp);
    /* Handler 0x10001B10: pop esi; pop ebp; ret 0x10.  old_ebp is the frame
     * after L_6F8DC630's push ebp, so ret 0x10 removes the return address and
     * four stdcall arguments: old_ebp + 4 + 0x10. */
    c->esp = old_ebp + 0x18u;
    c->eax = result;
}

void wc3_w3l_handle_0x83(CPU *c)
{
    uint32_t old_ebp = c->ebp;
    uint32_t saved_esi = rd32(old_ebp - 4u);
    uint32_t saved_ebx = c->ebx;
    uint32_t saved_edi = c->edi;
    uint32_t conn_call = c->ecx;
    uint32_t conn;
    uint32_t packet;
    uint32_t data_len;
    uint32_t name_len;
    uint32_t total;
    uint32_t buf;
    uint32_t vtable;
    uint32_t target;
    uint32_t i;

    /* Worker 0x10001B3A tests the incoming send object before touching the
     * packet global and returns zero only on this path. */
    if (conn_call == 0u) {
        w3l_finish_0x83(c, old_ebp, saved_esi, saved_ebx, saved_edi, 0u);
        return;
    }
    packet = rd32(GVA(WC3_W3L_PACKET_OBJECT_GVA));
    if (w3l_ptr_not_positive(packet)) {
        w3l_finish_0x83(c, old_ebp, saved_esi, saved_ebx, saved_edi, 1u);
        return;
    }
    /* wl27 worker 0x10001B64 takes the reply identity from the packet object
     * and keeps the incoming ecx only for the send vtable. */
    conn = rd32(packet + 0x1FCu);
    if (w3l_ptr_not_positive(conn)) {
        w3l_finish_0x83(c, old_ebp, saved_esi, saved_ebx, saved_edi, 1u);
        return;
    }

    data_len = (uint32_t)rd8(packet + 0x100u)
             | ((uint32_t)rd8(packet + 0x101u) << 8);
    if (data_len < 7u || data_len > 0x80u) {
        w3l_finish_0x83(c, old_ebp, saved_esi, saved_ebx, saved_edi, 1u);
        return;
    }

    name_len = 0u;
    while (name_len < 0x104u && rd8(conn + 0x0Cu + name_len) != 0u) {
        name_len++;
    }
    if (name_len >= 0x104u) {
        w3l_finish_0x83(c, old_ebp, saved_esi, saved_ebx, saved_edi, 1u);
        return;
    }

    /* Reply layout, exactly as wl27 worker 0x10001B30 builds it:
     *   FF 83 <len:u16> 00 00 00 00
     *   <u32 [conn+8]> <32 bytes [conn+0x110]>
     *   <u16 data_len> <data [packet+0x102]> <ASCIIZ [conn+0xC]>.
     * The frame in the worker is 0x204 bytes, but only the reply length plus
     * the three send-call slots is needed on the guest stack. */
    total = 0x2Eu + data_len + name_len + 1u;
    c->esp -= total;
    buf = c->esp;

    wr8(buf + 0u, 0xFFu);
    wr8(buf + 1u, 0x83u);
    wr8(buf + 2u, (uint8_t)(data_len & 0xFFu));
    wr8(buf + 3u, (uint8_t)(data_len >> 8));
    wr8(buf + 4u, 0u);
    wr8(buf + 5u, 0u);
    wr8(buf + 6u, 0u);
    wr8(buf + 7u, 0u);
    wr32(buf + 8u, rd32(conn + 8u));
    for (i = 0u; i < 32u; i++) {
        wr8(buf + 0x0Cu + i, rd8(conn + 0x110u + i));
    }
    wr8(buf + 0x2Cu, (uint8_t)(data_len & 0xFFu));
    wr8(buf + 0x2Du, (uint8_t)(data_len >> 8));
    for (i = 0u; i < data_len; i++) {
        wr8(buf + 0x2Eu + i, rd8(packet + 0x102u + i));
    }
    for (i = 0u; i < name_len; i++) {
        wr8(buf + 0x2Eu + data_len + i, rd8(conn + 0x0Cu + i));
    }
    wr8(buf + total - 1u, 0u);

    /* wl27 wrapper 0x10001C40 loads eax=[ecx] and calls [eax+0x2C]; it does
     * not guard the target, so a null target faults in the original too.
     * Reproduce the target-entry register state built by worker 0x10001B30:
     * eax=vtable, ebx=name_len, ecx=send object, edx=buf,
     * esi=name+name_len, edi=buf+total-1, ebp=name.  The wrapper has already
     * overwritten eax (total) with [ecx]. */
    vtable = rd32(conn_call);
    target = rd32(vtable + 0x2Cu);
    c->eax = vtable;
    c->ebx = name_len;
    c->ecx = conn_call;
    c->edx = buf;
    c->esi = conn + 0x0Cu + name_len;
    c->edi = buf + total - 1u;
    c->ebp = conn + 0x0Cu;
    push32(c, total);
    push32(c, buf);
    push32(c, WC3_W3L_DUMMY_RETURN_GVA);
    dispatch(c, target);

    w3l_finish_0x83(c, old_ebp, saved_esi, saved_ebx, saved_edi, 1u);
}
