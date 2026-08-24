/*
 * st2110.c - VLC access_demux plugin for SMPTE ST 2110-20 (RFC 4175) reception
 *
 * Receives 10bit YCbCr 4:2:2 GPM video carried per RFC 4175 over RTP/UDP and
 * outputs VLC_CODEC_I422_10L. See docs/vlc-st2110_receive-module_仕様書.md.
 *
 * Architecture (revised after measurement + research showed the previous
 * design's "one CPU core pegged, playback unusable" symptom was NOT a
 * hardware or OS limit, but our own threading pattern):
 *
 *   - The earlier design had N threads *all* calling recv()/WSARecv() on
 *     the same shared socket. This is a well-documented anti-pattern: a
 *     UDP socket's receive queue is guarded by a single kernel-level lock,
 *     so concurrent recv() calls from multiple threads on ONE socket
 *     serialize on that lock and get WORSE, not better, as thread count
 *     grows (matches published research, e.g. "Analysis of Linux UDP
 *     Sockets Concurrent Performance"). This is exactly why the symptom
 *     reproduced identically on both a weak laptop and a 24-core Xeon +
 *     Intel X520: the bottleneck was never raw CPU throughput. Real
 *     ST2110 software confirms this -- GStreamer's rtpvrawdepay (a real,
 *     deployed RFC4175 depayloader) is single-threaded; NVIDIA Rivermax
 *     documents a single, CPU-pinned receive thread per stream, not one
 *     thread per core.
 *   - New design: exactly ONE dedicated receive thread ever touches the
 *     socket. It does nothing but recv() and copy the raw datagram into a
 *     shared, bounded, mutex+condvar-guarded staging queue, then goes
 *     straight back to recv()ing -- kept as fast/simple as possible so it
 *     can never fall behind the wire rate. If the queue is ever full
 *     (workers falling behind), the incoming packet is dropped and
 *     counted rather than blocking the receive thread.
 *   - N worker threads pop raw packets off that queue and do the actual
 *     parse -> unpack -> write work in parallel. This is where the real
 *     per-packet CPU cost lives, and it parallelizes cleanly since RFC4175
 *     packets are fully self-describing (verified against the RFC4175
 *     text itself -- the continuation bit only chains headers WITHIN one
 *     packet, and Offset/Length are always relative to that packet's own
 *     data). No packet needs information from any other packet, and no
 *     "frame"/"field" bookkeeping is needed for correct placement. Writes
 *     into the master buffer never need locking: two different packets'
 *     target byte ranges never overlap.
 *   - Per-worker diagnostic counters (header/failure counts) are one array
 *     slot per worker, written only by that worker and read/reset by the
 *     sender thread every ~5s -- this also retires the previous design's
 *     other latent bug, where every thread incremented the SAME shared
 *     atomic counters on every packet (cache-line contention that gets
 *     worse with more threads, for the same underlying reason as the
 *     socket issue above).
 *   - A single, independent sender thread ticks on its own clock (not
 *     driven by arrival timing at all) and periodically snapshots whatever
 *     is currently in the master buffer to es_out. Packet loss just means
 *     some pixels are one tick stale, not a discarded/blanked frame.
 *   - Windows only: even a single dedicated receive thread using classic
 *     WSARecv()/overlapped I/O topped out around ~138k pkts/sec on real
 *     hardware (an Intel X520 test rig), well short of the wire rate, with
 *     the shortfall showing up as NIC-level ReceivedDiscardedPackets no
 *     NIC/driver tuning (RSS core, checksum offload, interrupt moderation)
 *     could move. A parallel capture of the exact same stream via pktmon
 *     (which taps packets at the NDIS level, below the normal socket
 *     stack) saw almost no loss, pointing at ordinary per-packet
 *     WSARecv()/AFD overhead -- not the NIC -- as the real ceiling. The
 *     receive thread therefore uses Windows Registered I/O (RIO) instead:
 *     a Winsock extension (Windows 8+, no external driver) built exactly
 *     for this -- pre-registered buffers plus a completion queue avoid
 *     most of that per-packet kernel-transition cost. Published RIO
 *     benchmarks reach 180k-450k+ pkts/sec on a single socket, comfortably
 *     past the wall this module was hitting.
 *
 * Targets the VLC 3.0.x plugin ABI (mtime_t/date_t, not the VLC4 vlc_tick_t
 * API).
 */

#ifdef _WIN32
# ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0602  /* Windows 8+: required for Registered I/O (RIO) */
# endif
# ifndef WINVER
#  define WINVER 0x0602
# endif
# include <winsock2.h>
# include <ws2tcpip.h>
# include <mswsock.h>
# define poll(fds, nfds, timeout) WSAPoll((fds), (nfds), (timeout))

/* The MinGW-w64 toolchain used to build this (mingw-w64-x86_64-gcc via
 * MSYS2) does not declare Registered I/O (RIO) in its mswsock.h --
 * verified directly (build failure: RIO_EXTENSION_FUNCTION_TABLE and
 * friends undeclared). RIO is accessed entirely through function
 * pointers obtained at runtime via WSAIoctl(), never linked from an
 * import library, so it's safe to declare the types/constants locally;
 * these are transcribed verbatim from the official Windows SDK's
 * mswsock.h/mswsockdef.h (Windows 8+ ABI, stable since 2012). Guarded so
 * this becomes a no-op if a future MinGW-w64 version adds them itself. */
# ifndef RIO_CORRUPT_CQ
typedef struct RIO_BUFFERID_t *RIO_BUFFERID;
typedef struct RIO_CQ_t       *RIO_CQ;
typedef struct RIO_RQ_t       *RIO_RQ;

#  define RIO_INVALID_BUFFERID ((RIO_BUFFERID)(ULONG_PTR)0xFFFFFFFF)
#  define RIO_INVALID_CQ       ((RIO_CQ)0)
#  define RIO_INVALID_RQ       ((RIO_RQ)0)
#  define RIO_CORRUPT_CQ       0xFFFFFFFF

typedef struct _RIO_BUF {
    RIO_BUFFERID BufferId;
    ULONG        Offset;
    ULONG        Length;
} RIO_BUF, *PRIO_BUF;

typedef struct _RIORESULT {
    LONG      Status;
    ULONG     BytesTransferred;
    ULONGLONG SocketContext;
    ULONGLONG RequestContext;
} RIORESULT, *PRIORESULT;

typedef enum _RIO_NOTIFICATION_COMPLETION_TYPE {
    RIO_EVENT_COMPLETION = 1,
    RIO_IOCP_COMPLETION  = 2,
} RIO_NOTIFICATION_COMPLETION_TYPE;

typedef struct _RIO_NOTIFICATION_COMPLETION {
    RIO_NOTIFICATION_COMPLETION_TYPE Type;
    union {
        struct {
            HANDLE EventHandle;
            BOOL   NotifyReset;
        } Event;
        struct {
            HANDLE IocpHandle;
            PVOID  CompletionKey;
            PVOID  Overlapped;
        } Iocp;
    };
} RIO_NOTIFICATION_COMPLETION, *PRIO_NOTIFICATION_COMPLETION;

