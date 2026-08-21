/*
 * st2110.c - VLC access_demux plugin for SMPTE ST 2110-20 (RFC 4175) reception
 *
 * Receives 10bit YCbCr 4:2:2 GPM video carried per RFC 4175 over RTP/UDP and
 * outputs VLC_CODEC_I422_10L. See docs/vlc-st2110_receive-module_仕様書.md.
 *
 * Architecture (rewritten from the earlier batched-worker-pool design after
 * measurement showed the true wire packet rate for a real 1080i59.94
 * 4:2:2 10bit stream (~486k pkts/sec, confirmed via an independent pktmon
 * capture) vastly exceeds what a single receive thread can drain, even
 * after extensive optimization):
 *
 *   - RFC4175 packets are fully self-describing: each one's Sample Row Data
 *     header(s) give the exact (line, field, x-offset, length) its own
 *     payload covers (verified against the RFC4175 text itself -- the
 *     continuation bit only chains multiple headers WITHIN one packet, and
 *     Offset/Length are always relative to that packet's own data). No
 *     packet needs any information from any other packet to know where its
 *     pixels belong, and no "frame" or "field" bookkeeping is needed for
 *     correctness of placement.
 *   - So: N identical I/O threads all pull completions from one shared
 *     socket/IOCP, and each does the FULL pipeline itself (parse -> unpack
 *     -> write) directly into one persistent, never-cleared "master"
 *     picture buffer. Writes never need locking: two different packets'
 *     target byte ranges never overlap (different lines, or non-
 *     overlapping segments of the same line).
 *   - A single, independent sender thread ticks on its own clock (not
 *     driven by arrival timing at all) and periodically snapshots whatever
 *     is currently in the master buffer to es_out. Packet loss just means
 *     some pixels are one tick stale, not a discarded/blanked frame.
 *
 * Targets the VLC 3.0.x plugin ABI (mtime_t/date_t, not the VLC4 vlc_tick_t
 * API).
 */

#ifdef _WIN32
# ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0601
# endif
# ifndef WINVER
#  define WINVER 0x0601
# endif
# include <winsock2.h>
# include <ws2tcpip.h>
# define poll(fds, nfds, timeout) WSAPoll((fds), (nfds), (timeout))
#endif

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_demux.h>
#include <vlc_network.h>
#include <vlc_threads.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#ifndef _WIN32
# include <poll.h>
# include <fcntl.h>
# include <unistd.h>
#endif

/* ---- RFC 4175 / RTP constants ---- */
#define RTP_HDR_MIN_LEN        12
#define RFC4175_EXT_SEQ_LEN     2
#define RFC4175_LINE_HDR_LEN    6

#define MAX_LINE_SEGMENTS      64
#define RECV_BUF_LEN         4096

#define DEFAULT_WIDTH         1920
#define DEFAULT_HEIGHT        1080
#define DEFAULT_FPS_NUM       30000
#define DEFAULT_FPS_DEN        1001

#define SO_RCVBUF_SIZE (32 * 1024 * 1024)

/* Per-I/O-thread share of pre-posted overlapped receive buffers (Windows).
 * More threads draining the same IOCP means more buffers should be kept in
 * flight at once so none of them ever stalls waiting for a free one. */
#define IOCP_BUFS_PER_THREAD    16
#define IOCP_MAX_COMPLETIONS    64

/*
 * SMPTE ST 2110-20:2017 6.1.5 defines the SRD Line No as zero-based. In
 * practice, some real SDI-to-IP gateways instead emit the SDI-legacy raw
 * raster numbering from SMPTE 274M/296M (blanking included). The two
 * conventions are numerically disjoint for field two of a 1080-line raster
 * (zero-based: 0-539, SDI-legacy: 584-1123), so we auto-detect per sender
 * instead of hardcoding either one.
 */
#define SMPTE274M_PROGRESSIVE_BASE   42
#define SMPTE274M_FIELD1_BASE        21
#define SMPTE274M_FIELD2_BASE       584
#define LINE_MODE_SDI_LEGACY_MIN    570
#define LINE_MODE_DETECT_MIN_PKTS     8

typedef enum {
    LINE_MODE_UNKNOWN = 0,
    LINE_MODE_ZERO_BASED,
    LINE_MODE_SDI_LEGACY,
} line_mode_t;

typedef struct {
    uint16_t length;      /* segment length in bytes (raw, pgroup-packed)  */
    bool     field2;      /* RFC4175 F bit                                 */
    uint16_t line_no;     /* raw 15-bit wire value                         */
    uint16_t offset;      /* raw 15-bit sample offset within the line      */
} rfc4175_line_hdr_t;

#ifdef _WIN32
typedef struct {
    OVERLAPPED overlapped;
    WSABUF     wsabuf;
    DWORD      flags;
    uint8_t    data[RECV_BUF_LEN];
} iocp_buf_t;
#endif

