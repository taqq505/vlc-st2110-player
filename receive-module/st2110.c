/*
 * st2110.c: SMPTE ST 2110-20 (RFC 4175) uncompressed video access_demux for VLC 3.0.x
 * See docs/vlc-st2110_receive-module_仕様書.md for the full specification.
 */

/* vlc_common.h pulls in vlc_threads.h, which uses poll()/struct pollfd.
 * On MinGW those only come from winsock2.h, and only once _WIN32_WINNT
 * requests Vista+ (WSAPoll). Must be set up before any vlc_*.h include. */
#ifdef _WIN32
# ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0601
# endif
# ifndef WINVER
#  define WINVER 0x0601
# endif
# include <winsock2.h>
# include <ws2tcpip.h>
/* mingw-w64 provides struct pollfd/WSAPoll() via winsock2.h (given the
 * _WIN32_WINNT bump above) but no plain poll(). VLC's own poll() shim
 * lives in vlc_fixups.h, which is only wired in through VLC's internally
 * generated config.h -- not available to an out-of-tree plugin build.
 * Alias it directly; vlc_threads.h later redefines poll() to its own
 * vlc_poll() wrapper for everything after that point, which is fine. */
# define poll(fds, nfds, timeout) WSAPoll((void *)(fds), (nfds), (timeout))
#else
# include <sys/socket.h>
# include <netinet/in.h>
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_demux.h>
#include <vlc_network.h>
#include <vlc_block.h>
#include <vlc_es.h>
#include <vlc_fourcc.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
# include <errno.h>
#endif

static int  Open(vlc_object_t *);
static void Close(vlc_object_t *);
static int  Demux(demux_t *);
static int  Control(demux_t *, int, va_list);

vlc_module_begin()
    set_shortname("ST2110")
    set_description("SMPTE ST 2110-20 uncompressed video receiver")
    set_capability("access_demux", 0)
    set_callbacks(Open, Close)
    add_shortcut("st2110")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACCESS)

    add_string ("st2110-source",      "",             "SSM source address", NULL, true)
    add_integer("st2110-width",       1920,           "Width", NULL, true)
    add_integer("st2110-height",      1080,           "Height", NULL, true)
    add_integer("st2110-depth",       10,             "Bit depth", NULL, true)
    add_string ("st2110-sampling",    "YCbCr-4:2:2",  "Chroma sampling", NULL, true)
    add_string ("st2110-fps",         "60000/1001",   "Frame rate (num/den)", NULL, true)
    add_bool   ("st2110-interlace",   false,          "Interlaced", NULL, true)
    add_string ("st2110-colorimetry", "BT709",        "Colorimetry", NULL, true)
    add_string ("st2110-tcs",         "SDR",          "Transfer characteristics", NULL, true)
vlc_module_end()

/* pgroup = 2 pixels of YCbCr 4:2:2 10bit, packed into 5 octets (RFC 4175). */
#define MAX_LINE_HDRS 180

typedef struct
{
    uint16_t len;     /* octets of pixel data carried by this segment */
    uint16_t line;    /* line number (per-field if interlaced, else per-frame) */
    uint16_t offset;  /* pixel offset within the line (even) */
    bool     field2;  /* F bit: false=top/progressive, true=bottom field */
} line_hdr_t;

struct demux_sys_t
{
    /* configuration (owned strings) */
    char     *psz_source;
    char     *psz_sampling;
    char     *psz_colorimetry;
    char     *psz_tcs;
    unsigned  i_width;
    unsigned  i_height;
    unsigned  i_depth;
    unsigned  i_fps_num;
    unsigned  i_fps_den;
    bool      b_interlace;

    /* socket */
    int       fd;
    char      psz_group[256];
    int       i_port;
    unsigned  i_idle_polls; /* consecutive receive timeouts with zero data */

    /* elementary stream */
    es_out_id_t *es;
    mtime_t      i_pts_delay;