typedef BOOL     (WSAAPI *LPFN_RIORECEIVE)(RIO_RQ, PRIO_BUF, ULONG, DWORD, PVOID);
typedef INT      (WSAAPI *LPFN_RIORECEIVEEX)(RIO_RQ, PRIO_BUF, ULONG, PRIO_BUF, PRIO_BUF, PRIO_BUF, PRIO_BUF, DWORD, PVOID);
typedef BOOL     (WSAAPI *LPFN_RIOSEND)(RIO_RQ, PRIO_BUF, ULONG, DWORD, PVOID);
typedef BOOL     (WSAAPI *LPFN_RIOSENDEX)(RIO_RQ, PRIO_BUF, ULONG, PRIO_BUF, PRIO_BUF, PRIO_BUF, PRIO_BUF, DWORD, PVOID);
typedef VOID     (WSAAPI *LPFN_RIOCLOSECOMPLETIONQUEUE)(RIO_CQ);
typedef RIO_CQ   (WSAAPI *LPFN_RIOCREATECOMPLETIONQUEUE)(DWORD, PRIO_NOTIFICATION_COMPLETION);
typedef RIO_RQ   (WSAAPI *LPFN_RIOCREATEREQUESTQUEUE)(SOCKET, ULONG, ULONG, ULONG, ULONG, RIO_CQ, RIO_CQ, PVOID);
typedef ULONG    (WSAAPI *LPFN_RIODEQUEUECOMPLETION)(RIO_CQ, PRIORESULT, ULONG);
typedef VOID     (WSAAPI *LPFN_RIODEREGISTERBUFFER)(RIO_BUFFERID);
typedef INT      (WSAAPI *LPFN_RIONOTIFY)(RIO_CQ);
typedef RIO_BUFFERID (WSAAPI *LPFN_RIOREGISTERBUFFER)(PCHAR, DWORD);
typedef BOOL     (WSAAPI *LPFN_RIORESIZECOMPLETIONQUEUE)(RIO_CQ, DWORD);
typedef BOOL     (WSAAPI *LPFN_RIORESIZEREQUESTQUEUE)(RIO_RQ, DWORD, DWORD);

typedef struct _RIO_EXTENSION_FUNCTION_TABLE {
    DWORD                          cbSize;
    LPFN_RIORECEIVE                RIOReceive;
    LPFN_RIORECEIVEEX              RIOReceiveEx;
    LPFN_RIOSEND                   RIOSend;
    LPFN_RIOSENDEX                 RIOSendEx;
    LPFN_RIOCLOSECOMPLETIONQUEUE   RIOCloseCompletionQueue;
    LPFN_RIOCREATECOMPLETIONQUEUE  RIOCreateCompletionQueue;
    LPFN_RIOCREATEREQUESTQUEUE     RIOCreateRequestQueue;
    LPFN_RIODEQUEUECOMPLETION      RIODequeueCompletion;
    LPFN_RIODEREGISTERBUFFER       RIODeregisterBuffer;
    LPFN_RIONOTIFY                 RIONotify;
    LPFN_RIOREGISTERBUFFER         RIORegisterBuffer;
    LPFN_RIORESIZECOMPLETIONQUEUE  RIOResizeCompletionQueue;
    LPFN_RIORESIZEREQUESTQUEUE     RIOResizeRequestQueue;
} RIO_EXTENSION_FUNCTION_TABLE, *PRIO_EXTENSION_FUNCTION_TABLE;

#  ifndef SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER
   /* _WSAIORW(IOC_WS2,36), spelled out numerically since this MinGW's
      headers don't reliably define all of _WSAIORW/IOC_WS2 either. */
#   define SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER 0xC8000024
#  endif

static const GUID st2110_WSAID_MULTIPLE_RIO = {
    0x8509e081, 0x96dd, 0x4005, { 0xb1, 0x65, 0x9e, 0x2e, 0xe8, 0xc7, 0x9e, 0x3f }
};
#  define WSAID_MULTIPLE_RIO st2110_WSAID_MULTIPLE_RIO
# endif /* !RIO_CORRUPT_CQ */
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

/* Depth of the staging queue between the one receive thread and the N
 * worker threads, in packets. At ~1400 bytes/packet this is ~16MB of
 * buffering -- generous headroom against short worker stalls without the
 * receive thread ever having to block. */
#define QUEUE_CAPACITY        4096

/* Registered I/O (RIO) receive buffer pool: the (single) Windows receive
 * thread pre-posts this many receive buffers up front (see the file
 * header comment for why RIO replaces classic WSARecv()/overlapped I/O
 * here), so this many datagrams can be in flight without the driver ever
 * having to wait on the application. */
#define RIO_BUF_COUNT        4096
#define RIO_CQ_SIZE           RIO_BUF_COUNT
#define RIO_MAX_RESULTS        256   /* batch size when draining completions */

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

/* One slot in the shared staging queue: a raw, not-yet-parsed copy of one
 * received datagram. */
typedef struct {
    uint8_t data[RECV_BUF_LEN];
    size_t  len;
} queue_slot_t;

/* Per-worker diagnostic counters: written only by the one worker thread
 * that owns this slot (see the file header comment for why this replaces
 * the old shared-atomic-counter design), read + reset by the sender
 * thread every ~5s. Since only the owning thread ever increments a given
 * slot, these atomic ops are always uncontended in steady state -- the
 * occasional cross-thread read by the sender is rare enough (every ~5s)
 * for the resulting cache-line transfer to be negligible. */
typedef struct {
    atomic_uint pkts_rtp_fail;
    atomic_uint pkts_line_fail;
    atomic_uint hdrs_total;
    atomic_uint hdrs_field2;
    /* Headers that parsed fine but were then silently skipped inside
       WriteLines() -- previously uncounted, so there was no way to tell
       whether stale-looking picture regions were from real packet loss
       (which pkts/line_fail above already show is ~0) or from our own
       validity checks rejecting geometry that should have been good. */
    atomic_uint hdrs_bad_geometry;  /* length/offset/width sanity checks */
    atomic_uint hdrs_out_of_range;  /* MapLine() rejected or out_line >= height */
} worker_stats_t;

typedef struct {
    demux_t *p_demux;
    unsigned worker_index;
} worker_arg_t;

struct demux_sys_t
{
    /* network */
    int      fd;
    char     psz_group[256];
    int      i_port;
    char    *psz_source;

#ifdef _WIN32
    /* Registered I/O (RIO) state: replaces classic WSARecv()/overlapped
       I/O for the receive thread (see the file header comment for why).
       The socket itself is created manually with WSA_FLAG_REGISTERED_IO
       instead of via net_OpenDgram(), since RIO requires that flag at
       socket-creation time. */
    RIO_EXTENSION_FUNCTION_TABLE rio;
    uint8_t      *p_rio_buf;      /* one big buffer, registered with RIO */
    RIO_BUFFERID  rio_buf_id;
    RIO_CQ        rio_cq;
    RIO_RQ        rio_rq;
    HANDLE        rio_event;
    bool          b_rio_inited;
#endif

    /* format */
    unsigned i_width;
    unsigned i_height;
    unsigned i_depth;
    bool     b_interlace;
    unsigned i_fps_num;
    unsigned i_fps_den;

    /* line-number convention, detected from the first few packets by
       whichever worker thread happens to see them first. Racy but
       harmless: redundant writes of the same correct value from multiple
       threads are fine, and DetectLineMode() only needs the SDP-declared
       cases to be told apart, not exact packet ordering. Only touched
       during the brief startup detection window, never again afterward. */
    atomic_uint line_mode;              /* line_mode_t */
    atomic_uint i_pkts_for_line_detect;  /* cumulative, never reset */

    /* elementary stream (touched only by the sender thread) */
    es_out_id_t *p_es;
    date_t       pts;

    /* Double-buffered picture buffer: two full frame buffers, each
       allocated and zeroed once and never cleared again. Workers always
       write into whichever one active_write_idx currently names; the
       worker whose decrement brings n_in_flight to zero (see
       marker_pending above) flips active_write_idx to the other buffer
       in the same breath as recording which one just became
       completed_buf_idx, before signaling the sender. From that instant
       no worker will ever write into completed_buf_idx again until a
       full frame period later, so the sender can copy it out at its own
       pace with no possibility of reading a buffer that's concurrently
       being written -- unlike a single shared buffer, where the sender's
       copy could in principle still land mid-write for whatever the
       *next* frame's earliest packets touch first. Within one buffer,
       no lock is needed for the writes themselves: RFC4175 guarantees
       two different packets never target overlapping byte ranges (see
       the file header comment), so concurrent writers into the same
       buffer never race with each other either. */
    uint8_t     *p_master_buf[2];
    atomic_uint  active_write_idx;   /* which buffer workers write into */
    atomic_uint  completed_buf_idx;  /* which buffer the sender should read */
    size_t   i_buf_size;
    size_t   i_y_plane_size;
    size_t   i_uv_plane_size;