struct demux_sys_t
{
    /* network */
    int      fd;
    char     psz_group[256];
    int      i_port;
    char    *psz_source;

    /* format */
    unsigned i_width;
    unsigned i_height;
    unsigned i_depth;
    bool     b_interlace;
    unsigned i_fps_num;
    unsigned i_fps_den;

    /* line-number convention, detected from the first few packets by
       whichever I/O thread happens to see them first. Racy but harmless:
       redundant writes of the same correct value from multiple threads
       are fine, and DetectLineMode() only needs the SDP-declared cases to
       be told apart, not exact packet ordering. */
    atomic_uint line_mode;              /* line_mode_t */
    atomic_uint i_pkts_for_line_detect;  /* cumulative, never reset */

    /* elementary stream (touched only by the sender thread) */
    es_out_id_t *p_es;
    date_t       pts;

    /* Persistent picture buffer: allocated and zeroed once, then NEVER
       cleared again. Every I/O thread writes incoming pixels directly
       into it at the position its own packet describes; the sender
       thread periodically copies whatever is currently in it out to
       es_out. No lock is needed for the writes: RFC4175 guarantees two
       different packets never target overlapping byte ranges (see the
       file header comment), so concurrent writers never race with each
       other. The sender's snapshot copy can in principle race with an
       in-progress write (a rare, cosmetically-torn line at worst) --
       an accepted trade-off for a monitoring tool that would rather show
       slightly-torn video than add synchronization back into the hot
       path. */
    uint8_t *p_master_buf;
    size_t   i_buf_size;
    size_t   i_y_plane_size;
    size_t   i_uv_plane_size;

    /* I/O threads: N identical threads, each running the full
       receive -> parse -> unpack pipeline itself (see the file header
       comment for why no hand-off between stages is needed anymore). */
    unsigned       n_threads;
    vlc_thread_t  *threads;
    unsigned       n_threads_started;

    /* sender thread: independent clock, decoupled from arrival timing */
    vlc_thread_t sender_thread;
    bool         b_sender_started;

    /* Cooperative shutdown signal: see the ReceiveThread-era comment this
       carries forward from -- a blocked GetQueuedCompletionStatusEx() or
       poll() is not guaranteed to be interrupted by vlc_cancel() on every
       platform, so every wait loop here also checks this flag on its own
       bounded timeout, guaranteeing Close()'s vlc_join() calls can never
       hang. */
    atomic_bool  b_stop_requested;

#ifdef _WIN32
    HANDLE      iocp;
    iocp_buf_t *iocp_bufs;
    unsigned    n_iocp_bufs;
#endif

    /* rolling diagnostics: written by any I/O thread (atomic), read and
       reset by the sender thread every ~5s */
    atomic_uint i_pkts_total;
    atomic_uint i_pkts_rtp_fail;
    atomic_uint i_pkts_line_fail;
    atomic_uint i_hdrs_total;
    atomic_uint i_hdrs_field2;
};

static int  Open(vlc_object_t *);
static void Close(vlc_object_t *);
static int  Control(demux_t *, int, va_list);
static void *IoThread(void *);
static void *SenderThread(void *);

vlc_module_begin()
    set_shortname("ST2110")
    set_description("SMPTE ST 2110-20 receiver (RFC 4175, 10bit GPM)")
    set_capability("access_demux", 0)
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACCESS)
    add_shortcut("st2110")
    set_callbacks(Open, Close)

    add_string("st2110-source", NULL, "Source filter address (SSM)", NULL, true)
    add_integer("st2110-width", DEFAULT_WIDTH, "Width", NULL, true)
    add_integer("st2110-height", DEFAULT_HEIGHT, "Height", NULL, true)
    add_integer("st2110-depth", 10, "Sample depth (bits)", NULL, true)
    add_string("st2110-sampling", "YCbCr-4:2:2", "Chroma sampling", NULL, true)
    add_string("st2110-fps", "30000/1001", "Frame rate (num/den)", NULL, true)
    add_string("st2110-colorimetry", "BT709", "Colorimetry (BT709/BT2020/ST2084/HLG)", NULL, true)
    add_bool("st2110-interlace", false, "Interlaced", NULL, true)
    add_integer("st2110-threads", 0, "Receive threads (0 = auto-detect from CPU count)", NULL, true)
vlc_module_end()

/* ---- helpers ---- */

static unsigned DetectCpuCount(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? (unsigned)si.dwNumberOfProcessors : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1;
#endif
}