    /* packed-frame accumulation buffer: height * stride_packed bytes */
    uint8_t  *p_buf;
    size_t    i_stride_packed;
    size_t    i_buf_size;
    bool     *p_line_filled;
    unsigned  i_lines_filled;

    /* line-segment drop counters for the frame currently being assembled.
     * Logged as one summary per frame in FinalizeFrame rather than per
     * packet: on a real high-bitrate stream a systematic mismatch (e.g.
     * wrong line-numbering assumption) can drop the large majority of
     * packets, and logging each one can emit tens of thousands of lines
     * per second -- enough to flood VLC's message console and hang the
     * whole UI, especially with the Messages window open. */
    unsigned  i_drop_field2;
    unsigned  i_drop_offset;
    unsigned  i_drop_line_range;
    unsigned  i_drop_stride;

    /* current in-progress frame (progressive) */
    bool      b_frame_open;
    uint32_t  i_frame_ts;

    /* current in-progress field pair (interlace only): whether a
     * marker-terminated top (F=0) / bottom (F=1) field has been woven
     * into the shared frame buffer since the last flush. */
    bool      b_field_seen[2];
};

/* ------------------------------------------------------------------------ */

static void ParseFraction(const char *s, unsigned *num, unsigned *den)
{
    unsigned n = 60000, d = 1001;
    unsigned a, b;

    if (s && *s)
    {
        if (sscanf(s, "%u/%u", &a, &b) == 2 && b > 0)
        {
            n = a;
            d = b;
        }
        else if (sscanf(s, "%u", &a) == 1 && a > 0)
        {
            n = a;
            d = 1;
        }
    }
    *num = n;
    *den = d;
}

static void SetColorimetry(video_format_t *v, const char *colorimetry, const char *tcs)
{
    if (colorimetry && !strcmp(colorimetry, "BT2020"))
    {
        v->primaries = COLOR_PRIMARIES_BT2020;
        v->space     = COLOR_SPACE_BT2020;
        if (tcs && !strcmp(tcs, "PQ"))
            v->transfer = TRANSFER_FUNC_SMPTE_ST2084;
        else if (tcs && !strcmp(tcs, "HLG"))
            v->transfer = TRANSFER_FUNC_HLG;
        else
            /* video_transfer_func_t has no dedicated BT2020 value in this
             * VLC version; BT.2020 SDR's OETF is numerically the same as
             * BT.709's, so that's the correct fallback, not a placeholder. */
            v->transfer = TRANSFER_FUNC_BT709;
    }
    else
    {
        v->primaries = COLOR_PRIMARIES_BT709;
        v->space     = COLOR_SPACE_BT709;
        v->transfer  = TRANSFER_FUNC_BT709;
    }
    v->b_color_range_full = false; /* narrow/video range, per spec §5.8 */
}

/* RTP header (RFC 3550). Validates all offsets against the received length
 * before touching them -- this parses attacker-reachable network input. */
static bool ParseRTP(const uint8_t *p, size_t len, uint32_t *pts_ts, bool *marker,
                      const uint8_t **out_payload, size_t *out_len)
{
    if (len < 12 || (p[0] >> 6) != 2)
        return false;

    bool     pad = (p[0] & 0x20) != 0;
    bool     ext = (p[0] & 0x10) != 0;
    unsigned cc  = p[0] & 0x0f;

    *marker = (p[1] & 0x80) != 0;

    size_t hdr = 12 + (size_t)cc * 4;
    if (hdr > len)
        return false;

    if (ext)
    {
        if (hdr + 4 > len)
            return false;
        unsigned ext_words = ((unsigned)p[hdr + 2] << 8) | p[hdr + 3];
        hdr += 4 + (size_t)ext_words * 4;
        if (hdr > len)
            return false;
    }

    size_t plen = len - hdr;
    if (pad)
    {
        if (plen == 0)
            return false;
        uint8_t pad_len = p[len - 1];
        if (pad_len == 0 || pad_len > plen)
            return false;
        plen -= pad_len;
    }

    *pts_ts     = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16)
                | ((uint32_t)p[6] << 8)  |  (uint32_t)p[7];
    *out_payload = p + hdr;
    *out_len     = plen;
    return true;
}

