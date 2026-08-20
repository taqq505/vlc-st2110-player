/*
 * st2110.c - VLC access_demux plugin for SMPTE ST 2110-20 (RFC 4175) reception
 *
 * Receives 10bit YCbCr 4:2:2 GPM video carried per RFC 4175 over RTP/UDP and
 * outputs VLC_CODEC_I422_10L. See docs/vlc-st2110_receive-module_仕様書.md.
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
#endif

/* ---- RFC 4175 / RTP constants ---- */
#define RTP_HDR_MIN_LEN        12
#define RFC4175_EXT_SEQ_LEN     2
#define RFC4175_LINE_HDR_LEN    6

#define MAX_LINE_SEGMENTS      64
#define RECV_BUF_LEN         2048

#define DEFAULT_WIDTH         1920
#define DEFAULT_HEIGHT        1080
#define DEFAULT_FPS_NUM       30000
#define DEFAULT_FPS_DEN        1001

#define SO_RCVBUF_SIZE (32 * 1024 * 1024)

/* Pixel-unpacking is the CPU-heavy part of this module (tens of millions of
 * pgroup unpacks/sec for full HD 4:2:2 10bit); the receive thread hands it
 * off to these workers so recv()/poll() itself never falls behind and
 * starves the OS socket buffer. 4-core assumption: 1 receive thread + 3
 * workers. */
#define N_WORKERS     3
#define QUEUE_SLOTS  32
#define BATCH_SIZE   16

#ifdef _WIN32
/*
 * On Windows, a poll()+recv() loop pays a full syscall pair per UDP
 * datagram; at the ~80k-140k pkts/sec a live uncompressed HD feed produces,
 * that alone can saturate a core independent of the actual unpack work
 * (confirmed: parallelizing the unpack work across worker threads did not
 * relieve the receive thread's own core). This is the same problem
 * high-performance packet capture (Npcap/WinPcap, and Microsoft's own
 * guidance for high-throughput network servers) solves with asynchronous,
 * batched I/O instead of one blocking call per packet: pre-post many
 * overlapped WSARecv buffers, and retrieve completions in bulk via
 * GetQueuedCompletionStatusEx(), which can dequeue MANY finished receives
 * in a single call instead of one syscall per packet.
 */
# define IOCP_BUFFER_COUNT      64  /* overlapped receives kept in flight  */
# define IOCP_MAX_COMPLETIONS   64  /* drained per GetQueuedCompletionStatusEx call */

typedef struct {
    OVERLAPPED overlapped;
    WSABUF     wsabuf;
    DWORD      flags;
    uint8_t    data[RECV_BUF_LEN];
} iocp_buf_t;
#endif

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

/* One packet's already-parsed headers plus its raw payload bytes. */
typedef struct {
    rfc4175_line_hdr_t hdrs[MAX_LINE_SEGMENTS];
    unsigned            n_hdrs;
    uint8_t             data[RECV_BUF_LEN];
    size_t              data_len;
} packet_data_t;

/* A batch of packets handed to one worker in a single dispatch. Batching
 * matters: at ~80k+ pkts/sec, waking a sleeping worker thread once per
 * packet costs more in OS wake/context-switch overhead than the few
 * microseconds of actual unpack work being handed off, so per-packet
 * dispatch can cost MORE than doing the work inline. Batching amortizes
 * that wake cost across BATCH_SIZE packets. */
typedef struct {
    packet_data_t packets[BATCH_SIZE];
    unsigned       n_packets;
} work_item_t;

/* Single-producer (the receive thread) / single-consumer (one worker)
 * bounded queue. SPSC keeps the concurrency reasoning simple: the only
 * two parties touching `head`/`tail` are always the same two threads.
 * A slot is only visible to the worker once `count` covers it (see
 * StartBatch/PublishBatch), so the receive thread can fill `items[tail]`
 * without holding the lock. */
typedef struct {
    vlc_mutex_t lock;
    vlc_cond_t  cond_work;   /* producer -> worker: a batch was published */
    vlc_cond_t  cond_avail;  /* worker -> producer: count just decreased
                                 (frees a slot, and/or satisfies a drain) */
    work_item_t items[QUEUE_SLOTS];
    unsigned    head, tail, count;
    bool        b_shutdown;
} worker_queue_t;