static void ParseFraction(const char *s, unsigned *num, unsigned *den)
{
    *num = DEFAULT_FPS_NUM;
    *den = DEFAULT_FPS_DEN;
    if (!s || !*s)
        return;

    const char *slash = strchr(s, '/');
    unsigned n = (unsigned)atoi(s);
    unsigned d = slash ? (unsigned)atoi(slash + 1) : 1;
    if (n > 0 && d > 0) {
        *num = n;
        *den = d;
    }
}

static void SetColorimetry(video_format_t *v, const char *psz_colorimetry)
{
    v->b_color_range_full = false;

    if (psz_colorimetry && strcmp(psz_colorimetry, "BT2020") == 0) {
        v->primaries = COLOR_PRIMARIES_BT2020;
        v->space     = COLOR_SPACE_BT2020;
        /* No dedicated BT.2020 SDR OETF value in this VLC version; BT.2020's
           SDR OETF is numerically identical to BT.709's, so this fallback
           is exact, not approximate. */
        v->transfer  = TRANSFER_FUNC_BT709;
    } else if (psz_colorimetry && strcmp(psz_colorimetry, "ST2084") == 0) {
        v->primaries = COLOR_PRIMARIES_BT2020;
        v->space     = COLOR_SPACE_BT2020;
        v->transfer  = TRANSFER_FUNC_SMPTE_ST2084;
    } else if (psz_colorimetry && strcmp(psz_colorimetry, "HLG") == 0) {
        v->primaries = COLOR_PRIMARIES_BT2020;
        v->space     = COLOR_SPACE_BT2020;
        v->transfer  = TRANSFER_FUNC_HLG;
    } else {
        v->primaries = COLOR_PRIMARIES_BT709;
        v->space     = COLOR_SPACE_BT709;
        v->transfer  = TRANSFER_FUNC_BT709;
    }
}

/* Parses the fixed 12-byte RTP header (plus CSRC list / extension header if
 * present) and returns the byte offset where the RTP payload begins. */
static int ParseRTP(const uint8_t *p, size_t len, unsigned *hdr_len)
{
    if (len < RTP_HDR_MIN_LEN)
        return -1;
    if ((p[0] >> 6) != 2)          /* RTP version must be 2 */
        return -1;

    unsigned cc = p[0] & 0x0f;
    bool ext = (p[0] & 0x10) != 0;

    size_t off = RTP_HDR_MIN_LEN + (size_t)cc * 4;
    if (off + (ext ? 4 : 0) > len)
        return -1;
    if (ext) {
        uint16_t ext_words = ((uint16_t)p[off + 2] << 8) | p[off + 3];
        off += 4 + (size_t)ext_words * 4;
    }
    if (off > len)
        return -1;

    *hdr_len = (unsigned)off;
    return 0;
}

/* Parses the chained RFC4175 Sample Row Data headers that follow the
 * Extended Sequence Number. Returns the number of headers and the total
 * byte length of the header chain (payload data follows immediately). */
static int ParseLineHeaders(const uint8_t *p, size_t len,
                             rfc4175_line_hdr_t *hdrs, unsigned max_hdrs,
                             unsigned *out_n, size_t *out_hdr_bytes)
{
    unsigned n = 0;
    size_t off = 0;

    for (;;) {
        if (off + RFC4175_LINE_HDR_LEN > len || n >= max_hdrs)
            return -1;

        uint16_t length = ((uint16_t)p[off] << 8) | p[off + 1];
        uint16_t f_line  = ((uint16_t)p[off + 2] << 8) | p[off + 3];
        uint16_t c_off   = ((uint16_t)p[off + 4] << 8) | p[off + 5];

        hdrs[n].length  = length;
        hdrs[n].field2  = (f_line & 0x8000) != 0;
        hdrs[n].line_no = f_line & 0x7fff;
        hdrs[n].offset  = c_off & 0x7fff;
        bool cont       = (c_off & 0x8000) != 0;

        off += RFC4175_LINE_HDR_LEN;
        n++;
        if (!cont)
            break;
    }

    *out_n = n;
    *out_hdr_bytes = off;
    return 0;
}

static void DetectLineMode(demux_sys_t *p_sys, const rfc4175_line_hdr_t *hdrs, unsigned n)
{
    if (atomic_load(&p_sys->line_mode) != LINE_MODE_UNKNOWN)
        return;

    for (unsigned i = 0; i < n; i++) {
        if (hdrs[i].line_no >= LINE_MODE_SDI_LEGACY_MIN) {
            atomic_store(&p_sys->line_mode, LINE_MODE_SDI_LEGACY);
            return;
        }
    }

    /* Only commit to zero-based after enough packets that an unlucky first
       packet (e.g. only low field-one lines) can't cause a misdetection. */
    if (atomic_fetch_add(&p_sys->i_pkts_for_line_detect, 1) + 1 >= LINE_MODE_DETECT_MIN_PKTS)
        atomic_store(&p_sys->line_mode, LINE_MODE_ZERO_BASED);
}