/* ST 2110-20 payload header: 2-octet ESN, then a chain of 6-octet line
 * headers (continuation bit C==1 while more follow), then the pixel data
 * for each declared segment concatenated in the same order. */
static bool ParseLineHeaders(const uint8_t *p, size_t len,
                              line_hdr_t *lines, unsigned *n_lines,
                              const uint8_t **out_pixels, size_t *out_pixels_len)
{
    if (len < 2)
        return false;

    size_t   pos = 2; /* skip Extended Sequence Number */
    unsigned k = 0;

    for (;;)
    {
        if (pos + 6 > len || k >= MAX_LINE_HDRS)
            return false;

        uint16_t seg_len = ((uint16_t)p[pos]     << 8) | p[pos + 1];
        uint16_t fl       = ((uint16_t)p[pos + 2] << 8) | p[pos + 3];
        uint16_t co       = ((uint16_t)p[pos + 4] << 8) | p[pos + 5];
        pos += 6;

        lines[k].len    = seg_len;
        lines[k].field2 = (fl >> 15) != 0;
        lines[k].line   = fl & 0x7fff;
        lines[k].offset = co & 0x7fff;
        bool cont        = (co >> 15) != 0;
        k++;

        if (!cont)
            break;
    }

    size_t total = 0;
    for (unsigned i = 0; i < k; i++)
        total += lines[i].len;
    if (pos + total > len)
        return false; /* declared segment lengths exceed what was received */

    *n_lines         = k;
    *out_pixels      = p + pos;
    *out_pixels_len  = total;
    return true;
}

static void ResetFrameBuffer(demux_sys_t *p_sys)
{
    memset(p_sys->p_buf, 0, p_sys->i_buf_size);
    memset(p_sys->p_line_filled, 0, p_sys->i_height * sizeof(bool));
    p_sys->i_lines_filled = 0;
    p_sys->i_drop_field2 = p_sys->i_drop_offset = p_sys->i_drop_line_range = p_sys->i_drop_stride = 0;
}

/* Writes each declared line segment into the packed frame buffer. Every
 * segment is bounds-checked against height/stride before the memcpy: this
 * data comes straight off the wire and must never drive an out-of-bounds
 * write. Segments that fail validation are dropped and counted (not logged
 * per-segment -- see the i_drop_* counters and their summary log in
 * FinalizeFrame); the rest of the frame is still assembled best-effort.
 *
 * In interlace mode, the wire's Line No is a per-field line index (0 to
 * height/2 - 1) and F (field2) selects top (0) or bottom (1); the two
 * fields are woven into one full-height buffer as actual_line = line*2+F,
 * matching how VLC expects a full picture. In progressive mode Line No is
 * already the actual frame line and field2 is expected to be unset. */
static void WriteLines(demux_t *p_demux, const line_hdr_t *lines, unsigned n_lines,
                        const uint8_t *pixels, size_t pixels_len)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    size_t pos = 0;

    for (unsigned i = 0; i < n_lines; i++)
    {
        uint16_t seg_len = lines[i].len;
        if (pos + seg_len > pixels_len)
            break;

        if (!p_sys->b_interlace && lines[i].field2)
        {
            p_sys->i_drop_field2++;
        }
        else if (lines[i].offset % 2 != 0)
        {
            p_sys->i_drop_offset++;
        }
        else
        {
            unsigned field_lines = p_sys->b_interlace ? p_sys->i_height / 2 : p_sys->i_height;
            if (lines[i].line >= field_lines)
            {
                p_sys->i_drop_line_range++;
                pos += seg_len;
                continue;
            }

            unsigned actual_line = p_sys->b_interlace
                                  ? (unsigned)lines[i].line * 2 + (lines[i].field2 ? 1 : 0)
                                  : lines[i].line;
            size_t byte_off = ((size_t)lines[i].offset / 2) * 5;
            if (byte_off + seg_len > p_sys->i_stride_packed)
            {
                p_sys->i_drop_stride++;
            }
            else
            {
                memcpy(p_sys->p_buf + (size_t)actual_line * p_sys->i_stride_packed + byte_off,
                       pixels + pos, seg_len);
                if (!p_sys->p_line_filled[actual_line])
                {
                    p_sys->p_line_filled[actual_line] = true;
                    p_sys->i_lines_filled++;
                }
            }
        }

        pos += seg_len;
    }
}