typedef struct {
    demux_t *p_demux;
    unsigned idx;
} worker_ctx_t;

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

    /* line-number convention, detected from the first few packets */
    line_mode_t line_mode;

    /* elementary stream */
    es_out_id_t *p_es;
    date_t       pts;

    /* Frame accumulation: workers write pixels directly into the block_t
       that will be es_out_Send()'d, instead of a separate scratch buffer
       that then has to be memcpy'd into one -- on a core-constrained
       machine an unconditional ~8MB copy every ~30-60 times/sec is real,
       avoidable cost. Only swapped (by the receive thread, in
       FinalizeFrame) while no worker is active, i.e. always right after
       DrainAllWorkers(), so workers reading it concurrently is safe. */
    block_t     *p_cur_block;
    size_t       i_buf_size;
    size_t       i_y_plane_size;
    size_t       i_uv_plane_size;
    atomic_bool *p_line_filled;   /* written by multiple workers concurrently */
    atomic_uint  i_lines_filled;

    /* Field/frame boundary detection for interlace (see ProcessPacket):
       driven by RTP timestamp changes rather than the marker bit, since
       RFC4175 guarantees every packet of a field shares one timestamp,
       while marker-bearing packets can be lost or reordered. Remembers
       BOTH of the current frame's field timestamps (not just a change
       count) so that ordinary network reordering -- a stray packet from
       the field just finishing arriving after the next field has already
       started -- can't be misread as the start of a third field. Touched
       only by the (single) receive thread, so plain types are fine. */
    uint32_t ts_seen[2];
    unsigned n_ts_seen;

    /* per-frame drop counters, reset after each finalized/dropped frame;
       written from multiple workers concurrently, hence atomic */
    atomic_uint i_drop_field2;
    atomic_uint i_drop_offset;
    atomic_uint i_drop_line_range;
    atomic_uint i_drop_stride;

    /* receive thread */
    vlc_thread_t thread;
    bool         b_thread_started;
    /* Cooperative shutdown signal for the Windows IOCP receive path: a
       blocked GetQueuedCompletionStatusEx() is not necessarily a
       vlc_cancel() cancellation point the way vlc_poll() is, so Close()
       sets this before cancelling and the receive loop checks it on every
       wakeup (at least once/sec, via the call's timeout) as a bounded-time
       fallback -- this must never be able to hang the way the old
       select()-based design once did. */
    atomic_bool  b_stop_requested;

#ifdef _WIN32
    HANDLE     iocp;
    iocp_buf_t iocp_bufs[IOCP_BUFFER_COUNT];
#endif

    /* pixel-unpack worker pool (see WriteLines/WorkerThread) */
    worker_queue_t queues[N_WORKERS];
    vlc_thread_t   worker_threads[N_WORKERS];
    worker_ctx_t   worker_ctx[N_WORKERS];
    unsigned       n_workers_started;
    unsigned       i_next_worker;
    bool           b_queues_initialized;

    /* rolling diagnostics, reset every ~5s */
    mtime_t  i_stat_window_start;
    unsigned i_pkts_total;
    unsigned i_pkts_rtp_fail;
    unsigned i_pkts_line_fail;
    unsigned i_pkts_no_frame;
    unsigned i_idle_polls;
};

static int  Open(vlc_object_t *);
static void Close(vlc_object_t *);
static int  Control(demux_t *, int, va_list);
static void *ReceiveThread(void *);
static void *WorkerThread(void *);

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
vlc_module_end()

/* ---- helpers ---- */

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
static int ParseRTP(const uint8_t *p, size_t len, unsigned *hdr_len,
                     uint32_t *timestamp, bool *marker)
{
    if (len < RTP_HDR_MIN_LEN)
        return -1;
    if ((p[0] >> 6) != 2)          /* RTP version must be 2 */
        return -1;

    unsigned cc = p[0] & 0x0f;
    bool ext = (p[0] & 0x10) != 0;
    *marker = (p[1] & 0x80) != 0;
    *timestamp = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16)
               | ((uint32_t)p[6] << 8)  |  (uint32_t)p[7];

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

static void DetectLineMode(demux_t *p_demux, demux_sys_t *p_sys,
                            const rfc4175_line_hdr_t *hdrs, unsigned n)
{
    if (p_sys->line_mode != LINE_MODE_UNKNOWN)
        return;

    for (unsigned i = 0; i < n; i++) {
        if (hdrs[i].line_no >= LINE_MODE_SDI_LEGACY_MIN) {
            p_sys->line_mode = LINE_MODE_SDI_LEGACY;
            msg_Info(p_demux, "st2110: detected SDI-legacy (SMPTE274M raw raster) line numbering");
            return;
        }
    }

    /* Only commit to zero-based after enough packets that an unlucky first
       packet (e.g. only low field-one lines) can't cause a misdetection. */
    if (p_sys->i_pkts_total >= LINE_MODE_DETECT_MIN_PKTS) {
        p_sys->line_mode = LINE_MODE_ZERO_BASED;
        msg_Info(p_demux, "st2110: detected zero-based (ST2110-20 spec) line numbering");
    }
}

