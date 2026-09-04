//---------------------------------------------------------------------------
/**
 * @file cast_dec.h
 * @brief Decoder backend seam -- a deliberate twin of avio-play's play_dec.h.
 *
 * The two drivers face the same problem from opposite ends of the same
 * pipe. avio-play reads length-prefixed NALs out of a recorded MKV; this one
 * reads the identical bytes off a TCP socket, because the board tees the
 * recorder's packets rather than encoding a second time. The packet format,
 * the codec ids, the CodecPrivate blob and the frame layouts are therefore
 * the same, and so is the useful abstraction: open / send / receive / flush
 * / close, with the backend reporting the layout it naturally produces
 * ('I420' from software decode, 'NV12' from hardware).
 *
 * That makes this file duplication with intent, not by accident. The
 * contract is copied verbatim from play_dec.h so the two can be merged
 * without a translation step, and the merge is the follow-up: once cast has
 * run and the contract has survived a second consumer, the seam belongs in
 * lbx where both drivers include one copy. Promoting it before that would
 * pin an unproven interface and drag the whole deploy chain along for it.
 * Until then, changes here and in play_dec.h travel together.
 *
 * The DMA-BUF descriptor is carried even though the PC path never fills it.
 * Dropping it would make the eventual merge a contract change rather than a
 * file move, and a board-side cast receiver (mppvideodec) would want it back.
 */
//---------------------------------------------------------------------------
#ifndef cast_decH
#define cast_decH

#include "lbx_type.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One decoded frame, borrowed from the backend. Valid until the next
 *  receive/flush/close call on the same decoder. */
typedef struct tagCAST_DEC_FRAME {
    i32_t       width, height;
    fourcc_t    format;      /* 'I420' (3 planes) | 'NV12' (2 planes)     */
    const u8_t *plane[3];    /* Y, U|UV, V (NULL when absent)             */
    i32_t       stride[3];   /* bytes per row, per plane                  */
    i64_t       pts_ms;      /* echoed from the packet that produced it   */
    i32_t       full_range;  /* 1 = full-range luma (JPEG range)          */
    i32_t       bt709;       /* 1 = BT.709 matrix (stream VUI), else 601  */

    /* Zero-copy descriptor, unused on the PC path. See the file comment. */
    i32_t       n_dma;       /* 0 = CPU frame; else plane count           */
    i32_t       dma_fd[3];
    u32_t       dma_offset[3];
    u32_t       dma_pitch[3];
    u64_t       dma_modifier;
} CAST_DEC_FRAME;

struct tagCAST_DECODER;

typedef struct tagCAST_DEC_VTBL {
    const char *name;        /* "avcodec" | "gst" -- diagnostics          */

    /** Configure from the stream's track: @p codec_id is the Matroska
     *  CodecID string, @p priv the CodecPrivate blob (avcC/hvcC). */
    i32_t (*Open)(struct tagCAST_DECODER *self, const char *codec_id,
                  const u8_t *priv, u32_t priv_len, i32_t w, i32_t h);

    /** Feed one block payload (length-prefixed NALs).
     *  @return 1 accepted; 0 pipeline full, packet NOT consumed -- Receive
     *  first, then retry the same packet; <0 backend error. */
    i32_t (*Send)(struct tagCAST_DECODER *self, const u8_t *pkt, u32_t size,
                  i64_t pts_ms);

    /** @return 1 = @p out filled (borrow), 0 = need more input, <0 error. */
    i32_t (*Receive)(struct tagCAST_DECODER *self, CAST_DEC_FRAME *out);

    /** Drop buffered state -- a reconnect restarts from a keyframe. */
    void  (*Flush)(struct tagCAST_DECODER *self);

    void  (*Close)(struct tagCAST_DECODER *self);
} CAST_DEC_VTBL;

typedef struct tagCAST_DECODER {
    const CAST_DEC_VTBL *vt;
    void *impl;              /* backend-private state                     */
} CAST_DECODER;

/** Binds the libavcodec backend. Inert stub without CONFIG_AVCODEC_DECODER. */
i32_t cast_dec_create_avcodec(CAST_DECODER *self);

/** Binds the gstreamer backend. Inert stub without CONFIG_GST_DECODER. */
i32_t cast_dec_create_gst(CAST_DECODER *self);

/**
 * Creates the preferred available backend. @p pref is the cast.decoder knob:
 * "avcodec" | "gst" pin one, NULL/""/"auto" probes in that order.
 */
i32_t cast_dec_create(CAST_DECODER *self, const char *pref);

#ifdef __cplusplus
}
#endif

#endif /* cast_decH */
