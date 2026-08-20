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
# include <sys/select.h>
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

static int   Open(vlc_object_t *);
static void  Close(vlc_object_t *);
static void *ReceiveThread(void *);
static int   Control(demux_t *, int, va_list);

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
    int       line_mode; /* LINE_MODE_*, see WriteLines */

    /* socket */
    int       fd;
    char      psz_group[256];
    int       i_port;
    unsigned  i_idle_polls; /* consecutive receive timeouts with zero data */

    /* receive thread: modules/access/rtp/rtp.c (VLC's own RTP access_demux)
     * runs its own vlc_clone()'d thread that reads the socket and calls
     * es_out_Send() directly, rather than implementing pf_demux -- pf_demux
     * is left NULL there, exactly like here. Live network sources don't fit
     * the "core pulls one demux worth of data at a time" model pf_demux
     * implies; a self-driven thread matches how VLC's own such module
     * actually does it. */
    vlc_thread_t thread;
    bool         thread_ready;

    /* packets ARE arriving but no frame has completed yet -- e.g. the
     * marker bit is never set by this sender, or (interlace) the two
     * fields never both get marker-terminated. Distinct from i_idle_polls
     * (zero packets); logged on its own timer, see ReceiveThread(). Broken down by
     * pipeline stage so a systematic parse failure (which would silently
     * `continue` before ever reaching the old single counter) is visible
     * too -- every received packet is accounted for somewhere. */
    unsigned i_pkts_total;
    unsigned i_pkts_rtp_fail;
    unsigned i_pkts_line_fail;
    unsigned i_pkts_no_frame;
    mtime_t  i_last_no_frame_log;

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

/* SMPTE ST 2110-20:2017 §6.1.5 Note 1 mandates the SRD Row Number ("Line
 * No") be a ZERO-BASED sample row number, distinct from RFC 4175's own
 * SMPTE-274M/296M raw-raster numbering (SDI-legacy, includes blanking).
 * In practice, though, many real senders -- especially SDI-to-IP gateways
 * carrying a camera's native SDI line numbers straight through -- emit the
 * SDI-legacy numbers instead of renumbering to zero-based. A receiver that
 * hardcodes either convention silently drops most of a real stream sent
 * with the other one. So: detect which convention this sender is actually
 * using from the observed values, rather than assume.
 *
 * For SMPTE 274M (1920x1080, the only raster this module supports) the two
 * conventions are numerically disjoint for field two: zero-based field two
 * is 0-539, SDI-legacy field two is 584-1123. LINE_MODE_SDI_LEGACY_MIN
 * sits in the gap between them, so a single field-two packet settles it.
 * Field one / progressive lines overlap between the conventions (21-539),
 * but zero-based-only line 0-20 still disambiguates unassisted. */
typedef enum { LINE_MODE_UNKNOWN = 0, LINE_MODE_ZERO_BASED, LINE_MODE_SDI_LEGACY } line_mode_t;

#define SMPTE274M_PROGRESSIVE_BASE 42
#define SMPTE274M_FIELD1_BASE      21
#define SMPTE274M_FIELD2_BASE      584
#define LINE_MODE_SDI_LEGACY_MIN   570  /* below SMPTE274M_FIELD2_BASE, above any zero-based field line */

static void DetectLineMode(demux_t *p_demux, uint16_t raw_line)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    if (p_sys->line_mode != LINE_MODE_UNKNOWN)
        return;

    if (raw_line < SMPTE274M_FIELD1_BASE)
    {
        p_sys->line_mode = LINE_MODE_ZERO_BASED;
        msg_Info(p_demux, "detected zero-based line numbering (ST 2110-20 spec convention; saw line %u)", raw_line);
    }
    else if (raw_line >= LINE_MODE_SDI_LEGACY_MIN)
    {
        p_sys->line_mode = LINE_MODE_SDI_LEGACY;
        msg_Info(p_demux, "detected SDI-legacy (SMPTE 274M raw raster) line numbering "
                  "(common on SDI-to-IP gateways; saw line %u)", raw_line);
    }
    /* else: still in the 21-569 overlap zone, keep waiting */
}