/* Maps a wire line header to the output picture's row index, normalizing
 * away the SDI-legacy base offset when detected, then weaving fields for
 * interlace (ST2110-20 6.1.5 Note 2: field two's rows are interleaved
 * "below" the like-numbered rows of field one). Fully self-contained: only
 * this one header's own fields are needed, nothing from any other packet
 * (verified against RFC4175's own text -- see the file header comment). */
static bool MapLine(const demux_sys_t *p_sys, const rfc4175_line_hdr_t *h,
                     unsigned *out_line)
{
    unsigned line = h->line_no;
    line_mode_t mode = (line_mode_t)atomic_load(&p_sys->line_mode);

    if (mode == LINE_MODE_SDI_LEGACY) {
        unsigned base = p_sys->b_interlace
                       ? (h->field2 ? SMPTE274M_FIELD2_BASE : SMPTE274M_FIELD1_BASE)
                       : SMPTE274M_PROGRESSIVE_BASE;
        if (line < base)
            return false;
        line -= base;
    }

    *out_line = p_sys->b_interlace ? (line * 2 + (h->field2 ? 1 : 0)) : line;
    return true;
}

/* Unpacks RFC4175 YCbCr-4:2:2 10bit GPM pgroups (5 bytes -> Cb,Y0,Cr,Y1)
 * directly into the persistent master buffer's Y/U/V planes. Called
 * directly by whichever I/O thread received this packet -- no queueing,
 * no hand-off, no lock: this packet's target byte ranges never overlap
 * another packet's (different line, or a non-overlapping segment of the
 * same line), so concurrent calls from different threads are safe. */
static void WriteLines(demux_sys_t *p_sys, const rfc4175_line_hdr_t *hdrs,
                        unsigned n_hdrs, const uint8_t *data, size_t data_len)
{
    size_t off = 0;
    unsigned half_w = p_sys->i_width / 2;

    uint8_t *buf = p_sys->p_master_buf;
    uint16_t *y_plane = (uint16_t *)buf;
    uint16_t *u_plane = (uint16_t *)(buf + p_sys->i_y_plane_size);
    uint16_t *v_plane = (uint16_t *)(buf + p_sys->i_y_plane_size
                                          + p_sys->i_uv_plane_size);

    for (unsigned i = 0; i < n_hdrs; i++) {
        const rfc4175_line_hdr_t *h = &hdrs[i];

        atomic_fetch_add(&p_sys->i_hdrs_total, 1);
        if (h->field2)
            atomic_fetch_add(&p_sys->i_hdrs_field2, 1);

        if (off + h->length > data_len)
            break; /* the rest of the chain can't be trusted either */

        if (h->field2 && !p_sys->b_interlace) {
            off += h->length;
            continue;
        }
        if (h->length % 5 != 0 || h->offset % 2 != 0 || h->offset >= p_sys->i_width) {
            off += h->length;
            continue;
        }

        unsigned out_line;
        if (!MapLine(p_sys, h, &out_line) || out_line >= p_sys->i_height) {
            off += h->length;
            continue;
        }

        unsigned n_pgroups = h->length / 5;
        const uint8_t *seg = data + off;

        /* Bounds-check once per segment instead of once per pgroup. */
        unsigned max_pgroups = (p_sys->i_width - h->offset) / 2;
        if (n_pgroups > max_pgroups)
            n_pgroups = max_pgroups;

        uint16_t *y_row = y_plane + (size_t)out_line * p_sys->i_width + h->offset;
        uint16_t *u_row = u_plane + (size_t)out_line * half_w + h->offset / 2;
        uint16_t *v_row = v_plane + (size_t)out_line * half_w + h->offset / 2;

        for (unsigned j = 0; j < n_pgroups; j++) {
            const uint8_t *b = seg + j * 5;
            uint16_t cb = (uint16_t)((b[0] << 2) | (b[1] >> 6));
            uint16_t y0 = (uint16_t)(((b[1] & 0x3f) << 4) | (b[2] >> 4));
            uint16_t cr = (uint16_t)(((b[2] & 0x0f) << 6) | (b[3] >> 2));
            uint16_t y1 = (uint16_t)(((b[3] & 0x03) << 8) | b[4]);

            y_row[j * 2]     = y0;
            y_row[j * 2 + 1] = y1;
            u_row[j]         = cb;
            v_row[j]         = cr;
        }

        off += h->length;
    }
}

/* Parses one packet and writes its pixels straight into the master buffer.
 * Shared by every platform's I/O thread below. No frame/field bookkeeping
 * of any kind: each packet is fully self-describing (see file header). */
