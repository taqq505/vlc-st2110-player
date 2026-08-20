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

#ifndef _WIN32
# include <poll.h>
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

    /* frame accumulation (tightly packed planar I422_10L: Y, U, V) */
    uint8_t *p_buf;
    size_t   i_buf_size;
    size_t   i_y_plane_size;
    size_t   i_uv_plane_size;
    bool    *p_line_filled;
    unsigned i_lines_filled;
    bool     b_seen_field2_pkt;

    /* per-frame drop counters, reset after each finalized/dropped frame */
    unsigned i_drop_field2;
    unsigned i_drop_offset;
    unsigned i_drop_line_range;
    unsigned i_drop_stride;

    /* receive thread */
    vlc_thread_t thread;
    bool         b_thread_started;

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

vlc_module_begin()
    set_shortname("ST2110")
    set_description(N_("SMPTE ST 2110-20 receiver (RFC 4175, 10bit GPM)"))
    set_capability("access_demux", 0)
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACCESS)
    add_shortcut("st2110")
    set_callbacks(Open, Close)

    add_string("st2110-source", NULL, N_("Source filter address (SSM)"), NULL, true)
    add_integer("st2110-width", DEFAULT_WIDTH, N_("Width"), NULL, true)
    add_integer("st2110-height", DEFAULT_HEIGHT, N_("Height"), NULL, true)
    add_integer("st2110-depth", 10, N_("Sample depth (bits)"), NULL, true)
    add_string("st2110-sampling", "YCbCr-4:2:2", N_("Chroma sampling"), NULL, true)
    add_string("st2110-fps", "30000/1001", N_("Frame rate (num/den)"), NULL, true)
    add_string("st2110-colorimetry", "BT709", N_("Colorimetry (BT709/BT2020/ST2084/HLG)"), NULL, true)
    add_bool("st2110-interlace", false, N_("Interlaced"), NULL, true)
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
 * directly into the picture buffer's Y/U/V planes. Returns true if any
 * header in this packet belonged to field two. */