/* Writes each declared line segment into the packed frame buffer. Every
 * segment is bounds-checked against height/stride before the memcpy: this
 * data comes straight off the wire and must never drive an out-of-bounds
 * write. Segments that fail validation are dropped and counted (not logged
 * per-segment -- see the i_drop_* counters and their summary log in
 * FinalizeFrame); the rest of the frame is still assembled best-effort.
 * Segments seen before the line-numbering convention is determined (see
 * DetectLineMode) are also dropped as i_drop_line_range, since they can't
 * be placed correctly yet -- this only costs the first field or so.
 *
 * In interlace mode the two fields are woven into one full-height buffer
 * as actual_line = field_relative_line*2+F, matching how VLC expects a
 * full picture. */
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
            DetectLineMode(p_demux, lines[i].line);

            unsigned field_lines = p_sys->b_interlace ? p_sys->i_height / 2 : p_sys->i_height;
            unsigned base = 0;
            if (p_sys->line_mode == LINE_MODE_SDI_LEGACY)
                base = p_sys->b_interlace
                     ? (lines[i].field2 ? SMPTE274M_FIELD2_BASE : SMPTE274M_FIELD1_BASE)
                     : SMPTE274M_PROGRESSIVE_BASE;

            if (p_sys->line_mode == LINE_MODE_UNKNOWN
             || lines[i].line < base || (unsigned)lines[i].line - base >= field_lines)
            {
                p_sys->i_drop_line_range++;
                pos += seg_len;
                continue;
            }
            unsigned field_relative_line = (unsigned)lines[i].line - base;

            unsigned actual_line = p_sys->b_interlace
                                  ? field_relative_line * 2 + (lines[i].field2 ? 1 : 0)
                                  : field_relative_line;
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

/* Bump this string whenever st2110.c changes, and check for it in the VLC
 * log (Tools/View > Messages) as the very first thing after "using
 * access_demux module st2110" -- this build has repeatedly been debugged
 * against stale .dll copies, so this removes all doubt about which build
 * is actually running. */
#define ST2110_BUILD_MARKER "st2110 build: recv-thread-arch(no-pf_demux)+auto-detect-lineno"