/* Maps a wire line header to the output picture's row index, normalizing
 * away the SDI-legacy base offset when detected, then weaving fields for
 * interlace (ST2110-20 6.1.5 Note 2: field two's rows are interleaved
 * "below" the like-numbered rows of field one). */
static bool MapLine(const demux_sys_t *p_sys, const rfc4175_line_hdr_t *h,
                     unsigned *out_line)
{
    unsigned line = h->line_no;

    if (p_sys->line_mode == LINE_MODE_SDI_LEGACY) {
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
 * directly into the current output block's Y/U/V planes. */
static void WriteLines(demux_sys_t *p_sys, const rfc4175_line_hdr_t *hdrs,
                        unsigned n_hdrs, const uint8_t *data, size_t data_len)
{
    if (!p_sys->p_cur_block)
        return; /* block_Alloc() failed when this cycle started; drop it */

    size_t off = 0;
    unsigned half_w = p_sys->i_width / 2;

    uint8_t *buf = p_sys->p_cur_block->p_buffer;
    uint16_t *y_plane = (uint16_t *)buf;
    uint16_t *u_plane = (uint16_t *)(buf + p_sys->i_y_plane_size);
    uint16_t *v_plane = (uint16_t *)(buf + p_sys->i_y_plane_size
                                          + p_sys->i_uv_plane_size);

    for (unsigned i = 0; i < n_hdrs; i++) {
        const rfc4175_line_hdr_t *h = &hdrs[i];

        if (off + h->length > data_len) {
            p_sys->i_drop_stride++;
            break; /* the rest of the chain can't be trusted either */
        }

        if (h->field2 && !p_sys->b_interlace) {
            p_sys->i_drop_field2++;
            off += h->length;
            continue;
        }
        if (h->length % 5 != 0 || h->offset % 2 != 0 || h->offset >= p_sys->i_width) {
            p_sys->i_drop_stride++;
            off += h->length;
            continue;
        }

        unsigned out_line;
        if (!MapLine(p_sys, h, &out_line) || out_line >= p_sys->i_height) {
            p_sys->i_drop_line_range++;
            off += h->length;
            continue;
        }

        unsigned n_pgroups = h->length / 5;
        const uint8_t *seg = data + off;

        /* Bounds-check once per segment instead of once per pgroup: the
           inner loop runs up to ~31M times/sec for a full HD 4:2:2 10bit
           feed, so a branch on every iteration is measurable overhead. */
        unsigned max_pgroups = (p_sys->i_width - h->offset) / 2;
        if (n_pgroups > max_pgroups) {
            n_pgroups = max_pgroups;
            p_sys->i_drop_stride++;
        }

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

        /* atomic_exchange, not a separate load+store: two workers can touch
           the same output line (its packets round-robin across workers by
           packet, not by line), so "check then set" would race. */
        if (!atomic_exchange(&p_sys->p_line_filled[out_line], true))
            p_sys->i_lines_filled++;

        off += h->length;
    }
}

/* Reserves the next free slot on `q` for the receive thread to fill
 * directly (no intermediate copy): the slot is invisible to the worker
 * until PublishBatch() runs, so filling it needs no lock held. */
static work_item_t *StartBatch(worker_queue_t *q)
{
    vlc_mutex_lock(&q->lock);
    while (q->count == QUEUE_SLOTS)
        vlc_cond_wait(&q->cond_avail, &q->lock);
    work_item_t *slot = &q->items[q->tail];
    vlc_mutex_unlock(&q->lock);
    return slot;
}

/* Publishes a batch the receive thread just filled via StartBatch(). */
static void PublishBatch(worker_queue_t *q, work_item_t *slot, unsigned n_packets)
{
    slot->n_packets = n_packets;
    vlc_mutex_lock(&q->lock);
    q->tail = (q->tail + 1) % QUEUE_SLOTS;
    q->count++;
    vlc_cond_signal(&q->cond_work);
    vlc_mutex_unlock(&q->lock);
}

/* Blocks until every batch already published on `q` has been fully
 * unpacked. Must be called (for every queue) before FinalizeFrame reads
 * the picture buffer, so no worker is still writing into it. */
static void DrainQueue(worker_queue_t *q)
{
    vlc_mutex_lock(&q->lock);
    while (q->count != 0)
        vlc_cond_wait(&q->cond_avail, &q->lock);
    vlc_mutex_unlock(&q->lock);
}

static void DrainAllWorkers(demux_sys_t *p_sys)
{
    for (unsigned i = 0; i < N_WORKERS; i++)
        DrainQueue(&p_sys->queues[i]);
}

/* Receive-thread-local batching state: accumulates parsed packets into the
 * current worker's reserved slot and publishes it every BATCH_SIZE
 * packets (or on demand via FlushBatch, e.g. before a frame boundary). */
typedef struct {
    worker_queue_t *q;
    work_item_t    *slot;
    unsigned         n;
} batch_state_t;

static void FlushBatch(batch_state_t *b)
{
    if (b->q && b->n > 0)
        PublishBatch(b->q, b->slot, b->n);
    b->q = NULL;
    b->slot = NULL;
    b->n = 0;
}

static void AddToBatch(demux_sys_t *p_sys, batch_state_t *b,
                        const rfc4175_line_hdr_t *hdrs, unsigned n_hdrs,
                        const uint8_t *data, size_t data_len)
{
    if (!b->q) {
        b->q = &p_sys->queues[p_sys->i_next_worker];
        p_sys->i_next_worker = (p_sys->i_next_worker + 1) % N_WORKERS;
        b->slot = StartBatch(b->q);
        b->n = 0;
    }

    packet_data_t *pd = &b->slot->packets[b->n++];
    memcpy(pd->hdrs, hdrs, n_hdrs * sizeof(*hdrs));
    pd->n_hdrs = n_hdrs;
    memcpy(pd->data, data, data_len);
    pd->data_len = data_len;

    if (b->n == BATCH_SIZE)
        FlushBatch(b);
}

static void *WorkerThread(void *data)
{
    worker_ctx_t *ctx = data;
    demux_sys_t *p_sys = ctx->p_demux->p_sys;
    worker_queue_t *q = &p_sys->queues[ctx->idx];

    for (;;) {
        vlc_mutex_lock(&q->lock);
        while (q->count == 0 && !q->b_shutdown)
            vlc_cond_wait(&q->cond_work, &q->lock);
        if (q->count == 0 && q->b_shutdown) {
            vlc_mutex_unlock(&q->lock);
            break;
        }
        unsigned idx = q->head;
        vlc_mutex_unlock(&q->lock);

        /* Only this worker ever reads `items[idx]` (SPSC), and the
           producer can't reuse it until `count` drops below QUEUE_SLOTS
           again below -- safe to process without holding the lock. */
        work_item_t *item = &q->items[idx];
        for (unsigned i = 0; i < item->n_packets; i++) {
            packet_data_t *pd = &item->packets[i];
            WriteLines(p_sys, pd->hdrs, pd->n_hdrs, pd->data, pd->data_len);
        }

        vlc_mutex_lock(&q->lock);
        q->head = (q->head + 1) % QUEUE_SLOTS;
        q->count--;
        vlc_cond_broadcast(&q->cond_avail);
        vlc_mutex_unlock(&q->lock);
    }

    return NULL;
}

static void FinalizeFrame(demux_t *p_demux, demux_sys_t *p_sys)
{
    unsigned min_lines = p_sys->i_height / 2;
    bool b_ok = p_sys->i_lines_filled >= min_lines;

    if (!b_ok) {
        p_sys->i_pkts_no_frame++;
    } else if (p_sys->p_cur_block) {
        /* Workers already wrote pixels straight into this block (see
           WriteLines) -- no ~8MB copy needed here, just send it and swap
           in a fresh one for the next cycle. */
        p_sys->p_cur_block->i_dts = p_sys->p_cur_block->i_pts = date_Get(&p_sys->pts);
        es_out_Control(p_demux->out, ES_OUT_SET_PCR, p_sys->p_cur_block->i_pts);
        es_out_Send(p_demux->out, p_sys->p_es, p_sys->p_cur_block);
        date_Increment(&p_sys->pts, 1);

        p_sys->p_cur_block = block_Alloc(p_sys->i_buf_size);
        if (p_sys->p_cur_block)
            memset(p_sys->p_cur_block->p_buffer, 0, p_sys->i_buf_size);
    }

    /* Minor per-frame shortfall is normal on a live uncompressed feed; only
       log when it's bad enough to be worth a human's attention, so this
       doesn't itself become a meaningful CPU cost at ~30-60 calls/sec. */
    if (p_sys->i_drop_field2 || p_sys->i_drop_offset || p_sys->i_drop_line_range
     || p_sys->i_drop_stride || !b_ok) {
        msg_Warn(p_demux, "st2110: frame lines=%u/%u drop(field2=%u offset=%u range=%u stride=%u)",
                 p_sys->i_lines_filled, p_sys->i_height, p_sys->i_drop_field2,
                 p_sys->i_drop_offset, p_sys->i_drop_line_range, p_sys->i_drop_stride);
    }

    /* A sent block was already replaced above by a freshly zeroed one. A
       discarded (malformed) frame reuses the SAME block for the next
       cycle -- clear only the lines this cycle actually touched, since
       everything else in it is already zero from before. */
    if (!b_ok && p_sys->p_cur_block) {
        unsigned half_w = p_sys->i_width / 2;
        size_t y_row_bytes = (size_t)p_sys->i_width * 2;
        size_t uv_row_bytes = (size_t)half_w * 2;
        uint8_t *y_plane = p_sys->p_cur_block->p_buffer;
        uint8_t *u_plane = y_plane + p_sys->i_y_plane_size;
        uint8_t *v_plane = u_plane + p_sys->i_uv_plane_size;
        for (unsigned line = 0; line < p_sys->i_height; line++) {
            if (!p_sys->p_line_filled[line])
                continue;
            memset(y_plane + (size_t)line * y_row_bytes, 0, y_row_bytes);
            memset(u_plane + (size_t)line * uv_row_bytes, 0, uv_row_bytes);
            memset(v_plane + (size_t)line * uv_row_bytes, 0, uv_row_bytes);
        }
    }

    for (unsigned line = 0; line < p_sys->i_height; line++)
        p_sys->p_line_filled[line] = false;
    p_sys->i_lines_filled = 0;
    p_sys->i_drop_field2 = p_sys->i_drop_offset = 0;
    p_sys->i_drop_line_range = p_sys->i_drop_stride = 0;
}

/* Parses one packet and feeds it into the frame-accumulation state machine:
 * line-mode detection, interlace field-boundary tracking (RTP timestamp
 * based -- see the comment inline below), batched dispatch to the worker
 * pool, and finalizing a frame when it's complete. Shared by every
 * platform's packet-acquisition loop below. */
static void ProcessPacket(demux_t *p_demux, demux_sys_t *p_sys, batch_state_t *batch,
                           const uint8_t *pkt, size_t len)
{
    unsigned hdr_len;
    uint32_t ts;
    bool marker;
    if (ParseRTP(pkt, len, &hdr_len, &ts, &marker) != 0) {
        p_sys->i_pkts_rtp_fail++;
        return;
    }

    const uint8_t *payload = pkt + hdr_len;
    size_t payload_len = len - hdr_len;
    if (payload_len < RFC4175_EXT_SEQ_LEN) {
        p_sys->i_pkts_line_fail++;
        return;
    }
    payload += RFC4175_EXT_SEQ_LEN;
    payload_len -= RFC4175_EXT_SEQ_LEN;

    rfc4175_line_hdr_t hdrs[MAX_LINE_SEGMENTS];
    unsigned n_hdrs;
    size_t hdr_bytes;
    if (ParseLineHeaders(payload, payload_len, hdrs, MAX_LINE_SEGMENTS,
                          &n_hdrs, &hdr_bytes) != 0) {
        p_sys->i_pkts_line_fail++;
        return;
    }

    DetectLineMode(p_demux, p_sys, hdrs, n_hdrs);

    if (p_sys->b_interlace) {
        /* Field/frame boundaries are delimited by RTP timestamp changes
           (RFC4175: every packet of one field shares one timestamp), not
           by the marker bit -- a lost or reordered marker packet must not
           be able to desync the field count. A frame is exactly two
           distinct timestamps (field one, field two); a packet carrying
           either of the two ALREADY-SEEN timestamps for this frame is
           just ordinary reordering and must not be mistaken for a third
           field starting -- only a genuinely new (third) timestamp, seen
           after two are already known, means the next frame has begun. */
        bool b_known = (p_sys->n_ts_seen >= 1 && ts == p_sys->ts_seen[0])
                     || (p_sys->n_ts_seen >= 2 && ts == p_sys->ts_seen[1]);
        if (!b_known) {
            if (p_sys->n_ts_seen >= 2) {
                /* This packet belongs to the new field/frame, so it must
                   not be in the batch being flushed here. */
                FlushBatch(batch);
                DrainAllWorkers(p_sys);
                FinalizeFrame(p_demux, p_sys);
                p_sys->n_ts_seen = 0;
            }
            p_sys->ts_seen[p_sys->n_ts_seen++] = ts;
        }
        AddToBatch(p_sys, batch, hdrs, n_hdrs, payload + hdr_bytes, payload_len - hdr_bytes);
    } else {
        AddToBatch(p_sys, batch, hdrs, n_hdrs, payload + hdr_bytes, payload_len - hdr_bytes);
        if (marker) {
            /* This packet (the frame's last) must be included. */
            FlushBatch(batch);
            DrainAllWorkers(p_sys);
            FinalizeFrame(p_demux, p_sys);
        }
    }
}

static void CheckStatsWindow(demux_t *p_demux, demux_sys_t *p_sys)
{
    if (mdate() - p_sys->i_stat_window_start <= 5 * CLOCK_FREQ)
        return;
    if (p_sys->i_pkts_rtp_fail || p_sys->i_pkts_line_fail || p_sys->i_pkts_no_frame)
        msg_Warn(p_demux, "st2110: pkts=%u rtp_fail=%u line_fail=%u no_frame=%u",
                 p_sys->i_pkts_total, p_sys->i_pkts_rtp_fail,
                 p_sys->i_pkts_line_fail, p_sys->i_pkts_no_frame);
    p_sys->i_pkts_total = p_sys->i_pkts_rtp_fail = 0;
    p_sys->i_pkts_line_fail = p_sys->i_pkts_no_frame = 0;
    p_sys->i_idle_polls = 0;
    p_sys->i_stat_window_start = mdate();
}

#ifdef _WIN32
/* Windows receive thread: asynchronous, batched I/O via IOCP instead of a
 * blocking-call-per-packet model (see the IOCP_BUFFER_COUNT comment above
 * for why). Many overlapped WSARecv()s are kept in flight at once; each
 * wakeup can harvest many finished ones via GetQueuedCompletionStatusEx().
 *
 * The wait is non-alertable: shutdown does not rely on vlc_cancel()'s APC
 * reaching this thread (unverified whether VLC's Windows thread
 * implementation even delivers it here), and an alertable wait risks
 * returning early on unrelated APCs at a rate high enough to spin the CPU
 * for no useful work. Instead, b_stop_requested plus this call's own
 * 1000ms timeout is a bounded-time fallback that guarantees the loop
 * exits on its own, so Close()'s vlc_join() can never hang the way it did
 * with the old select()-based design. */
static void *ReceiveThread(void *data)
{
    demux_t *p_demux = data;
    demux_sys_t *p_sys = p_demux->p_sys;
    batch_state_t batch = { NULL, NULL, 0 };
    OVERLAPPED_ENTRY entries[IOCP_MAX_COMPLETIONS];

    p_sys->i_stat_window_start = mdate();

    for (;;) {
        if (atomic_load(&p_sys->b_stop_requested))
            break;

        ULONG n = 0;
        /* fAlertable=FALSE: b_stop_requested + this call's own 1000ms
           timeout are the actual shutdown guarantee (see the comment
           above), so there's no need for an alertable wait here -- and if
           VLC's Windows thread implementation delivers APCs to this thread
           for any other reason, an alertable wait would return early on
           every one of them with nothing to do, which at a high call
           frequency is a real way to burn a full core doing no useful
           work at all. */
        BOOL ok = GetQueuedCompletionStatusEx(p_sys->iocp, entries,
                                               IOCP_MAX_COMPLETIONS, &n, 1000, FALSE);
        int canc = vlc_savecancel();

        if (!ok) {
            DWORD err = GetLastError();
            if (err == WAIT_TIMEOUT) {
                p_sys->i_idle_polls++;
                if (p_sys->i_idle_polls % 10 == 0)
                    msg_Warn(p_demux, "st2110: no data received for ~%us", p_sys->i_idle_polls);
            } else {
                /* IOCP handle gone or another fatal error */
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
                p_sys->i_pkts_total++;
                ProcessPacket(p_demux, p_sys, &batch, b->data, (size_t)xfer);
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

        CheckStatsWindow(p_demux, p_sys);
        FlushBatch(&batch);
        vlc_restorecancel(canc);
    }

    return NULL;
}
#else
/* POSIX receive thread. poll() here resolves to vlc_poll() (see
 * vlc_threads.h), which is VLC's cancellation-aware wrapper -- raw
 * select() has no such wrapper and would make Close()'s vlc_join() hang
 * forever. */
static void *ReceiveThread(void *data)
{
    demux_t *p_demux = data;
    demux_sys_t *p_sys = p_demux->p_sys;
    uint8_t pkt[RECV_BUF_LEN];
    struct pollfd ufd;
    batch_state_t batch = { NULL, NULL, 0 };

    ufd.fd = p_sys->fd;
    ufd.events = POLLIN;

    p_sys->i_stat_window_start = mdate();

    for (;;) {
        int ret = poll(&ufd, 1, 1000);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        int canc = vlc_savecancel();

        if (ret == 0) {
            p_sys->i_idle_polls++;
            if (p_sys->i_idle_polls % 10 == 0)
                msg_Warn(p_demux, "st2110: no data received for ~%us", p_sys->i_idle_polls);
            vlc_restorecancel(canc);
            continue;
        }

        /* Drain every packet already queued by the kernel before going
           back to poll(): at high packet rates several are typically
           waiting by the time poll() wakes us, and processing them all
           here amortizes poll()'s syscall cost across the whole burst
           instead of paying it once per packet. */
        for (;;) {
            ssize_t len = recv(p_sys->fd, (char *)pkt, sizeof(pkt), 0);
            if (len <= 0) {
                if (len < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
                    break;
                break; /* real error or 0-length packet: stop this burst */
            }
            p_sys->i_pkts_total++;
            ProcessPacket(p_demux, p_sys, &batch, pkt, (size_t)len);
            CheckStatsWindow(p_demux, p_sys);
        }

        /* Don't let a partial batch sit un-dispatched until the next burst
           arrives -- flush whatever's accumulated before going back to
           poll(). */
        FlushBatch(&batch);

        vlc_restorecancel(canc);
    }

    return NULL;
}
#endif

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

    p_sys->fd = net_OpenDgram(p_demux, p_sys->psz_group, p_sys->i_port,
                               p_sys->psz_source, 0, IPPROTO_UDP);
    if (p_sys->fd == -1) {
        msg_Err(p_demux, "st2110: failed to join %s:%d", p_sys->psz_group, p_sys->i_port);
        goto error;
    }
    int rcvbuf = SO_RCVBUF_SIZE;
    setsockopt(p_sys->fd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));

#ifdef _WIN32
    /* Set up IOCP-based asynchronous receive (see the ReceiveThread comment
       for why): associate the socket with a completion port, then keep
       IOCP_BUFFER_COUNT overlapped WSARecv()s in flight at all times. */
    p_sys->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!p_sys->iocp) {
        msg_Err(p_demux, "st2110: CreateIoCompletionPort failed (err=%lu)", GetLastError());
        goto error;
    }
    if (!CreateIoCompletionPort((HANDLE)(uintptr_t)(SOCKET)p_sys->fd, p_sys->iocp, 0, 0)) {
        msg_Err(p_demux, "st2110: failed to associate socket with IOCP (err=%lu)", GetLastError());
        goto error;
    }
    for (unsigned i = 0; i < IOCP_BUFFER_COUNT; i++) {
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
    /* ReceiveThread uses poll() only to wait for the first packet of a
       burst, then drains everything already queued with plain recv()
       calls before going back to poll() -- at ~80k+ pkts/sec on a live HD
       feed, paying poll()'s syscall cost per packet instead of per burst
       is itself enough to peg a core. */
    { int flags = fcntl(p_sys->fd, F_GETFL, 0); fcntl(p_sys->fd, F_SETFL, flags | O_NONBLOCK); }
#endif

    p_sys->i_y_plane_size  = (size_t)p_sys->i_width * p_sys->i_height * 2;
    p_sys->i_uv_plane_size = (size_t)(p_sys->i_width / 2) * p_sys->i_height * 2;
    p_sys->i_buf_size = p_sys->i_y_plane_size + 2 * p_sys->i_uv_plane_size;
    p_sys->p_cur_block = block_Alloc(p_sys->i_buf_size);
    p_sys->p_line_filled = calloc(p_sys->i_height, sizeof(atomic_bool));
    if (!p_sys->p_cur_block || !p_sys->p_line_filled)
        goto error;
    memset(p_sys->p_cur_block->p_buffer, 0, p_sys->i_buf_size);

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

    p_demux->pf_demux = NULL;      /* live source: pushed asynchronously by ReceiveThread */
    p_demux->pf_control = Control;

    msg_Info(p_demux, "st2110: %s:%d %ux%u interlace=%d fps=%u/%u",
             p_sys->psz_group, p_sys->i_port, p_sys->i_width, p_sys->i_height,
             p_sys->b_interlace, p_sys->i_fps_num, p_sys->i_fps_den);

    for (unsigned i = 0; i < N_WORKERS; i++) {
        worker_queue_t *q = &p_sys->queues[i];
        vlc_mutex_init(&q->lock);
        vlc_cond_init(&q->cond_work);
        vlc_cond_init(&q->cond_avail);
    }
    p_sys->b_queues_initialized = true;

    for (unsigned i = 0; i < N_WORKERS; i++) {
        p_sys->worker_ctx[i].p_demux = p_demux;
        p_sys->worker_ctx[i].idx = i;
        /* LOW, not INPUT: these do bulk CPU-bound unpack work with no
           latency deadline of their own. On a core-constrained machine,
           giving them the same elevated priority as the time-critical
           receive thread lets them compete with (and potentially starve)
           VLC's own decode/convert/render threads for the CPU, which
           would stall playback even if this module's own pipeline is
           keeping up fine. */
        if (vlc_clone(&p_sys->worker_threads[i], WorkerThread, &p_sys->worker_ctx[i],
                      VLC_THREAD_PRIORITY_LOW)) {
            msg_Err(p_demux, "st2110: failed to spawn worker thread %u", i);
            goto error;
        }
        p_sys->n_workers_started++;
    }

    if (vlc_clone(&p_sys->thread, ReceiveThread, p_demux, VLC_THREAD_PRIORITY_INPUT)) {
        msg_Err(p_demux, "st2110: failed to spawn receive thread");
        goto error;
    }
    p_sys->b_thread_started = true;

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

    /* Stop the producer first so no more work gets enqueued, then let the
       workers drain out and shut down. */
    atomic_store(&p_sys->b_stop_requested, true);
    if (p_sys->b_thread_started) {
        vlc_cancel(p_sys->thread);
        vlc_join(p_sys->thread, NULL);
    }

#ifdef _WIN32
    if (p_sys->iocp) {
        /* Cancel and drain any WSARecv()s still outstanding against our
           buffers before freeing them below -- ReceiveThread has already
           exited by this point (joined above), so nothing will observe
           these completions; this purely ensures the OS is done writing
           into iocp_bufs before free(p_sys) reclaims that memory. */
        if (p_sys->fd != -1)
            CancelIoEx((HANDLE)(uintptr_t)(SOCKET)p_sys->fd, NULL);
        OVERLAPPED_ENTRY drain[IOCP_BUFFER_COUNT];
        ULONG n;
        while (GetQueuedCompletionStatusEx(p_sys->iocp, drain, IOCP_BUFFER_COUNT, &n, 0, FALSE) && n > 0)
            ; /* discard */
        CloseHandle(p_sys->iocp);
        p_sys->iocp = NULL;
    }
#endif

    if (p_sys->b_queues_initialized) {
        for (unsigned i = 0; i < p_sys->n_workers_started; i++) {
            worker_queue_t *q = &p_sys->queues[i];
            vlc_mutex_lock(&q->lock);
            q->b_shutdown = true;
            vlc_cond_broadcast(&q->cond_work);
            vlc_mutex_unlock(&q->lock);
        }
        for (unsigned i = 0; i < p_sys->n_workers_started; i++)
            vlc_join(p_sys->worker_threads[i], NULL);

        for (unsigned i = 0; i < N_WORKERS; i++) {
            worker_queue_t *q = &p_sys->queues[i];
            vlc_mutex_destroy(&q->lock);
            vlc_cond_destroy(&q->cond_work);
            vlc_cond_destroy(&q->cond_avail);
        }
    }

    if (p_sys->fd != -1)
        net_Close(p_sys->fd);

    free(p_sys->psz_source);
    if (p_sys->p_cur_block)
        block_Release(p_sys->p_cur_block);
    free(p_sys->p_line_filled);
    free(p_sys);
    p_demux->p_sys = NULL;
}