    /* Receive thread: exactly one. It is the ONLY thread that ever
       touches the socket (see the file header comment for why sharing a
       socket across multiple recv()ing threads is an anti-pattern, not a
       parallelism win). Its only job is recv() + copy into p_queue below,
       as fast as possible -- no parsing happens here. */
    vlc_thread_t receive_thread;
    bool         b_receive_started;

    /* Shared staging queue between the one receive thread (single
       producer) and the N worker threads below (multiple consumers).
       Bounded: if it's ever full, the receive thread drops the incoming
       packet (counted in i_pkts_dropped_queue_full) rather than blocking,
       since a blocked receive thread is worse than one dropped packet. */
    queue_slot_t *p_queue;
    unsigned      i_queue_cap;
    unsigned      i_queue_head;
    unsigned      i_queue_tail;
    unsigned      i_queue_count;
    vlc_mutex_t   queue_lock;
    bool          b_queue_lock_inited;
#ifdef _WIN32
    /* Windows: a semaphore instead of vlc_cond_t (see QueuePush()/
       QueuePop() for why -- vlc_cond_signal() was found, via a captured
       thread stack, to wake every waiter instead of just one). */
    HANDLE        queue_sem;
#else
    vlc_cond_t    queue_not_empty;
    bool          b_queue_cond_inited;
#endif
    atomic_uint   i_pkts_dropped_queue_full;

    /* Worker threads: N of these pop raw packets off p_queue and run the
       full parse -> unpack -> write pipeline in parallel. */
    unsigned        n_threads;
    vlc_thread_t   *worker_threads;
    worker_arg_t   *p_worker_args;
    worker_stats_t *p_worker_stats;
    unsigned        n_workers_started;

    /* sender thread: paced by SignalFrameComplete() below, not an
       independent clock (see SenderThread() for why) */
    vlc_thread_t sender_thread;
    bool         b_sender_started;

    /* Frame-complete signal from whichever worker processes the packet
       carrying RFC4175's "last packet of this field/frame" marker bit
       (see SignalFrameComplete()/ProcessPacket()). Only ever one waiter
       (the sender thread), so this plain condvar is not subject to the
       "wakes every waiter" issue documented on queue_sem above. */
    vlc_mutex_t  frame_lock;
    vlc_cond_t   frame_cond;
    bool         b_frame_lock_inited;
    bool         b_frame_cond_inited;
    bool         b_frame_ready;   /* protected by frame_lock */
    /* Diagnostic: how many times SignalFrameComplete() actually fired,
       so the marker-bit assumption above can be checked against reality
       instead of just trusted -- read/reset by the sender thread every
       ~5s, alongside its other stats. */
    atomic_uint  i_frame_signals;

    /* Completion barrier for the marker-bit signal above: with N worker
       threads all popping from one FIFO queue but finishing in whatever
       order they finish (not necessarily the order they popped in),
       "the worker that processed the marker packet is done" does NOT by
       itself mean every earlier-queued packet has also finished being
       written -- a slower worker could still be mid-write on one. That
       gap caused visible block-corruption, not just tearing, when first
       tried without this. n_in_flight counts packets that have been
       popped but not yet fully processed (see QueuePop()/WorkerThread());
       marker_pending is set when a worker sees the completing marker.
       Whichever worker's decrement is the one that brings n_in_flight to
       zero -- i.e. genuinely the last packet queued up to that point to
       finish, regardless of which thread it happened to run on --
       atomically claims marker_pending and fires the signal. This keeps
       full N-way parallelism in the unpack stage (needed for weaker
       hardware than this dev machine) while still being race-free. */
    atomic_uint  n_in_flight;
    atomic_bool  marker_pending;

    /* Cooperative shutdown signal: a blocked WaitForMultipleObjects()/
       poll()/vlc_cond_timedwait() is not guaranteed to be interrupted by
       vlc_cancel() on every platform, so every wait loop here also checks
       this flag on its own bounded timeout, guaranteeing Close()'s
       vlc_join() calls can never hang. */
    atomic_bool  b_stop_requested;

    /* rolling diagnostic: total packets actually received off the wire.
       Written only by the single receive thread, so this atomic is never
       contended; read and reset by the sender thread every ~5s. */
    atomic_uint i_pkts_total;

    /* Wire-level loss, detected from gaps in the RTP sequence number
       (see TrackSequenceLoss()) -- the same check done manually with an
       external capture earlier in this project's life, now built in and
       always on. i_last_rtp_seq is touched only by the single receive
       thread (no synchronization needed); -1 means "haven't seen a
       first packet yet". i_pkts_seq_lost is read/reset by the sender
       thread every ~5s like the other counters. */
    int          i_last_rtp_seq;
    atomic_uint  i_pkts_seq_lost;
};

static int  Open(vlc_object_t *);
static void Close(vlc_object_t *);
static int  Demux(demux_t *);
static int  Control(demux_t *, int, va_list);
static void *ReceiveThread(void *);
static void *WorkerThread(void *);
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
    add_integer("st2110-threads", 0, "Processing threads (0 = auto-detect from CPU count)", NULL, true)
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
 * present) and returns the byte offset where the RTP payload begins, plus
 * the marker bit (RFC4175: set on the last packet of each field's, or for
 * progressive the frame's, transmission -- used to know when a complete,
 * non-torn frame has just landed in the master buffer). */
static int ParseRTP(const uint8_t *p, size_t len, unsigned *hdr_len, bool *out_marker)
{
    if (len < RTP_HDR_MIN_LEN)
        return -1;
    if ((p[0] >> 6) != 2)          /* RTP version must be 2 */
        return -1;

    *out_marker = (p[1] & 0x80) != 0;

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
 * directly into buf_idx's Y/U/V planes (see the demux_sys_t comment on
 * p_master_buf for why the caller picks the buffer once up front rather
 * than this function re-reading active_write_idx itself). Called by
 * whichever worker thread dequeued this packet -- no lock: this packet's
 * target byte ranges never overlap another packet's (different line, or a
 * non-overlapping segment of the same line), so concurrent calls from
 * different worker threads writing into the same buffer are safe. */