/* Unpacks the accumulated 10bit 4:2:2 GPM buffer into a planar
 * VLC_CODEC_I422_10L block (10bit samples in the low bits of 16bit LE
 * words). Frames with too few received lines are dropped rather than
 * shown as mostly-black/garbage (threshold left to implementer discretion
 * per the spec's open question). */
static block_t *FinalizeFrame(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_sys->i_drop_field2 || p_sys->i_drop_offset || p_sys->i_drop_line_range || p_sys->i_drop_stride)
    {
        msg_Warn(p_demux, "dropped segments this frame: field2=%u odd-offset=%u "
                  "line-range=%u stride=%u",
                  p_sys->i_drop_field2, p_sys->i_drop_offset,
                  p_sys->i_drop_line_range, p_sys->i_drop_stride);
    }

    if (p_sys->i_lines_filled < p_sys->i_height / 2)
    {
        msg_Warn(p_demux, "dropping frame: only %u/%u lines received",
                  p_sys->i_lines_filled, p_sys->i_height);
        return NULL;
    }

    unsigned w = p_sys->i_width, h = p_sys->i_height;
    size_t y_plane = (size_t)w * h * 2;
    size_t c_plane = (size_t)(w / 2) * h * 2;

    block_t *block = block_Alloc(y_plane + 2 * c_plane);
    if (!block)
        return NULL;

    uint8_t *py = block->p_buffer;
    uint8_t *pu = py + y_plane;
    uint8_t *pv = pu + c_plane;
    size_t y_stride = (size_t)w * 2;
    size_t c_stride = (size_t)(w / 2) * 2;

    for (unsigned y = 0; y < h; y++)
    {
        const uint8_t *row  = p_sys->p_buf + (size_t)y * p_sys->i_stride_packed;
        uint8_t       *yrow = py + (size_t)y * y_stride;
        uint8_t       *urow = pu + (size_t)y * c_stride;
        uint8_t       *vrow = pv + (size_t)y * c_stride;

        for (unsigned x2 = 0; x2 < w / 2; x2++)
        {
            const uint8_t *pg = row + (size_t)x2 * 5;
            uint16_t cb = ((uint16_t)pg[0] << 2) | (pg[1] >> 6);
            uint16_t y0 = (((uint16_t)pg[1] & 0x3f) << 4) | (pg[2] >> 4);
            uint16_t cr = (((uint16_t)pg[2] & 0x0f) << 6) | (pg[3] >> 2);
            uint16_t y1 = (((uint16_t)pg[3] & 0x03) << 8) |  pg[4];

            uint8_t *yy = yrow + (size_t)x2 * 4;
            yy[0] = (uint8_t)y0;       yy[1] = (uint8_t)(y0 >> 8);
            yy[2] = (uint8_t)y1;       yy[3] = (uint8_t)(y1 >> 8);

            uint8_t *uu = urow + (size_t)x2 * 2;
            uu[0] = (uint8_t)cb;       uu[1] = (uint8_t)(cb >> 8);

            uint8_t *vv = vrow + (size_t)x2 * 2;
            vv[0] = (uint8_t)cr;       vv[1] = (uint8_t)(cr >> 8);
        }
    }

    block->i_dts = block->i_pts = mdate() + p_sys->i_pts_delay;
    return block;
}