static void ProcessPacket(demux_sys_t *p_sys, const uint8_t *pkt, size_t len)
{
    unsigned hdr_len;
    if (ParseRTP(pkt, len, &hdr_len) != 0) {
        atomic_fetch_add(&p_sys->i_pkts_rtp_fail, 1);
        return;
    }

    const uint8_t *payload = pkt + hdr_len;
    size_t payload_len = len - hdr_len;
    if (payload_len < RFC4175_EXT_SEQ_LEN) {
        atomic_fetch_add(&p_sys->i_pkts_line_fail, 1);
        return;
    }
    payload += RFC4175_EXT_SEQ_LEN;
    payload_len -= RFC4175_EXT_SEQ_LEN;

    rfc4175_line_hdr_t hdrs[MAX_LINE_SEGMENTS];
    unsigned n_hdrs;
    size_t hdr_bytes;
    if (ParseLineHeaders(payload, payload_len, hdrs, MAX_LINE_SEGMENTS,
                          &n_hdrs, &hdr_bytes) != 0) {
        atomic_fetch_add(&p_sys->i_pkts_line_fail, 1);
        return;
    }

    DetectLineMode(p_sys, hdrs, n_hdrs);
    WriteLines(p_sys, hdrs, n_hdrs, payload + hdr_bytes, payload_len - hdr_bytes);
}

#ifdef _WIN32
/* Windows I/O thread: N of these all pull completions from the SAME
 * shared IOCP (the classic IOCP thread-pool pattern -- the OS load-
 * balances completions across however many threads are currently
 * waiting). Each does the full parse+unpack itself and reposts its
 * buffer immediately.
 *
 * The wait is non-alertable and bounded (1000ms): shutdown does not rely
 * on vlc_cancel()'s APC reaching this thread (unverified whether VLC's
 * Windows thread implementation even delivers it here, and an alertable
 * wait risks spinning on unrelated APCs at a rate high enough to burn a
 * full core for no useful work). b_stop_requested plus this call's own
 * timeout guarantees the loop exits on its own. */