static void WriteLines(demux_sys_t *p_sys, worker_stats_t *stats, unsigned buf_idx,
                        const rfc4175_line_hdr_t *hdrs, unsigned n_hdrs,
                        const uint8_t *data, size_t data_len)
{
    size_t off = 0;
    unsigned half_w = p_sys->i_width / 2;

    uint8_t *buf = p_sys->p_master_buf[buf_idx];
    uint16_t *y_plane = (uint16_t *)buf;
    uint16_t *u_plane = (uint16_t *)(buf + p_sys->i_y_plane_size);
    uint16_t *v_plane = (uint16_t *)(buf + p_sys->i_y_plane_size
                                          + p_sys->i_uv_plane_size);

    for (unsigned i = 0; i < n_hdrs; i++) {
        const rfc4175_line_hdr_t *h = &hdrs[i];

        atomic_fetch_add(&stats->hdrs_total, 1);
        if (h->field2)
            atomic_fetch_add(&stats->hdrs_field2, 1);

        if (off + h->length > data_len)
            break; /* the rest of the chain can't be trusted either */

        if (h->field2 && !p_sys->b_interlace) {
            off += h->length;
            continue;
        }
        if (h->length % 5 != 0 || h->offset % 2 != 0 || h->offset >= p_sys->i_width) {
            atomic_fetch_add(&stats->hdrs_bad_geometry, 1);
            off += h->length;
            continue;
        }

        unsigned out_line;
        if (!MapLine(p_sys, h, &out_line) || out_line >= p_sys->i_height) {
            atomic_fetch_add(&stats->hdrs_out_of_range, 1);
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

/* Wakes the sender thread (see SenderThread()) to tell it a complete,
 * non-torn frame just finished landing in the master buffer. There is
 * only ever one waiter (the sender thread), so -- unlike QueuePush's
 * situation with many worker threads (see the file header comment) --
 * a plain vlc_cond_signal() here can't cause a thundering herd: there's
 * only one thread it could possibly wake. */
static void SignalFrameComplete(demux_sys_t *p_sys)
{
    atomic_fetch_add(&p_sys->i_frame_signals, 1);
    vlc_mutex_lock(&p_sys->frame_lock);
    p_sys->b_frame_ready = true;
    vlc_mutex_unlock(&p_sys->frame_lock);
    vlc_cond_signal(&p_sys->frame_cond);
}

/* Parses one packet and writes its pixels straight into the master buffer.
 * Called by a worker thread with its own per-worker stats slot (see the
 * file header comment for why these are per-worker, not shared). No
 * frame/field bookkeeping is needed for CORRECT PLACEMENT of pixels --
 * each packet is fully self-describing (see file header) -- but the RTP
 * marker bit is still tracked for a different reason: knowing when a
 * complete frame is ready to be *sent*, so the sender thread doesn't have
 * to guess via an independent clock (see SenderThread()). */
static void ProcessPacket(demux_sys_t *p_sys, worker_stats_t *stats,
                           const uint8_t *pkt, size_t len)
{
    unsigned hdr_len;
    bool marker;
    if (ParseRTP(pkt, len, &hdr_len, &marker) != 0) {
        atomic_fetch_add(&stats->pkts_rtp_fail, 1);
        return;
    }

    const uint8_t *payload = pkt + hdr_len;
    size_t payload_len = len - hdr_len;
    if (payload_len < RFC4175_EXT_SEQ_LEN) {
        atomic_fetch_add(&stats->pkts_line_fail, 1);
        return;
    }
    payload += RFC4175_EXT_SEQ_LEN;
    payload_len -= RFC4175_EXT_SEQ_LEN;

    rfc4175_line_hdr_t hdrs[MAX_LINE_SEGMENTS];
    unsigned n_hdrs;
    size_t hdr_bytes;
    if (ParseLineHeaders(payload, payload_len, hdrs, MAX_LINE_SEGMENTS,
                          &n_hdrs, &hdr_bytes) != 0) {
        atomic_fetch_add(&stats->pkts_line_fail, 1);
        return;
    }

    DetectLineMode(p_sys, hdrs, n_hdrs);
    unsigned buf_idx = atomic_load(&p_sys->active_write_idx);
    WriteLines(p_sys, stats, buf_idx, hdrs, n_hdrs, payload + hdr_bytes, payload_len - hdr_bytes);

    /* For interlace, only field two's marker completes a full woven
       frame -- field one's marker just means "half done, field two still
       to come". hdrs[0] stands in for the whole packet's field since a
       single packet's chained SRD headers are always for one field.
       This only marks the intent to signal; WorkerThread() is what
       actually fires it, once every packet queued up to this point has
       genuinely finished being written (see the n_in_flight/
       marker_pending comment on demux_sys_t for why that distinction
       matters with N parallel workers). */
    if (marker && (!p_sys->b_interlace || hdrs[0].field2))
        atomic_store(&p_sys->marker_pending, true);
}

/* Cheap peek at the RTP sequence number (header bytes 2-3) to detect
 * genuine wire-level loss -- gaps in the sequence -- independent of and
 * before any full header/line parsing. Called for every datagram the
 * receive thread gets, regardless of whether it later parses cleanly, so
 * this measures what actually arrived on the wire, not just what our own
 * parser accepted. Only ever called from the single receive thread (see
 * the file header comment for why that's exactly one thread), so
 * i_last_rtp_seq needs no locking or atomics despite being read-then-
 * written here every packet. */
static void TrackSequenceLoss(demux_sys_t *p_sys, const uint8_t *data, size_t len)
{
    if (len < 4)
        return;

    uint16_t seq = ((uint16_t)data[2] << 8) | data[3];
    if (p_sys->i_last_rtp_seq >= 0) {
        uint16_t expected = (uint16_t)(p_sys->i_last_rtp_seq + 1);
        int16_t delta = (int16_t)(seq - expected);
        /* A positive delta means `delta` sequence numbers were skipped --
           real loss. A negative delta means this packet arrived out of
           order or is a duplicate, which isn't loss (whatever it's a
           duplicate/reorder of already got counted, or will). */
        if (delta > 0)
            atomic_fetch_add(&p_sys->i_pkts_seq_lost, (unsigned)delta);
    }
    p_sys->i_last_rtp_seq = seq;
}

/* Pushes a raw, not-yet-parsed datagram into the shared staging queue.
 * Called only by the one receive thread (single producer); the queue's
 * head/tail/count state is still shared with the N consumer worker
 * threads, so it's protected by a plain mutex -- cheap relative to the
 * per-packet parse/unpack work done outside the lock on the consumer
 * side. If the queue is full, the packet is dropped rather than blocking
 * the receive thread, which must never fall behind recv()ing. */
static void QueuePush(demux_sys_t *p_sys, const uint8_t *data, size_t len)
{
    vlc_mutex_lock(&p_sys->queue_lock);
    if (p_sys->i_queue_count == p_sys->i_queue_cap) {
        vlc_mutex_unlock(&p_sys->queue_lock);
        atomic_fetch_add(&p_sys->i_pkts_dropped_queue_full, 1);
        return;
    }

    queue_slot_t *slot = &p_sys->p_queue[p_sys->i_queue_tail];
    memcpy(slot->data, data, len);
    slot->len = len;
    p_sys->i_queue_tail = (p_sys->i_queue_tail + 1) % p_sys->i_queue_cap;
    p_sys->i_queue_count++;
    vlc_mutex_unlock(&p_sys->queue_lock);

#ifdef _WIN32
    /* Not vlc_cond_signal(): live profiling (Process Explorer, a captured
       thread stack) caught this call chain pegging a core --
       QueuePush -> vlc_cond_signal -> ... -> RtlWakeAllConditionVariable.
       On this VLC/Windows build, vlc_cond_signal() wakes *every* waiter,
       not just one -- with N worker threads blocked in QueuePop below,
       every single packet arrival was waking all of them, most of which
       just found the queue empty (one item, many waiters) and went back
       to sleep. That's an O(N) kernel-mode thundering herd on every one
       of a few hundred thousand packets per second. A semaphore doesn't
       have this problem: ReleaseSemaphore(sem, 1, ...) is guaranteed by
       the OS to wake at most one waiter. POSIX is unaffected --
       pthread_cond_signal there really does wake only one -- so it still
       uses the plain vlc_cond_t path below. */
    ReleaseSemaphore(p_sys->queue_sem, 1, NULL);
#else
    vlc_cond_signal(&p_sys->queue_not_empty);
#endif
}

/* Pops one raw datagram for a worker thread to process. Returns false only
 * once shutdown has been requested AND the queue has fully drained -- any
 * packets still queued at shutdown are processed first, so nothing queued
 * is silently discarded. The wait is bounded (500ms) purely so the stop
 * flag gets rechecked even if a signal/release is ever missed; it is not
 * the primary wakeup path (QueuePush's own signal/release is). */
#ifdef _WIN32
static bool QueuePop(demux_sys_t *p_sys, uint8_t *out_data, size_t *out_len)
{
    for (;;) {
        DWORD w = WaitForSingleObject(p_sys->queue_sem, 500);
        if (w == WAIT_OBJECT_0)
            break;
        if (atomic_load(&p_sys->b_stop_requested))
            return false;
        /* WAIT_TIMEOUT: recheck the stop flag and keep waiting. Any other
           result means the semaphore itself is gone -- nothing more this
           thread can do. */
        if (w != WAIT_TIMEOUT)
            return false;
    }

    /* The semaphore count exactly mirrors i_queue_count increments from
       QueuePush, so a successful wait here guarantees an item is
       present -- no need to loop/recheck under the lock. */
    vlc_mutex_lock(&p_sys->queue_lock);
    queue_slot_t *slot = &p_sys->p_queue[p_sys->i_queue_head];
    memcpy(out_data, slot->data, slot->len);
    *out_len = slot->len;
    p_sys->i_queue_head = (p_sys->i_queue_head + 1) % p_sys->i_queue_cap;
    p_sys->i_queue_count--;
    vlc_mutex_unlock(&p_sys->queue_lock);
    return true;
}
#else
static bool QueuePop(demux_sys_t *p_sys, uint8_t *out_data, size_t *out_len)
{
    vlc_mutex_lock(&p_sys->queue_lock);
    while (p_sys->i_queue_count == 0) {
        if (atomic_load(&p_sys->b_stop_requested)) {
            vlc_mutex_unlock(&p_sys->queue_lock);
            return false;
        }
        vlc_cond_timedwait(&p_sys->queue_not_empty, &p_sys->queue_lock,
                            mdate() + 500000 /* 500ms */);
    }

    queue_slot_t *slot = &p_sys->p_queue[p_sys->i_queue_head];
    memcpy(out_data, slot->data, slot->len);
    *out_len = slot->len;
    p_sys->i_queue_head = (p_sys->i_queue_head + 1) % p_sys->i_queue_cap;
    p_sys->i_queue_count--;
    vlc_mutex_unlock(&p_sys->queue_lock);
    return true;
}
#endif

#ifdef _WIN32
/* Retrieves the RIO extension function table for a RIO-capable socket
 * (one created with WSA_FLAG_REGISTERED_IO). RIO isn't exposed as a
 * normal linkable API -- it must be queried per-socket via WSAIoctl. */
static bool GetRioFunctionTable(SOCKET s, RIO_EXTENSION_FUNCTION_TABLE *out)
{
    GUID rio_guid = WSAID_MULTIPLE_RIO;
    DWORD bytes = 0;
    int r = WSAIoctl(s, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER,
                      &rio_guid, sizeof(rio_guid),
                      out, sizeof(*out), &bytes, NULL, NULL);
    return r == 0;
}

/* Creates the RIO-capable receive socket and joins the multicast group,
 * replacing net_OpenDgram() on Windows (see the file header comment for
 * why): RIO requires the socket to be created with WSA_FLAG_REGISTERED_IO
 * up front, which VLC's own net_OpenDgram() has no way to request. This
 * duplicates net_OpenDgram()'s bind + (source-specific, if st2110-source
 * is set) multicast join, IPv4 only. */
static int OpenRioSocket(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    /* Sentinels matching what Close() checks each resource against, so
       that whichever step below fails, Close()'s existing RIO cleanup
       block (gated on b_rio_inited, set just below once the socket
       exists) can safely no-op on every resource this call never got to
       allocate. */
    p_sys->rio_buf_id = RIO_INVALID_BUFFERID;
    p_sys->rio_cq = RIO_INVALID_CQ;
    p_sys->rio_rq = RIO_INVALID_RQ;

    struct in_addr group_addr, source_addr;
    if (InetPtonA(AF_INET, p_sys->psz_group, &group_addr) != 1) {
        msg_Err(p_demux, "st2110: invalid group address \"%s\"", p_sys->psz_group);
        return VLC_EGENERIC;
    }
    bool b_ssm = p_sys->psz_source && *p_sys->psz_source;
    if (b_ssm && InetPtonA(AF_INET, p_sys->psz_source, &source_addr) != 1) {
        msg_Err(p_demux, "st2110: invalid source address \"%s\"", p_sys->psz_source);
        return VLC_EGENERIC;
    }

    SOCKET s = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0,
                           WSA_FLAG_OVERLAPPED | WSA_FLAG_REGISTERED_IO);
    if (s == INVALID_SOCKET) {
        msg_Err(p_demux, "st2110: WSASocket failed (err=%d)", WSAGetLastError());
        return VLC_EGENERIC;
    }
    /* From here on, p_sys->fd owns this socket and b_rio_inited marks
       that Close() is responsible for tearing down whatever RIO state
       got created below -- so no branch below needs its own
       closesocket(), avoiding any risk of double-closing it. */
    p_sys->fd = (int)s;
    p_sys->b_rio_inited = true;

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    int rcvbuf = SO_RCVBUF_SIZE;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)p_sys->i_port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        msg_Err(p_demux, "st2110: bind failed (err=%d)", WSAGetLastError());
        return VLC_EGENERIC;
    }

    if (b_ssm) {
        struct ip_mreq_source mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr = group_addr;
        mreq.imr_sourceaddr = source_addr;
        mreq.imr_interface.s_addr = INADDR_ANY;
        if (setsockopt(s, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP,
                        (const char *)&mreq, sizeof(mreq)) == SOCKET_ERROR) {
            msg_Err(p_demux, "st2110: IP_ADD_SOURCE_MEMBERSHIP failed (err=%d)", WSAGetLastError());
            return VLC_EGENERIC;
        }
    } else {
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr = group_addr;
        mreq.imr_interface.s_addr = INADDR_ANY;
        if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                        (const char *)&mreq, sizeof(mreq)) == SOCKET_ERROR) {
            msg_Err(p_demux, "st2110: IP_ADD_MEMBERSHIP failed (err=%d)", WSAGetLastError());
            return VLC_EGENERIC;
        }
    }

    if (!GetRioFunctionTable(s, &p_sys->rio)) {
        msg_Err(p_demux, "st2110: failed to get RIO function table (err=%d)", WSAGetLastError());
        return VLC_EGENERIC;
    }

    size_t total_size = (size_t)RIO_BUF_COUNT * RECV_BUF_LEN;
    p_sys->p_rio_buf = VirtualAlloc(NULL, total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p_sys->p_rio_buf) {
        msg_Err(p_demux, "st2110: VirtualAlloc failed for RIO buffer");
        return VLC_EGENERIC;
    }

    p_sys->rio_buf_id = p_sys->rio.RIORegisterBuffer((PCHAR)p_sys->p_rio_buf, (DWORD)total_size);
    if (p_sys->rio_buf_id == RIO_INVALID_BUFFERID) {
        msg_Err(p_demux, "st2110: RIORegisterBuffer failed (err=%d)", WSAGetLastError());
        return VLC_EGENERIC;
    }

    p_sys->rio_event = CreateEvent(NULL, TRUE /* manual reset */, FALSE, NULL);
    if (!p_sys->rio_event) {
        msg_Err(p_demux, "st2110: CreateEvent failed for RIO");
        return VLC_EGENERIC;
    }

    RIO_NOTIFICATION_COMPLETION notify;
    memset(&notify, 0, sizeof(notify));
    notify.Type = RIO_EVENT_COMPLETION;
    notify.Event.EventHandle = p_sys->rio_event;
    notify.Event.NotifyReset = TRUE;

    p_sys->rio_cq = p_sys->rio.RIOCreateCompletionQueue(RIO_CQ_SIZE, &notify);
    if (p_sys->rio_cq == RIO_INVALID_CQ) {
        msg_Err(p_demux, "st2110: RIOCreateCompletionQueue failed (err=%d)", WSAGetLastError());
        return VLC_EGENERIC;
    }

    p_sys->rio_rq = p_sys->rio.RIOCreateRequestQueue(s, RIO_BUF_COUNT, 1, 0, 1,
                                                       p_sys->rio_cq, p_sys->rio_cq, NULL);
    if (p_sys->rio_rq == RIO_INVALID_RQ) {
        msg_Err(p_demux, "st2110: RIOCreateRequestQueue failed (err=%d)", WSAGetLastError());
        return VLC_EGENERIC;
    }

    for (ULONG i = 0; i < RIO_BUF_COUNT; i++) {
        RIO_BUF buf;
        buf.BufferId = p_sys->rio_buf_id;
        buf.Offset = i * RECV_BUF_LEN;
        buf.Length = RECV_BUF_LEN;
        if (!p_sys->rio.RIOReceive(p_sys->rio_rq, &buf, 1, 0, (PVOID)(uintptr_t)i))
            msg_Warn(p_demux, "st2110: RIOReceive failed to post buffer %lu (err=%d)", i, WSAGetLastError());
    }

    return VLC_SUCCESS;
}