static int Open(vlc_object_t *p_this)
{
    demux_t *p_demux = (demux_t *)p_this;

    msg_Info(p_demux, ST2110_BUILD_MARKER);

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
    /* No SO_RCVTIMEO here: ReceiveThread() times out receive waits itself
     * via select(), which needs no socket-level timeout configured. */

    p_sys->i_stride_packed = (size_t)(p_sys->i_width / 2) * 5;
    p_sys->i_buf_size      = p_sys->i_stride_packed * (size_t)p_sys->i_height;
    /* calloc, not malloc: the first frame accumulated has no prior
     * ResetFrameBuffer() call to zero it, so any line never received
     * before the first flush would otherwise show as uninitialized
     * heap garbage instead of black. */
    p_sys->p_buf           = calloc(1, p_sys->i_buf_size);
    p_sys->p_line_filled   = calloc(p_sys->i_height, sizeof(bool));
    if (!p_sys->p_buf || !p_sys->p_line_filled)
        goto error;
    p_sys->i_last_no_frame_log = mdate();

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

    /* No pf_demux: ReceiveThread runs on its own and pushes to es_out_Send()
     * directly, matching modules/access/rtp/rtp.c. */
    p_demux->pf_demux   = NULL;
    p_demux->pf_control = Control;

    if (vlc_clone(&p_sys->thread, ReceiveThread, p_demux, VLC_THREAD_PRIORITY_INPUT))
    {
        msg_Err(p_demux, "failed to start receive thread");
        goto error;
    }
    p_sys->thread_ready = true;

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

    /* Stop and join the thread before touching anything it might still be
     * using (the socket, the frame buffer, es_out). */
    if (p_sys->thread_ready)
    {
        vlc_cancel(p_sys->thread);
        vlc_join(p_sys->thread, NULL);
    }

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

/* Runs for the lifetime of the stream (vlc_clone()'d from Open(), stopped
 * via vlc_cancel()+vlc_join() in Close()): reads packets, and whenever one
 * full frame is assembled (marker bit, or for progressive a timestamp
 * change revealing the previous frame's marker was lost), pushes it with
 * es_out_Send() directly -- there is no caller to return frames to one at a
 * time. Matches modules/access/rtp/rtp.c's rtp_dgram_thread(): pf_demux is
 * not used for a live network source here (see Open()).
 *
 * Owns the socket I/O directly (select() then recv()) rather than going
 * through net_Read(), for the same reason rtp_dgram_thread() uses raw
 * poll()+recvmsg(): a plain OS-level timeout has no VLC-internal machinery
 * to second-guess. vlc_savecancel()/vlc_restorecancel() bracket each
 * iteration's processing (mirroring rtp_dgram_thread()), so a pending
 * vlc_cancel() is only acted on while blocked in select(), never mid-frame. */
static void *ReceiveThread(void *data)
{
    demux_t     *p_demux = data;
    demux_sys_t *p_sys   = p_demux->p_sys;
    uint8_t pkt[9000];

    for (;;)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(p_sys->fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int r = select(p_sys->fd + 1, &rfds, NULL, NULL, &tv);

        int canc = vlc_savecancel();

        if (r == 0)
        {
            p_sys->i_idle_polls++;
            /* log roughly every 10s of total silence, not every 1s poll,
             * to stay well clear of the message-flood failure mode this
             * module hit before (see WriteLines). */
            if (p_sys->i_idle_polls % 10 == 1)
                msg_Warn(p_demux, "no data received in %us on %s:%d (SSM source \"%s\") "
                          "-- check multicast routing/PIM-SSM reachability from this host",
                          p_sys->i_idle_polls, p_sys->psz_group, p_sys->i_port,
                          (p_sys->psz_source && *p_sys->psz_source) ? p_sys->psz_source : "(none/ASM)");
            vlc_restorecancel(canc);
            continue;
        }
        if (r < 0)
        {
            vlc_restorecancel(canc);
            break; /* socket error */
        }

        p_sys->i_idle_polls = 0;

        ssize_t n = recv(p_sys->fd, (char *)pkt, sizeof(pkt), 0);
        if (n < 0)
        {
            /* select() said readable but the actual read still failed: a
             * genuine socket error, not a timeout (select() already owns
             * all timeout handling above). */
            vlc_restorecancel(canc);
            break;
        }
        if (n == 0)
        {
            vlc_restorecancel(canc);
            continue;
        }

        p_sys->i_pkts_total++;

        uint32_t       ts = 0;
        bool           marker = false;
        const uint8_t *payload = NULL;
        size_t         payload_len = 0;
        bool rtp_ok = ParseRTP(pkt, (size_t)n, &ts, &marker, &payload, &payload_len);
        if (!rtp_ok)
            p_sys->i_pkts_rtp_fail++;

        line_hdr_t     lines[MAX_LINE_HDRS];
        unsigned       n_lines = 0;
        const uint8_t *pixels = NULL;
        size_t         pixels_len = 0;
        bool lines_ok = rtp_ok
                       && ParseLineHeaders(payload, payload_len, lines, &n_lines, &pixels, &pixels_len);
        if (rtp_ok && !lines_ok)
            p_sys->i_pkts_line_fail++;
        if (lines_ok)
            p_sys->i_pkts_no_frame++;

        /* Every received packet is now accounted for in exactly one of
         * these counters (or a completed frame reset them all -- see the
         * es_out_Send call sites below), so this fires every ~5s no matter
         * which stage is actually the problem: silently discarding packets
         * before this point (as the old single-counter version did) is
         * exactly the failure mode that hid a systematic parse rejection. */
        mtime_t now = mdate();
        if (now - p_sys->i_last_no_frame_log > INT64_C(5000000) /* 5s */)
        {
            msg_Warn(p_demux, "last ~5s: %u packets received, %u failed RTP header parse, "
                      "%u failed line-header parse, %u parsed OK but no frame completed yet",
                      p_sys->i_pkts_total, p_sys->i_pkts_rtp_fail,
                      p_sys->i_pkts_line_fail, p_sys->i_pkts_no_frame);
            p_sys->i_pkts_total = p_sys->i_pkts_rtp_fail = p_sys->i_pkts_line_fail
                                 = p_sys->i_pkts_no_frame = 0;
            p_sys->i_last_no_frame_log = now;
        }

        if (!lines_ok)
        {
            vlc_restorecancel(canc);
            continue;
        }

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
                        p_sys->i_pkts_total = p_sys->i_pkts_rtp_fail = p_sys->i_pkts_line_fail
                                             = p_sys->i_pkts_no_frame = 0;
                        es_out_Send(p_demux->out, p_sys->es, cur);
                    }
                    /* if cur is NULL: frame dropped (too few lines received) */
                }
            }
            vlc_restorecancel(canc);
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
            p_sys->i_pkts_total = p_sys->i_pkts_rtp_fail = p_sys->i_pkts_line_fail
                                 = p_sys->i_pkts_no_frame = 0;
            es_out_Send(p_demux->out, p_sys->es, to_send);
        }

        if (marker)
        {
            block_t *cur = FinalizeFrame(p_demux);
            ResetFrameBuffer(p_sys);
            p_sys->b_frame_open = false;
            if (cur)
            {
                p_sys->i_pkts_total = p_sys->i_pkts_rtp_fail = p_sys->i_pkts_line_fail
                                     = p_sys->i_pkts_no_frame = 0;
                es_out_Send(p_demux->out, p_sys->es, cur);
            }
            /* if cur is NULL: frame dropped (too few lines received) */
        }

        vlc_restorecancel(canc);
    }
    return NULL;
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
            /* false: this is a live real-time source (matches modules/access/dvb
             * and dtv in real VLC), not a file the core can pull ahead of and
             * pace against PTS itself -- we already only deliver data as fast
             * as the network provides it. */
            *va_arg(args, bool *) = false;
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