static void *IoThread(void *data)
{
    demux_t *p_demux = data;
    demux_sys_t *p_sys = p_demux->p_sys;
    OVERLAPPED_ENTRY entries[IOCP_MAX_COMPLETIONS];

    for (;;) {
        if (atomic_load(&p_sys->b_stop_requested))
            break;

        ULONG n = 0;
        BOOL ok = GetQueuedCompletionStatusEx(p_sys->iocp, entries,
                                               IOCP_MAX_COMPLETIONS, &n, 1000, FALSE);
        int canc = vlc_savecancel();

        if (!ok) {
            if (GetLastError() != WAIT_TIMEOUT) {
                vlc_restorecancel(canc);
                break;
            }
            vlc_restorecancel(canc);
            continue;
        }

        for (ULONG e = 0; e < n; e++) {
            iocp_buf_t *b = (iocp_buf_t *)CONTAINING_RECORD(entries[e].lpOverlapped,
                                                              iocp_buf_t, overlapped);
            DWORD xfer = entries[e].dwNumberOfBytesTransferred;

            if (xfer > 0) {
                atomic_fetch_add(&p_sys->i_pkts_total, 1);
                ProcessPacket(p_sys, b->data, (size_t)xfer);
            }

            if (!atomic_load(&p_sys->b_stop_requested)) {
                b->wsabuf.buf = (char *)b->data;
                b->wsabuf.len = RECV_BUF_LEN;
                b->flags = 0;
                memset(&b->overlapped, 0, sizeof(b->overlapped));
                int r = WSARecv((SOCKET)p_sys->fd, &b->wsabuf, 1, NULL, &b->flags,
                                 &b->overlapped, NULL);
                if (r == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
                    msg_Warn(p_demux, "st2110: WSARecv repost failed (err=%d)", WSAGetLastError());
            }
        }

        vlc_restorecancel(canc);
    }

    return NULL;
}
#else
/* POSIX I/O thread: N of these all poll()+recv() the SAME shared socket.
 * poll() here resolves to vlc_poll() (see vlc_threads.h), VLC's
 * cancellation-aware wrapper -- raw select() has no such wrapper and
 * would make Close()'s vlc_join() hang forever. Multiple threads blocking
 * in poll()/recv() on one UDP socket is a standard, safe pattern: the
 * kernel hands each arriving datagram to exactly one waiter. */
static void *IoThread(void *data)
{
    demux_t *p_demux = data;
    demux_sys_t *p_sys = p_demux->p_sys;
    uint8_t pkt[RECV_BUF_LEN];
    struct pollfd ufd;

    ufd.fd = p_sys->fd;
    ufd.events = POLLIN;

    for (;;) {
        int ret = poll(&ufd, 1, 1000);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        int canc = vlc_savecancel();

        if (ret == 0) {
            vlc_restorecancel(canc);
            continue;
        }

        for (;;) {
            ssize_t len = recv(p_sys->fd, (char *)pkt, sizeof(pkt), 0);
            if (len <= 0) {
                if (len < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
                    break;
                break;
            }
            atomic_fetch_add(&p_sys->i_pkts_total, 1);
            ProcessPacket(p_sys, pkt, (size_t)len);
        }

        vlc_restorecancel(canc);
    }

    return NULL;
}
#endif

/* Independent sender thread: ticks on its own clock at the stream's
 * declared frame rate, completely decoupled from arrival timing. Each
 * tick just snapshots whatever is currently in the master buffer and
 * sends it -- packet loss just means some pixels are one tick stale,
 * never a discarded or blanked frame. Also owns the periodic (~5s)
 * diagnostic log, since it already wakes on a steady schedule. */
static void *SenderThread(void *data)
{
    demux_t *p_demux = data;
    demux_sys_t *p_sys = p_demux->p_sys;
    mtime_t frame_interval = (mtime_t)CLOCK_FREQ * p_sys->i_fps_den / p_sys->i_fps_num;
    mtime_t next_tick = mdate();
    mtime_t stat_window_start = next_tick;

    for (;;) {
        if (atomic_load(&p_sys->b_stop_requested))
            break;

        next_tick += frame_interval;
        mtime_t now = mdate();
        if (next_tick < now)
            next_tick = now; /* fell behind: resync rather than bursting to catch up */
        mwait(next_tick);

        if (atomic_load(&p_sys->b_stop_requested))
            break;

        block_t *p_block = block_Alloc(p_sys->i_buf_size);
        if (p_block) {
            memcpy(p_block->p_buffer, p_sys->p_master_buf, p_sys->i_buf_size);
            p_block->i_dts = p_block->i_pts = date_Get(&p_sys->pts);
            es_out_Control(p_demux->out, ES_OUT_SET_PCR, p_block->i_pts);
            es_out_Send(p_demux->out, p_sys->p_es, p_block);
        }
        date_Increment(&p_sys->pts, 1);

        if (mdate() - stat_window_start > 5 * CLOCK_FREQ) {
            unsigned pkts      = atomic_exchange(&p_sys->i_pkts_total, 0);
            unsigned rtp_fail  = atomic_exchange(&p_sys->i_pkts_rtp_fail, 0);
            unsigned line_fail = atomic_exchange(&p_sys->i_pkts_line_fail, 0);
            unsigned hdrs      = atomic_exchange(&p_sys->i_hdrs_total, 0);
            unsigned hdrs_f2   = atomic_exchange(&p_sys->i_hdrs_field2, 0);

            msg_Warn(p_demux, "st2110: pkts=%u rtp_fail=%u line_fail=%u",
                     pkts, rtp_fail, line_fail);
            if (p_sys->b_interlace)
                msg_Warn(p_demux, "st2110: line headers total=%u field2=%u (%.1f%%)",
                         hdrs, hdrs_f2, hdrs ? 100.0 * hdrs_f2 / hdrs : 0.0);

            stat_window_start = mdate();
        }
    }

    return NULL;
}

static int Control(demux_t *p_demux, int query, va_list args)
{
    switch (query) {
        case DEMUX_CAN_PAUSE:
        case DEMUX_CAN_SEEK:
        /* A live real-time source paces itself; VLC's own dvb/dtv access
           modules return false here for the same reason (verified against
           modules/access/dvb/access.c and modules/access/dtv/access.c). */
        case DEMUX_CAN_CONTROL_PACE:
            *va_arg(args, bool *) = false;
            return VLC_SUCCESS;

        case DEMUX_GET_PTS_DELAY:
            *va_arg(args, int64_t *) =
                INT64_C(1000) * var_InheritInteger(p_demux, "network-caching");
            return VLC_SUCCESS;

        case DEMUX_SET_PAUSE_STATE:
            return VLC_SUCCESS;

        default:
            return VLC_EGENERIC;
    }
}

static int Open(vlc_object_t *obj)
{
    demux_t *p_demux = (demux_t *)obj;
    demux_sys_t *p_sys = calloc(1, sizeof(*p_sys));
    if (!p_sys)
        return VLC_ENOMEM;
    p_demux->p_sys = p_sys;
    p_sys->fd = -1;

    /* MRL is st2110://<group>:<port>; psz_location has the scheme already
       stripped by VLC's URL parser. */
    const char *psz_loc = p_demux->psz_location;
    const char *psz_colon = psz_loc ? strrchr(psz_loc, ':') : NULL;
    if (!psz_loc || !psz_colon || psz_colon == psz_loc) {
        msg_Err(p_demux, "st2110: expected st2110://<group>:<port>, got \"%s\"",
                psz_loc ? psz_loc : "");
        goto error;
    }
    size_t host_len = (size_t)(psz_colon - psz_loc);
    if (host_len >= sizeof(p_sys->psz_group)) {
        msg_Err(p_demux, "st2110: group address too long");
        goto error;
    }
    memcpy(p_sys->psz_group, psz_loc, host_len);
    p_sys->psz_group[host_len] = '\0';

    p_sys->i_port = atoi(psz_colon + 1);
    if (p_sys->i_port <= 0 || p_sys->i_port > 65535) {
        msg_Err(p_demux, "st2110: invalid port in \"%s\"", psz_loc);
        goto error;
    }

    p_sys->psz_source  = var_InheritString(p_demux, "st2110-source");
    p_sys->i_width     = var_InheritInteger(p_demux, "st2110-width");
    p_sys->i_height    = var_InheritInteger(p_demux, "st2110-height");
    p_sys->i_depth     = var_InheritInteger(p_demux, "st2110-depth");
    p_sys->b_interlace = var_InheritBool(p_demux, "st2110-interlace");
    if (p_sys->i_width == 0)  p_sys->i_width  = DEFAULT_WIDTH;
    if (p_sys->i_height == 0) p_sys->i_height = DEFAULT_HEIGHT;
    if (p_sys->i_depth == 0)  p_sys->i_depth  = 10;

    if (p_sys->i_depth != 10) {
        msg_Err(p_demux, "st2110: only 10bit GPM is supported (st2110-depth=%u)", p_sys->i_depth);
        goto error;
    }

    char *psz_fps = var_InheritString(p_demux, "st2110-fps");
    ParseFraction(psz_fps, &p_sys->i_fps_num, &p_sys->i_fps_den);
    free(psz_fps);

    char *psz_sampling = var_InheritString(p_demux, "st2110-sampling");
    if (psz_sampling && strcmp(psz_sampling, "YCbCr-4:2:2") != 0)
        msg_Warn(p_demux, "st2110: only YCbCr-4:2:2 is implemented (got \"%s\"), proceeding anyway",
                 psz_sampling);
    free(psz_sampling);

    /* How many I/O threads: an explicit st2110-threads option always wins;
       otherwise auto-detect from the CPU count, reserving one core for
       VLC's own decode/convert/render pipeline (and the OS) so this
       module's reception work can't starve it -- these threads run at
       elevated priority below, since (unlike the old design) they now do
       the full, latency-critical receive+unpack pipeline themselves. */
    int i_threads_opt = var_InheritInteger(p_demux, "st2110-threads");
    if (i_threads_opt > 0) {
        p_sys->n_threads = (unsigned)i_threads_opt;
    } else {
        unsigned n_cpu = DetectCpuCount();
        p_sys->n_threads = n_cpu > 1 ? n_cpu - 1 : 1;
    }

    p_sys->fd = net_OpenDgram(p_demux, p_sys->psz_group, p_sys->i_port,
                               p_sys->psz_source, 0, IPPROTO_UDP);
    if (p_sys->fd == -1) {
        msg_Err(p_demux, "st2110: failed to join %s:%d", p_sys->psz_group, p_sys->i_port);
        goto error;
    }
    int rcvbuf = SO_RCVBUF_SIZE;
    setsockopt(p_sys->fd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));

#ifdef _WIN32
    p_sys->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!p_sys->iocp) {
        msg_Err(p_demux, "st2110: CreateIoCompletionPort failed (err=%lu)", GetLastError());
        goto error;
    }
    if (!CreateIoCompletionPort((HANDLE)(uintptr_t)(SOCKET)p_sys->fd, p_sys->iocp, 0, 0)) {
        msg_Err(p_demux, "st2110: failed to associate socket with IOCP (err=%lu)", GetLastError());
        goto error;
    }
    p_sys->n_iocp_bufs = p_sys->n_threads * IOCP_BUFS_PER_THREAD;
    p_sys->iocp_bufs = calloc(p_sys->n_iocp_bufs, sizeof(iocp_buf_t));
    if (!p_sys->iocp_bufs)
        goto error;
    for (unsigned i = 0; i < p_sys->n_iocp_bufs; i++) {
        iocp_buf_t *b = &p_sys->iocp_bufs[i];
        memset(&b->overlapped, 0, sizeof(b->overlapped));
        b->wsabuf.buf = (char *)b->data;
        b->wsabuf.len = RECV_BUF_LEN;
        b->flags = 0;
        int r = WSARecv((SOCKET)p_sys->fd, &b->wsabuf, 1, NULL, &b->flags, &b->overlapped, NULL);
        if (r == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            msg_Err(p_demux, "st2110: WSARecv failed to post buffer %u (err=%d)", i, WSAGetLastError());
            goto error;
        }
    }
#else
    { int flags = fcntl(p_sys->fd, F_GETFL, 0); fcntl(p_sys->fd, F_SETFL, flags | O_NONBLOCK); }
#endif

    p_sys->i_y_plane_size  = (size_t)p_sys->i_width * p_sys->i_height * 2;
    p_sys->i_uv_plane_size = (size_t)(p_sys->i_width / 2) * p_sys->i_height * 2;
    p_sys->i_buf_size = p_sys->i_y_plane_size + 2 * p_sys->i_uv_plane_size;
    p_sys->p_master_buf = calloc(1, p_sys->i_buf_size);
    if (!p_sys->p_master_buf)
        goto error;

    es_format_t fmt;
    es_format_Init(&fmt, VIDEO_ES, VLC_CODEC_I422_10L);
    fmt.video.i_width  = fmt.video.i_visible_width  = p_sys->i_width;
    fmt.video.i_height = fmt.video.i_visible_height = p_sys->i_height;
    fmt.video.i_sar_num = fmt.video.i_sar_den = 1;
    fmt.video.i_frame_rate      = p_sys->i_fps_num;
    fmt.video.i_frame_rate_base = p_sys->i_fps_den;
    char *psz_colorimetry = var_InheritString(p_demux, "st2110-colorimetry");
    SetColorimetry(&fmt.video, psz_colorimetry);
    free(psz_colorimetry);

    p_sys->p_es = es_out_Add(p_demux->out, &fmt);
    es_format_Clean(&fmt);
    if (!p_sys->p_es)
        goto error;

    date_Init(&p_sys->pts, p_sys->i_fps_num, p_sys->i_fps_den);
    date_Set(&p_sys->pts, VLC_TS_0);

    p_demux->pf_demux = NULL;      /* live source: pushed asynchronously by the sender thread */
    p_demux->pf_control = Control;

    msg_Info(p_demux, "st2110: %s:%d %ux%u interlace=%d fps=%u/%u threads=%u",
             p_sys->psz_group, p_sys->i_port, p_sys->i_width, p_sys->i_height,
             p_sys->b_interlace, p_sys->i_fps_num, p_sys->i_fps_den, p_sys->n_threads);

    p_sys->threads = calloc(p_sys->n_threads, sizeof(vlc_thread_t));
    if (!p_sys->threads)
        goto error;
    for (unsigned i = 0; i < p_sys->n_threads; i++) {
        if (vlc_clone(&p_sys->threads[i], IoThread, p_demux, VLC_THREAD_PRIORITY_INPUT)) {
            msg_Err(p_demux, "st2110: failed to spawn I/O thread %u", i);
            goto error;
        }
        p_sys->n_threads_started++;
    }

    if (vlc_clone(&p_sys->sender_thread, SenderThread, p_demux, VLC_THREAD_PRIORITY_OUTPUT)) {
        msg_Err(p_demux, "st2110: failed to spawn sender thread");
        goto error;
    }
    p_sys->b_sender_started = true;

    return VLC_SUCCESS;

error:
    Close(obj);
    return VLC_EGENERIC;
}