/* Windows receive thread: exactly one of these ever exists (see the file
 * header comment). Uses Registered I/O (RIO) instead of classic
 * WSARecv()/overlapped I/O -- all RIO_BUF_COUNT receive buffers were
 * pre-posted once by OpenRioSocket() before this thread started; this
 * loop only waits for completions, copies each completed datagram into
 * the staging queue, and reposts that same buffer slot immediately. No
 * parsing happens on this thread.
 *
 * The wait is bounded (1000ms), so this thread also notices
 * b_stop_requested promptly on shutdown without needing anything like the
 * old CancelIoEx() call. */
static void *ReceiveThread(void *data)
{
    demux_t *p_demux = data;
    demux_sys_t *p_sys = p_demux->p_sys;
    RIORESULT results[RIO_MAX_RESULTS];

    /* Spin guard: if RIONotify()+WaitForSingleObject() ever reports the
       event signaled with genuinely nothing to dequeue, several times in
       a row, something is wrong with the notify/reset handshake (e.g. a
       spurious re-signal) and this loop would otherwise spin at 100% CPU
       calling nothing but Win32 APIs -- exactly the kind of runaway that
       destabilized a whole test machine (including its RDP session) once
       already. This forces a real sleep once that pattern is detected,
       trading a little latency for never being able to peg a core doing
       nothing. */
    unsigned n_consecutive_empty = 0;

    for (;;) {
        if (atomic_load(&p_sys->b_stop_requested))
            break;

        p_sys->rio.RIONotify(p_sys->rio_cq);

        DWORD w = WaitForSingleObject(p_sys->rio_event, 1000);
        if (w == WAIT_TIMEOUT)
            continue;
        if (w != WAIT_OBJECT_0)
            break; /* WAIT_FAILED or abandoned: nothing more we can do here */

        int canc = vlc_savecancel();

        ULONG n_total = 0;
        for (;;) {
            ULONG n = p_sys->rio.RIODequeueCompletion(p_sys->rio_cq, results, RIO_MAX_RESULTS);
            if (n == 0 || n == RIO_CORRUPT_CQ)
                break;
            n_total += n;

            for (ULONG i = 0; i < n; i++) {
                ULONG slot = (ULONG)(uintptr_t)results[i].RequestContext;

                if (results[i].BytesTransferred > 0) {
                    atomic_fetch_add(&p_sys->i_pkts_total, 1);
                    TrackSequenceLoss(p_sys, p_sys->p_rio_buf + (size_t)slot * RECV_BUF_LEN,
                                       results[i].BytesTransferred);
                    QueuePush(p_sys, p_sys->p_rio_buf + (size_t)slot * RECV_BUF_LEN,
                              results[i].BytesTransferred);
                }

                if (!atomic_load(&p_sys->b_stop_requested)) {
                    RIO_BUF buf;
                    buf.BufferId = p_sys->rio_buf_id;
                    buf.Offset = slot * RECV_BUF_LEN;
                    buf.Length = RECV_BUF_LEN;
                    p_sys->rio.RIOReceive(p_sys->rio_rq, &buf, 1, 0, (PVOID)(uintptr_t)slot);
                }
            }

            if (n < RIO_MAX_RESULTS)
                break;
        }

        vlc_restorecancel(canc);

        if (n_total == 0) {
            if (++n_consecutive_empty >= 64) {
                if (n_consecutive_empty == 64)
                    msg_Warn(p_demux, "st2110: receive thread woke with nothing to "
                             "dequeue 64 times in a row -- throttling to avoid a busy spin");
                Sleep(5);
            }
        } else {
            n_consecutive_empty = 0;
        }
    }

    return NULL;
}
#else
/* POSIX receive thread: exactly one of these ever exists (see the file
 * header comment). poll() here resolves to vlc_poll() (see vlc_threads.h),
 * VLC's cancellation-aware wrapper -- raw select() has no such wrapper and
 * would make Close()'s vlc_join() hang forever. Its only job is to drain
 * the socket and copy each datagram into the staging queue -- no parsing
 * happens on this thread. */