static bool WriteLines(demux_sys_t *p_sys, const rfc4175_line_hdr_t *hdrs,
                        unsigned n_hdrs, const uint8_t *data, size_t data_len)
{
    bool b_saw_field2 = false;
    size_t off = 0;
    unsigned half_w = p_sys->i_width / 2;

    uint16_t *y_plane = (uint16_t *)p_sys->p_buf;
    uint16_t *u_plane = (uint16_t *)(p_sys->p_buf + p_sys->i_y_plane_size);
    uint16_t *v_plane = (uint16_t *)(p_sys->p_buf + p_sys->i_y_plane_size
                                                    + p_sys->i_uv_plane_size);

    for (unsigned i = 0; i < n_hdrs; i++) {
        const rfc4175_line_hdr_t *h = &hdrs[i];
        if (h->field2)
            b_saw_field2 = true;

        if (off + h->length > data_len) {
            p_sys->i_drop_stride++;
            break; /* the rest of the chain can't be trusted either */
        }

        if (h->field2 && !p_sys->b_interlace) {
            p_sys->i_drop_field2++;
            off += h->length;
            continue;
        }
        if (h->length % 5 != 0 || h->offset % 2 != 0) {
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
        bool line_ok = true;

        for (unsigned j = 0; j < n_pgroups; j++) {
            unsigned x = h->offset + j * 2;
            if (x + 1 >= p_sys->i_width) {
                line_ok = false;
                break;
            }

            const uint8_t *b = seg + j * 5;
            uint16_t cb = (uint16_t)((b[0] << 2) | (b[1] >> 6));
            uint16_t y0 = (uint16_t)(((b[1] & 0x3f) << 4) | (b[2] >> 4));
            uint16_t cr = (uint16_t)(((b[2] & 0x0f) << 6) | (b[3] >> 2));
            uint16_t y1 = (uint16_t)(((b[3] & 0x03) << 8) | b[4]);

            y_plane[out_line * p_sys->i_width + x]     = y0;
            y_plane[out_line * p_sys->i_width + x + 1] = y1;
            u_plane[out_line * half_w + x / 2]          = cb;
            v_plane[out_line * half_w + x / 2]          = cr;
        }
        if (!line_ok)
            p_sys->i_drop_stride++;

        if (!p_sys->p_line_filled[out_line]) {
            p_sys->p_line_filled[out_line] = true;
            p_sys->i_lines_filled++;
        }

        off += h->length;
    }

    return b_saw_field2;
}

static void FinalizeFrame(demux_t *p_demux, demux_sys_t *p_sys)
{
    unsigned min_lines = p_sys->i_height / 2;

    if (p_sys->i_lines_filled < min_lines) {
        p_sys->i_pkts_no_frame++;
    } else {
        block_t *p_block = block_Alloc(p_sys->i_buf_size);
        if (p_block) {
            memcpy(p_block->p_buffer, p_sys->p_buf, p_sys->i_buf_size);
            p_block->i_dts = p_block->i_pts = date_Get(&p_sys->pts);
            es_out_Control(p_demux->out, ES_OUT_SET_PCR, p_block->i_pts);
            es_out_Send(p_demux->out, p_sys->p_es, p_block);
        }
        date_Increment(&p_sys->pts, 1);
    }

    if (p_sys->i_drop_field2 || p_sys->i_drop_offset || p_sys->i_drop_line_range
     || p_sys->i_drop_stride || p_sys->i_lines_filled < p_sys->i_height) {
        msg_Warn(p_demux, "st2110: frame lines=%u/%u drop(field2=%u offset=%u range=%u stride=%u)",
                 p_sys->i_lines_filled, p_sys->i_height, p_sys->i_drop_field2,
                 p_sys->i_drop_offset, p_sys->i_drop_line_range, p_sys->i_drop_stride);
    }

    memset(p_sys->p_buf, 0, p_sys->i_buf_size);
    memset(p_sys->p_line_filled, 0, p_sys->i_height * sizeof(bool));
    p_sys->i_lines_filled = 0;
    p_sys->i_drop_field2 = p_sys->i_drop_offset = 0;
    p_sys->i_drop_line_range = p_sys->i_drop_stride = 0;
}

/* Dedicated receive thread. VLC's own RTP module (modules/access/rtp/rtp.c)
 * uses this same pattern for live network sources: pf_demux is left NULL and
 * a vlc_clone()'d thread pushes blocks to es_out asynchronously, rather than
 * the core polling pf_demux. poll() here resolves to vlc_poll() (see
 * vlc_threads.h), which is VLC's cancellation-aware wrapper -- raw select()
 * has no such wrapper and would make Close()'s vlc_join() hang forever. */
static void *ReceiveThread(void *data)
{
    demux_t *p_demux = data;
    demux_sys_t *p_sys = p_demux->p_sys;
    uint8_t pkt[RECV_BUF_LEN];
    struct pollfd ufd;

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

        ssize_t len = recv(p_sys->fd, (char *)pkt, sizeof(pkt), 0);
        if (len <= 0) {
            vlc_restorecancel(canc);
            continue;
        }
        p_sys->i_pkts_total++;

        unsigned hdr_len;
        uint32_t ts;
        bool marker;
        if (ParseRTP(pkt, (size_t)len, &hdr_len, &ts, &marker) != 0) {
            p_sys->i_pkts_rtp_fail++;
            vlc_restorecancel(canc);
            continue;
        }

        const uint8_t *payload = pkt + hdr_len;
        size_t payload_len = (size_t)len - hdr_len;
        if (payload_len < RFC4175_EXT_SEQ_LEN) {
            p_sys->i_pkts_line_fail++;
            vlc_restorecancel(canc);
            continue;
        }
        payload += RFC4175_EXT_SEQ_LEN;
        payload_len -= RFC4175_EXT_SEQ_LEN;

        rfc4175_line_hdr_t hdrs[MAX_LINE_SEGMENTS];
        unsigned n_hdrs;
        size_t hdr_bytes;
        if (ParseLineHeaders(payload, payload_len, hdrs, MAX_LINE_SEGMENTS,
                              &n_hdrs, &hdr_bytes) != 0) {
            p_sys->i_pkts_line_fail++;
            vlc_restorecancel(canc);
            continue;
        }

        DetectLineMode(p_demux, p_sys, hdrs, n_hdrs);

        bool saw_field2 = WriteLines(p_sys, hdrs, n_hdrs,
                                      payload + hdr_bytes, payload_len - hdr_bytes);
        if (saw_field2)
            p_sys->b_seen_field2_pkt = true;

        if (marker && (!p_sys->b_interlace || p_sys->b_seen_field2_pkt)) {
            FinalizeFrame(p_demux, p_sys);
            p_sys->b_seen_field2_pkt = false;
        }

        if (mdate() - p_sys->i_stat_window_start > 5 * CLOCK_FREQ) {
            if (p_sys->i_pkts_rtp_fail || p_sys->i_pkts_line_fail || p_sys->i_pkts_no_frame)
                msg_Warn(p_demux, "st2110: pkts=%u rtp_fail=%u line_fail=%u no_frame=%u",
                         p_sys->i_pkts_total, p_sys->i_pkts_rtp_fail,
                         p_sys->i_pkts_line_fail, p_sys->i_pkts_no_frame);
            p_sys->i_pkts_total = p_sys->i_pkts_rtp_fail = 0;
            p_sys->i_pkts_line_fail = p_sys->i_pkts_no_frame = 0;
            p_sys->i_idle_polls = 0;
            p_sys->i_stat_window_start = mdate();
        }

        vlc_restorecancel(canc);
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

    p_sys->fd = net_OpenDgram(p_demux, p_sys->psz_group, p_sys->i_port,
                               p_sys->psz_source, 0, IPPROTO_UDP);
    if (p_sys->fd == -1) {
        msg_Err(p_demux, "st2110: failed to join %s:%d", p_sys->psz_group, p_sys->i_port);
        goto error;
    }
    int rcvbuf = SO_RCVBUF_SIZE;
    setsockopt(p_sys->fd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));

    p_sys->i_y_plane_size  = (size_t)p_sys->i_width * p_sys->i_height * 2;
    p_sys->i_uv_plane_size = (size_t)(p_sys->i_width / 2) * p_sys->i_height * 2;
    p_sys->i_buf_size = p_sys->i_y_plane_size + 2 * p_sys->i_uv_plane_size;
    p_sys->p_buf = calloc(1, p_sys->i_buf_size);
    p_sys->p_line_filled = calloc(p_sys->i_height, sizeof(bool));
    if (!p_sys->p_buf || !p_sys->p_line_filled)
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

    p_demux->pf_demux = NULL;      /* live source: pushed asynchronously by ReceiveThread */
    p_demux->pf_control = Control;

    msg_Info(p_demux, "st2110: %s:%d %ux%u interlace=%d fps=%u/%u",
             p_sys->psz_group, p_sys->i_port, p_sys->i_width, p_sys->i_height,
             p_sys->b_interlace, p_sys->i_fps_num, p_sys->i_fps_den);

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

    if (p_sys->b_thread_started) {
        vlc_cancel(p_sys->thread);
        vlc_join(p_sys->thread, NULL);
    }
    if (p_sys->fd != -1)
        net_Close(p_sys->fd);

    free(p_sys->psz_source);
    free(p_sys->p_buf);
    free(p_sys->p_line_filled);
    free(p_sys);
    p_demux->p_sys = NULL;
}