/* ------------------------------------------------------------------------ */

static int Open(vlc_object_t *p_this)
{
    demux_t *p_demux = (demux_t *)p_this;

    if (!p_demux->psz_location || !*p_demux->psz_location)
    {
        msg_Err(p_demux, "missing MRL, expected st2110://<group>:<port>");
        return VLC_EGENERIC;
    }

    char     psz_group[256];
    int      i_port;
    {
        const char *psz_loc   = p_demux->psz_location;
        const char *psz_colon = strrchr(psz_loc, ':');
        if (!psz_colon || psz_colon == psz_loc)
        {
            msg_Err(p_demux, "invalid MRL, expected st2110://<group>:<port>");
            return VLC_EGENERIC;
        }
        size_t len = (size_t)(psz_colon - psz_loc);
        if (len == 0 || len >= sizeof(psz_group))
        {
            msg_Err(p_demux, "invalid MRL: group address too long");
            return VLC_EGENERIC;
        }
        memcpy(psz_group, psz_loc, len);
        psz_group[len] = '\0';

        i_port = atoi(psz_colon + 1);
        if (i_port <= 0 || i_port > 65535)
        {
            msg_Err(p_demux, "invalid MRL: bad port");
            return VLC_EGENERIC;
        }
    }

    demux_sys_t *p_sys = calloc(1, sizeof(*p_sys));
    if (!p_sys)
        return VLC_ENOMEM;
    p_demux->p_sys = p_sys;
    p_sys->fd = -1;
    memcpy(p_sys->psz_group, psz_group, sizeof(psz_group));
    p_sys->i_port = i_port;

    p_sys->psz_source      = var_InheritString(p_demux, "st2110-source");
    p_sys->i_width          = var_InheritInteger(p_demux, "st2110-width");
    p_sys->i_height         = var_InheritInteger(p_demux, "st2110-height");
    p_sys->i_depth          = var_InheritInteger(p_demux, "st2110-depth");
    p_sys->psz_sampling    = var_InheritString(p_demux, "st2110-sampling");
    p_sys->b_interlace      = var_InheritBool(p_demux, "st2110-interlace");
    p_sys->psz_colorimetry = var_InheritString(p_demux, "st2110-colorimetry");
    p_sys->psz_tcs         = var_InheritString(p_demux, "st2110-tcs");
    {
        char *psz_fps = var_InheritString(p_demux, "st2110-fps");
        ParseFraction(psz_fps, &p_sys->i_fps_num, &p_sys->i_fps_den);
        free(psz_fps);
    }

    if (p_sys->i_depth != 10)
    {
        msg_Err(p_demux, "unsupported st2110-depth=%u (only 10 is supported)", p_sys->i_depth);
        goto error;
    }
    if (!p_sys->psz_sampling || strcmp(p_sys->psz_sampling, "YCbCr-4:2:2") != 0)
    {
        msg_Err(p_demux, "unsupported st2110-sampling (only YCbCr-4:2:2 is supported)");
        goto error;
    }
    if (p_sys->i_width == 0 || (p_sys->i_width % 2) != 0 || p_sys->i_height == 0)
    {
        msg_Err(p_demux, "invalid st2110-width/st2110-height");
        goto error;
    }
    if (p_sys->b_interlace && (p_sys->i_height % 2) != 0)
    {
        msg_Err(p_demux, "invalid st2110-height for interlace (must be even, one field is height/2)");
        goto error;
    }

    p_sys->fd = net_OpenDgram(p_this, psz_group, i_port,
                               (p_sys->psz_source && *p_sys->psz_source) ? p_sys->psz_source : NULL,
                               0, IPPROTO_UDP);
    if (p_sys->fd < 0)
    {
        msg_Err(p_demux, "failed to open multicast socket %s:%d", psz_group, i_port);
        goto error;
    }
    {
        int i_rcvbuf = 32 * 1024 * 1024;
        setsockopt(p_sys->fd, SOL_SOCKET, SO_RCVBUF, (const char *)&i_rcvbuf, sizeof(i_rcvbuf));
    }
    /* Without a receive timeout, Demux() blocks in net_Read forever if no
     * packets ever arrive (wrong SSM source, no PIM-SSM route to this host,
     * a firewall, ...) with zero visibility into why. Timing out lets
     * Demux() log periodic diagnostics instead of just sitting silent. */
#ifdef _WIN32
    {
        DWORD timeout_ms = 2000;
        setsockopt(p_sys->fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
    }
#else
    {
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(p_sys->fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    }
#endif

    p_sys->i_stride_packed = (size_t)(p_sys->i_width / 2) * 5;
    p_sys->i_buf_size      = p_sys->i_stride_packed * (size_t)p_sys->i_height;
    p_sys->p_buf           = malloc(p_sys->i_buf_size);
    p_sys->p_line_filled   = calloc(p_sys->i_height, sizeof(bool));
    if (!p_sys->p_buf || !p_sys->p_line_filled)
        goto error;

    p_sys->i_pts_delay = (mtime_t)var_InheritInteger(p_demux, "network-caching") * INT64_C(1000);
    if (p_sys->i_pts_delay <= 0)
        p_sys->i_pts_delay = INT64_C(200000); /* 200ms default, matches Lua extension's default */

    es_format_t fmt;
    es_format_Init(&fmt, VIDEO_ES, VLC_CODEC_I422_10L);
    fmt.video.i_width  = fmt.video.i_visible_width  = p_sys->i_width;
    fmt.video.i_height = fmt.video.i_visible_height = p_sys->i_height;
    fmt.video.i_sar_num = fmt.video.i_sar_den = 1;
    fmt.video.i_frame_rate      = p_sys->i_fps_num;
    fmt.video.i_frame_rate_base = p_sys->i_fps_den;
    SetColorimetry(&fmt.video, p_sys->psz_colorimetry, p_sys->psz_tcs);

    p_sys->es = es_out_Add(p_demux->out, &fmt);
    if (!p_sys->es)
        goto error;

    p_demux->pf_demux   = Demux;
    p_demux->pf_control = Control;

    return VLC_SUCCESS;

error:
    Close(p_this);
    return VLC_EGENERIC;
}

static void Close(vlc_object_t *p_this)
{
    demux_t     *p_demux = (demux_t *)p_this;
    demux_sys_t *p_sys   = p_demux->p_sys;

    if (!p_sys)
        return;

    if (p_sys->fd >= 0)
        net_Close(p_sys->fd);

    free(p_sys->psz_source);
    free(p_sys->psz_sampling);
    free(p_sys->psz_colorimetry);
    free(p_sys->psz_tcs);
    free(p_sys->p_buf);
    free(p_sys->p_line_filled);
    free(p_sys);
    p_demux->p_sys = NULL;
}

/* True if the last socket call failed merely because SO_RCVTIMEO elapsed
 * with no data (i.e. not a real error). */
static bool LastRecvTimedOut(void)
{
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT;
#endif
}

/* Reads packets until one full frame is assembled (marker bit), or a
 * timestamp change reveals that the previous frame's marker was lost, then
 * sends exactly one block and returns -- per spec §5.3. */
static int Demux(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    uint8_t pkt[9000];

    for (;;)
    {
        ssize_t n = net_Read(VLC_OBJECT(p_demux), p_sys->fd, pkt, sizeof(pkt));
        if (n < 0)
        {
            if (LastRecvTimedOut())
            {
                p_sys->i_idle_polls++;
                /* SO_RCVTIMEO is 2s; log roughly every 10s of total silence,
                 * not every timeout, to stay well clear of the message-flood
                 * failure mode this module hit before (see WriteLines). */
                if (p_sys->i_idle_polls % 5 == 1)
                    msg_Warn(p_demux, "no data received in %us on %s:%d (SSM source \"%s\") "
                              "-- check multicast routing/PIM-SSM reachability from this host",
                              p_sys->i_idle_polls * 2, p_sys->psz_group, p_sys->i_port,
                              (p_sys->psz_source && *p_sys->psz_source) ? p_sys->psz_source : "(none/ASM)");
                continue;
            }
            return VLC_DEMUXER_EGENERIC;
        }
        p_sys->i_idle_polls = 0;
        if (n == 0)
            continue;

        uint32_t       ts;
        bool           marker;
        const uint8_t *payload;
        size_t         payload_len;
        if (!ParseRTP(pkt, (size_t)n, &ts, &marker, &payload, &payload_len))
            continue;

        line_hdr_t     lines[MAX_LINE_HDRS];
        unsigned       n_lines;
        const uint8_t *pixels;
        size_t         pixels_len;
        if (!ParseLineHeaders(payload, payload_len, lines, &n_lines, &pixels, &pixels_len))
            continue;

        if (p_sys->b_interlace)
        {
            /* Each field carries its own RTP timestamp, so unlike the
             * progressive path there's no single "current frame timestamp"
             * to detect a lost marker against; a field is only considered
             * done once its own marker arrives. Both fields weave into the
             * same buffer (WriteLines maps line*2+F), and the full picture
             * is sent once both top and bottom have each been marker-
             * terminated since the last flush -- in whichever order they
             * arrive. This does not recover from a lost field marker. */
            WriteLines(p_demux, lines, n_lines, pixels, pixels_len);

            if (marker && n_lines > 0)
            {
                unsigned field = lines[n_lines - 1].field2 ? 1 : 0;
                p_sys->b_field_seen[field] = true;

                if (p_sys->b_field_seen[0] && p_sys->b_field_seen[1])
                {
                    block_t *cur = FinalizeFrame(p_demux);
                    ResetFrameBuffer(p_sys);
                    p_sys->b_field_seen[0] = p_sys->b_field_seen[1] = false;
                    if (cur)
                    {
                        es_out_Send(p_demux->out, p_sys->es, cur);
                        return VLC_DEMUXER_SUCCESS;
                    }
                    /* frame dropped (too few lines received): keep reading */
                }
            }
            continue;
        }

        block_t *to_send = NULL;

        if (p_sys->b_frame_open && ts != p_sys->i_frame_ts)
        {
            to_send = FinalizeFrame(p_demux);
            ResetFrameBuffer(p_sys);
            p_sys->b_frame_open = false;
        }

        if (!p_sys->b_frame_open)
        {
            p_sys->i_frame_ts   = ts;
            p_sys->b_frame_open = true;
        }

        WriteLines(p_demux, lines, n_lines, pixels, pixels_len);

        if (to_send)
        {
            es_out_Send(p_demux->out, p_sys->es, to_send);
            return VLC_DEMUXER_SUCCESS;
        }

        if (marker)
        {
            block_t *cur = FinalizeFrame(p_demux);
            ResetFrameBuffer(p_sys);
            p_sys->b_frame_open = false;
            if (cur)
            {
                es_out_Send(p_demux->out, p_sys->es, cur);
                return VLC_DEMUXER_SUCCESS;
            }
            /* frame dropped (too few lines received): keep reading */
        }
    }
}

static int Control(demux_t *p_demux, int i_query, va_list args)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    switch (i_query)
    {
        case DEMUX_CAN_PAUSE:
        case DEMUX_CAN_SEEK:
            *va_arg(args, bool *) = false;
            return VLC_SUCCESS;

        case DEMUX_CAN_CONTROL_PACE:
            *va_arg(args, bool *) = true;
            return VLC_SUCCESS;

        case DEMUX_GET_PTS_DELAY:
            *va_arg(args, int64_t *) = p_sys->i_pts_delay;
            return VLC_SUCCESS;

        case DEMUX_SET_PAUSE_STATE:
            return VLC_SUCCESS;

        default:
            return VLC_EGENERIC;
    }
}