static void *ReceiveThread(void *data)
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
            TrackSequenceLoss(p_sys, pkt, (size_t)len);
            QueuePush(p_sys, pkt, (size_t)len);
        }

        vlc_restorecancel(canc);
    }

    return NULL;
}
#endif

/* Worker thread: N of these pop raw packets off the staging queue and run
 * the full parse -> unpack -> write pipeline in parallel, entirely
 * decoupled from the socket (see the file header comment). Platform-
 * independent, since it never touches the network itself. */
static void *WorkerThread(void *data)
{
    worker_arg_t *arg = data;
    demux_t     *p_demux = arg->p_demux;
    demux_sys_t *p_sys   = p_demux->p_sys;
    worker_stats_t *stats = &p_sys->p_worker_stats[arg->worker_index];
    uint8_t pkt[RECV_BUF_LEN];

    for (;;) {
        size_t len;
        if (!QueuePop(p_sys, pkt, &len))
            break;
        atomic_fetch_add(&p_sys->n_in_flight, 1);

        int canc = vlc_savecancel();
        ProcessPacket(p_sys, stats, pkt, len);

        /* atomic_fetch_sub() returns the pre-decrement value, so == 1
           means this decrement is the one that brought the count to
           zero -- i.e. every packet popped up to and including this
           moment has now genuinely finished being written, regardless
           of which worker thread did the writing. Only then is it safe
           to check/claim marker_pending (see the demux_sys_t comment for
           why the two need to be checked together, not independently). */
        if (atomic_fetch_sub(&p_sys->n_in_flight, 1) == 1) {
            bool expected = true;
            if (atomic_compare_exchange_strong(&p_sys->marker_pending, &expected, false)) {
                /* Flip which buffer workers write into *before* telling
                   the sender which one just became safe to read (see the
                   demux_sys_t comment on p_master_buf). n_in_flight is
                   genuinely zero right now -- no other worker is mid-
                   write -- so nothing can still be writing into the
                   buffer we're about to hand off. */
                unsigned old_idx = atomic_load(&p_sys->active_write_idx);
                atomic_store(&p_sys->completed_buf_idx, old_idx);
                atomic_store(&p_sys->active_write_idx, old_idx ^ 1);
                SignalFrameComplete(p_sys);
            }
        }

        vlc_restorecancel(canc);
    }

    return NULL;
}

/* Sender thread: paced by whichever worker calls SignalFrameComplete()
 * (see ProcessPacket()) when a complete, non-torn frame just finished
 * landing in the master buffer, rather than ticking on an independent
 * clock. The earlier independent-clock design had no idea whether a
 * write was mid-flight when it snapshotted, so it could (and, per a
 * visible horizontal-tearing artifact, did) grab a frame that was half
 * this field and half the previous one. Waiting for the real field/frame
 * boundary instead eliminates that at the source. The wait is still
 * bounded (one frame interval -- see the note below on why not more) as
 * a fallback for a missed marker or a stalled/lost stream, so this
 * thread can't wedge; packet loss still just means some pixels are one
 * tick stale, never a discarded or blanked frame. Also owns the periodic
 * (~5s) diagnostic log. */