static void Close(vlc_object_t *obj)
{
    demux_t *p_demux = (demux_t *)obj;
    demux_sys_t *p_sys = p_demux->p_sys;
    if (!p_sys)
        return;

    atomic_store(&p_sys->b_stop_requested, true);

    if (p_sys->b_sender_started) {
        vlc_cancel(p_sys->sender_thread);
        vlc_join(p_sys->sender_thread, NULL);
    }

    for (unsigned i = 0; i < p_sys->n_threads_started; i++) {
        vlc_cancel(p_sys->threads[i]);
        vlc_join(p_sys->threads[i], NULL);
    }
    free(p_sys->threads);

#ifdef _WIN32
    if (p_sys->iocp) {
        /* All I/O threads have already been joined above, so nothing will
           observe these completions; this purely ensures the OS is done
           writing into iocp_bufs before free() reclaims that memory. */
        if (p_sys->fd != -1)
            CancelIoEx((HANDLE)(uintptr_t)(SOCKET)p_sys->fd, NULL);
        if (p_sys->iocp_bufs) {
            OVERLAPPED_ENTRY drain[IOCP_MAX_COMPLETIONS];
            ULONG n;
            while (GetQueuedCompletionStatusEx(p_sys->iocp, drain, IOCP_MAX_COMPLETIONS, &n, 0, FALSE) && n > 0)
                ; /* discard */
        }
        CloseHandle(p_sys->iocp);
        p_sys->iocp = NULL;
    }
    free(p_sys->iocp_bufs);
#endif

    if (p_sys->fd != -1)
        net_Close(p_sys->fd);

    free(p_sys->psz_source);
    free(p_sys->p_master_buf);
    free(p_sys);
    p_demux->p_sys = NULL;
}