static void *SenderThread(void *data)
{
    demux_t *p_demux = data;
    demux_sys_t *p_sys = p_demux->p_sys;
    mtime_t frame_interval = (mtime_t)CLOCK_FREQ * p_sys->i_fps_den / p_sys->i_fps_num;
    mtime_t stat_window_start = mdate();

    for (;;) {
        if (atomic_load(&p_sys->b_stop_requested))
            break;

        vlc_mutex_lock(&p_sys->frame_lock);
        if (!p_sys->b_frame_ready)
            /* Real-world senders don't all set the RTP marker bit on
               every single frame (confirmed via the frame_signals log
               below landing at ~75-80% of the expected count, not 0% or
               100%) -- so this fallback isn't a rare safety net, it's a
               regular occurrence. Bounding it to exactly one frame
               interval, not more, matters: anything longer adds that
               much extra latency on every frame the marker misses. */
            vlc_cond_timedwait(&p_sys->frame_cond, &p_sys->frame_lock,
                                mdate() + frame_interval);
        p_sys->b_frame_ready = false;
        vlc_mutex_unlock(&p_sys->frame_lock);

        if (atomic_load(&p_sys->b_stop_requested))
            break;

        block_t *p_block = block_Alloc(p_sys->i_buf_size);
        if (p_block) {
            unsigned buf_idx = atomic_load(&p_sys->completed_buf_idx);
            memcpy(p_block->p_buffer, p_sys->p_master_buf[buf_idx], p_sys->i_buf_size);
            p_block->i_dts = p_block->i_pts = date_Get(&p_sys->pts);
            /* WriteLines() already weaves both fields into one frame (see
               MapLine()), but without this flag VLC's own deinterlace
               filter has no way to know that -- it can only guess from
               the format, guesses wrong, and leaves the raw field comb
               visible as horizontal striping. Top-field-first matches
               virtually all real-world 1080i broadcast sources; RFC4175
               itself carries no TFF/BFF signal to check against. */
            if (p_sys->b_interlace)
                p_block->i_flags |= BLOCK_FLAG_TOP_FIELD_FIRST;
            es_out_Control(p_demux->out, ES_OUT_SET_PCR, p_block->i_pts);
            es_out_Send(p_demux->out, p_sys->p_es, p_block);
        }
        date_Increment(&p_sys->pts, 1);

#if 0   /* TEMPORARILY DISABLED: user asked for a "pure" build with no
           diagnostic logging, to check whether it's a factor in a
           reported gradual slowdown (possibly VLC's own message
           console accumulating text over a long session, not this
           module's own processing cost -- the atomic_exchange() calls
           below are cheap regardless of whether their results get
           logged). Re-enable by flipping this to `#if 1` once that's
           been isolated. */
        if (mdate() - stat_window_start > 5 * CLOCK_FREQ) {
            unsigned pkts     = atomic_exchange(&p_sys->i_pkts_total, 0);
            unsigned dropped  = atomic_exchange(&p_sys->i_pkts_dropped_queue_full, 0);
            unsigned signals  = atomic_exchange(&p_sys->i_frame_signals, 0);
            unsigned seq_lost = atomic_exchange(&p_sys->i_pkts_seq_lost, 0);

            unsigned rtp_fail = 0, line_fail = 0, hdrs = 0, hdrs_f2 = 0;
            unsigned bad_geo = 0, out_of_range = 0;
            for (unsigned i = 0; i < p_sys->n_threads; i++) {
                worker_stats_t *ws = &p_sys->p_worker_stats[i];
                rtp_fail     += atomic_exchange(&ws->pkts_rtp_fail, 0);
                line_fail    += atomic_exchange(&ws->pkts_line_fail, 0);
                hdrs         += atomic_exchange(&ws->hdrs_total, 0);
                hdrs_f2      += atomic_exchange(&ws->hdrs_field2, 0);
                bad_geo      += atomic_exchange(&ws->hdrs_bad_geometry, 0);
                out_of_range += atomic_exchange(&ws->hdrs_out_of_range, 0);
            }

            msg_Warn(p_demux, "st2110: pkts=%u dropped=%u rtp_fail=%u line_fail=%u",
                     pkts, dropped, rtp_fail, line_fail);
            /* Genuine wire-level loss (gaps in the RTP sequence number,
               see TrackSequenceLoss()) -- distinct from `dropped` above
               (our own queue backpressure) and from rtp_fail/line_fail
               (packets that arrived but didn't parse). This is the same
               check done manually with pktmon/Wireshark earlier in this
               project, now always on. */
            if (seq_lost)
                msg_Warn(p_demux, "st2110: seq_lost=%u (packets that never "
                         "arrived, per RTP sequence gaps)", seq_lost);
            if (p_sys->b_interlace)
                msg_Warn(p_demux, "st2110: line headers total=%u field2=%u (%.1f%%)",
                         hdrs, hdrs_f2, hdrs ? 100.0 * hdrs_f2 / hdrs : 0.0);
            /* Headers that parsed correctly but were then rejected before
               ever reaching the master buffer -- i.e. valid data that our
               OWN checks discarded, not data lost on the wire. Any
               nonzero count here is a direct, exact measure of pixels
               that stayed stale (carried over from a previous write)
               instead of being refreshed this cycle. */
            if (bad_geo || out_of_range)
                msg_Warn(p_demux, "st2110: headers rejected: bad_geometry=%u "
                         "out_of_range=%u (this many stayed stale instead of "
                         "being refreshed)", bad_geo, out_of_range);
            /* Sanity check for the RTP-marker-bit assumption SenderThread
               now relies on: over 5s, this should land close to
               5*(fps_num/fps_den) -- e.g. ~150 for 29.97fps. Near 0 means
               this sender never sets the marker bit, and the sender
               thread is silently running on its bounded fallback timeout
               the whole time instead of the real frame boundary. */
            msg_Warn(p_demux, "st2110: frame_signals=%u (expected ~%.0f/5s if the "
                     "sender sets the RTP marker bit)",
                     signals, 5.0 * p_sys->i_fps_num / p_sys->i_fps_den);

            stat_window_start = mdate();
        }
#else
        (void)stat_window_start;
#endif
    }

    return NULL;
}

/* Minimal no-op pf_demux(): our real data delivery is entirely
 * asynchronous (SenderThread pushes via es_out_Send on its own schedule,
 * independent of this ever being called -- see the file header comment).
 * This exists only so VLC's input thread has something to call in its
 * normal main loop; leaving pf_demux NULL (as before) also appeared to
 * stop VLC from properly servicing an attached --input-slave (e.g. a
 * separate AES67/ST2110-30 audio SDP opened alongside this as the video
 * master), which needs the main input's loop to keep turning to pump the
 * slave too -- audio went silent specifically when this module was the
 * main input with pf_demux == NULL. A short sleep keeps this from
 * busy-spinning; returning 1 just means "nothing fatal, call again". */
static int Demux(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    if (atomic_load(&p_sys->b_stop_requested))
        return 0;
    msleep(10000); /* 10ms: frequent enough to keep VLC's input loop (and
                       whatever slave-servicing rides along with it)
                       turning, without spinning uselessly. */
    return 1;
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
    p_sys->i_last_rtp_seq = -1;  /* calloc() zeroed this; -1 means "no packet seen yet" */

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

    /* How many worker (processing) threads: an explicit st2110-threads
       option always wins; otherwise auto-detect from the CPU count,
       reserving TWO cores -- one for the single dedicated receive thread
       above, one for VLC's own decode/convert/render pipeline. These
       threads never touch the socket (see the file header comment for
       why that's now a dedicated single thread instead), so there's no
       RSS/kernel-affinity concern to work around here: this count is
       purely about how much parse/unpack CPU work can run in parallel. */
    int i_threads_opt = var_InheritInteger(p_demux, "st2110-threads");
    if (i_threads_opt > 0) {
        p_sys->n_threads = (unsigned)i_threads_opt;
    } else {
        unsigned n_cpu = DetectCpuCount();
        p_sys->n_threads = n_cpu > 2 ? n_cpu - 2 : 1;
    }

#ifdef _WIN32
    /* Not net_OpenDgram(): the receive socket must be created with
       WSA_FLAG_REGISTERED_IO up front for RIO (see OpenRioSocket() and
       the file header comment for why). This also does the bind and
       multicast join, and sets SO_RCVBUF itself. */
    if (OpenRioSocket(p_demux) != VLC_SUCCESS)
        goto error;
#else
    p_sys->fd = net_OpenDgram(p_demux, p_sys->psz_group, p_sys->i_port,
                               p_sys->psz_source, 0, IPPROTO_UDP);
    if (p_sys->fd == -1) {
        msg_Err(p_demux, "st2110: failed to join %s:%d", p_sys->psz_group, p_sys->i_port);
        goto error;
    }
    int rcvbuf = SO_RCVBUF_SIZE;
    setsockopt(p_sys->fd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
    { int flags = fcntl(p_sys->fd, F_GETFL, 0); fcntl(p_sys->fd, F_SETFL, flags | O_NONBLOCK); }
#endif

    p_sys->i_y_plane_size  = (size_t)p_sys->i_width * p_sys->i_height * 2;
    p_sys->i_uv_plane_size = (size_t)(p_sys->i_width / 2) * p_sys->i_height * 2;
    p_sys->i_buf_size = p_sys->i_y_plane_size + 2 * p_sys->i_uv_plane_size;
    p_sys->p_master_buf[0] = calloc(1, p_sys->i_buf_size);
    p_sys->p_master_buf[1] = calloc(1, p_sys->i_buf_size);
    if (!p_sys->p_master_buf[0] || !p_sys->p_master_buf[1])
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

    /* Real data still arrives asynchronously via the sender thread, not
       through this callback (see Demux()'s own comment for why it's a
       no-op that exists only to keep VLC's input loop -- and whatever
       --input-slave servicing rides along with it -- turning). */
    p_demux->pf_demux = Demux;
    p_demux->pf_control = Control;

    msg_Info(p_demux, "st2110: %s:%d %ux%u interlace=%d fps=%u/%u workers=%u",
             p_sys->psz_group, p_sys->i_port, p_sys->i_width, p_sys->i_height,
             p_sys->b_interlace, p_sys->i_fps_num, p_sys->i_fps_den, p_sys->n_threads);

    /* Staging queue, shared between the one receive thread and the N
       worker threads spawned below. */
    p_sys->i_queue_cap = QUEUE_CAPACITY;
    p_sys->p_queue = calloc(p_sys->i_queue_cap, sizeof(queue_slot_t));
    if (!p_sys->p_queue)
        goto error;
    vlc_mutex_init(&p_sys->queue_lock);
    p_sys->b_queue_lock_inited = true;
#ifdef _WIN32
    p_sys->queue_sem = CreateSemaphore(NULL, 0, QUEUE_CAPACITY, NULL);
    if (!p_sys->queue_sem) {
        msg_Err(p_demux, "st2110: CreateSemaphore failed for queue");
        goto error;
    }
#else
    vlc_cond_init(&p_sys->queue_not_empty);
    p_sys->b_queue_cond_inited = true;
#endif

    p_sys->p_worker_stats = calloc(p_sys->n_threads, sizeof(worker_stats_t));
    p_sys->p_worker_args  = calloc(p_sys->n_threads, sizeof(worker_arg_t));
    p_sys->worker_threads = calloc(p_sys->n_threads, sizeof(vlc_thread_t));
    if (!p_sys->p_worker_stats || !p_sys->p_worker_args || !p_sys->worker_threads)
        goto error;

    /* Frame-complete signal between whichever worker sees a field/frame's
       last packet and the sender thread (see SignalFrameComplete()). */
    vlc_mutex_init(&p_sys->frame_lock);
    p_sys->b_frame_lock_inited = true;
    vlc_cond_init(&p_sys->frame_cond);
    p_sys->b_frame_cond_inited = true;

    if (vlc_clone(&p_sys->receive_thread, ReceiveThread, p_demux, VLC_THREAD_PRIORITY_INPUT)) {
        msg_Err(p_demux, "st2110: failed to spawn receive thread");
        goto error;
    }
    p_sys->b_receive_started = true;

    for (unsigned i = 0; i < p_sys->n_threads; i++) {
        p_sys->p_worker_args[i].p_demux = p_demux;
        p_sys->p_worker_args[i].worker_index = i;
        if (vlc_clone(&p_sys->worker_threads[i], WorkerThread, &p_sys->p_worker_args[i],
                      VLC_THREAD_PRIORITY_INPUT)) {
            msg_Err(p_demux, "st2110: failed to spawn worker thread %u", i);
            goto error;
        }
        p_sys->n_workers_started++;
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

    if (p_sys->b_receive_started) {
        vlc_cancel(p_sys->receive_thread);
        vlc_join(p_sys->receive_thread, NULL);
    }

#ifdef _WIN32
    /* Must happen after the receive thread has stopped (it's the only
       thing still touching rio_cq/rio_rq), and before the socket itself
       is closed below via net_Close(). RIO_RQ has no separate close call
       -- it's torn down implicitly when the socket closes. */
    if (p_sys->b_rio_inited) {
        if (p_sys->rio_buf_id != RIO_INVALID_BUFFERID)
            p_sys->rio.RIODeregisterBuffer(p_sys->rio_buf_id);
        if (p_sys->rio_cq != RIO_INVALID_CQ)
            p_sys->rio.RIOCloseCompletionQueue(p_sys->rio_cq);
        if (p_sys->rio_event)
            CloseHandle(p_sys->rio_event);
        if (p_sys->p_rio_buf)
            VirtualFree(p_sys->p_rio_buf, 0, MEM_RELEASE);
    }
#endif

    /* Worker threads drain whatever is still queued, then notice
       b_stop_requested inside QueuePop (bounded to at most a 500ms wait
       if a wakeup signal is ever missed) and exit. */
    for (unsigned i = 0; i < p_sys->n_workers_started; i++) {
        vlc_cancel(p_sys->worker_threads[i]);
        vlc_join(p_sys->worker_threads[i], NULL);
    }
    free(p_sys->worker_threads);
    free(p_sys->p_worker_args);
    free(p_sys->p_worker_stats);

    if (p_sys->b_frame_cond_inited)
        vlc_cond_destroy(&p_sys->frame_cond);
    if (p_sys->b_frame_lock_inited)
        vlc_mutex_destroy(&p_sys->frame_lock);

#ifdef _WIN32
    if (p_sys->queue_sem)
        CloseHandle(p_sys->queue_sem);
#else
    if (p_sys->b_queue_cond_inited)
        vlc_cond_destroy(&p_sys->queue_not_empty);
#endif
    if (p_sys->b_queue_lock_inited)
        vlc_mutex_destroy(&p_sys->queue_lock);
    free(p_sys->p_queue);

    if (p_sys->fd != -1)
        net_Close(p_sys->fd);

    free(p_sys->psz_source);
    free(p_sys->p_master_buf[0]);
    free(p_sys->p_master_buf[1]);
    free(p_sys);
    p_demux->p_sys = NULL;
}
