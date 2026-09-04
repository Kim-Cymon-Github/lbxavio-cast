//---------------------------------------------------------------------------
/**
 * @file cast_core.h
 * @brief Receive core: channels, frame ring, refcounts, packet tap.
 *
 * Pure state, no AVIO types -- avio_cast_main.cpp maps the driver vtable
 * onto this, the way avio_play_main maps onto play_core. One CAST_CORE per
 * driver process; channels are its devices.
 *
 * ## The ring is not an optimisation
 *
 * A frame is not just drawn. Algorithms run on it, and some of them hold a
 * picture for several frames (motion, calibration, tracking). The avio
 * contract makes that explicit: a delivered frame is valid until the next
 * OnFrame for the same device, and a host that needs it longer calls
 * RefFrame / UnrefFrame. So every channel owns a ring of buffers with a
 * refcount each, exactly as avio-v4l2 does -- a single "latest frame" slot,
 * which is all a viewer needs, would be overwritten under a reader.
 *
 * Depth follows avio-v4l2's NUM_VIDBUF, which is 8. This is not a round
 * number picked for comfort: the ring was once quietly narrowed 8 -> 3 and
 * that alone produced the FPS oscillation chased through lbx-gfx
 * plan-frame-export-seam.md SS15. Held buffers leave rotation, so the depth
 * has to cover (display hold) + (algorithm holds) + (decode in flight)
 * without starving the decoder.
 *
 * Refcount lifecycle, mirroring avio_v4l2.cpp:
 *
 *   Grab: take newest ready slot -> ref_cnt = 1 (driver-implicit)
 *         [recording tap, when it exists: ++ref_cnt, submit]
 *         OnFrame(dev, &ev)          <- host may RefFrame here
 *         --ref_cnt; if <= 0 -> slot returns to the free list
 *
 * When every slot is held the driver must say so rather than stall
 * silently; avio-v4l2 fires OnError ("device lost") in that case and this
 * one reports a starved link the same way.
 *
 * ## Drain to latest -- and why the stale frames are not simply dropped
 *
 * The host calls Grab once per loop. If the decode thread produced more
 * than one picture since then, the older ones are stale for display and
 * Grab returns them to the ring without firing OnFrame, so a backlog cannot
 * accumulate into apparent frame reversal (avio_v4l2.cpp does exactly this).
 *
 * But "stale for display" is not "unwanted". avio-v4l2 feeds the dropped
 * frames to the encoder anyway -- display takes the newest, recording takes
 * all of them. The same split applies here the moment cast learns to record,
 * so the drain path keeps that fork visible from the start instead of
 * hard-coding a drop.
 *
 * ## Recording is a mux here, not an encode
 *
 * Nothing on this path re-encodes. The board already tees the recorder's own
 * H.264/H.265 packets onto the wire, so a PC-side recording is those packets
 * written to a container -- which is also why the channel thread keeps the
 * packet reachable (@ref CAST_PACKET_FN) and does not throw it away once the
 * decoder has consumed it. Recording itself is deliberately not built yet;
 * this is the seam it will attach to, and leaving it out of the data flow
 * now would mean rebuilding the flow later.
 *
 * ## Image identity
 *
 * Each ring slot owns a persistent LBX_IMAGE that is never reallocated and
 * never wholesale-copied. Two reasons, both from the avio texture contract:
 * the image address is the identity key the gfx backend caches GPU resources
 * by, and planes[i].texture belongs to that backend -- a struct copy would
 * clear it every frame and drop the cached texture. Only the mutable fields
 * move, and `sequence` is bumped to mark new content.
 */
//---------------------------------------------------------------------------
#ifndef cast_coreH
#define cast_coreH

#include "lbx_type.h"
#include "image/lbx_image.h"
#include "cast_dec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Ring depth per channel. See the file comment -- 8 is avio-v4l2's
 *  NUM_VIDBUF, and the value a documented regression argues against
 *  lowering. */
#define CAST_RING_DEPTH (8)

/**
 * @brief Encoded packet as it arrived, before decode.
 *
 * Handed to whoever taps the stream (a future recorder muxes it verbatim).
 * Borrowed for the duration of the call.
 *
 * @param key True on a keyframe -- a muxer starts a file here.
 */
typedef void (*CAST_PACKET_FN)(void *user, i32_t ch, const u8_t *pkt,
                               u32_t size, i64_t pts_ms, bool key);

struct tagCAST_CORE;
typedef struct tagCAST_CORE CAST_CORE;

/** Builds the core and its link with defaults (127.0.0.1,
 *  AVIO_CAST_BASE_PORT). Opens no channel. */
CAST_CORE *cast_core_create(void);
void       cast_core_destroy(CAST_CORE *self);

/** @p host is usually "127.0.0.1" -- adb forward does the rest.
 *  @p decoder_pref is the cast.decoder knob ("avcodec" | "gst" | NULL). */
void  cast_core_config(CAST_CORE *self, const char *host, i32_t base_port,
                       const char *decoder_pref);

/** The transport, for the status panel and for Do("adb.*"). */
struct tagCAST_LINK *cast_core_link(CAST_CORE *self);

/** Starts a channel's link and decode thread. Returns immediately; the
 *  thread retries on its own, so starting before the board streams is fine. */
i32_t cast_core_open_ch(CAST_CORE *self, i32_t ch);
void  cast_core_close_ch(CAST_CORE *self, i32_t ch);

/**
 * @brief Takes the newest ready slot for @p ch, draining staler ones.
 *
 * @param timeout_ms 0 = non-blocking single poll, >0 = bounded wait.
 * @return The slot's image with one implicit ref held, or NULL on a miss.
 *         The caller fires OnFrame and then releases through
 *         @ref cast_core_unref.
 */
LBX_IMAGE *cast_core_take(CAST_CORE *self, i32_t ch, i32_t timeout_ms);

void  cast_core_ref(CAST_CORE *self, i32_t ch, const LBX_IMAGE *img);
void  cast_core_unref(CAST_CORE *self, i32_t ch, const LBX_IMAGE *img);

/**
 * @brief Has this slot's content changed since it was last uploaded?
 *
 * The GPU upload is the driver's to make (host_api->gfx), on the thread that
 * owns the GL context -- which is the Grab caller, never the decode thread.
 * The bookkeeping is per slot, not per channel: every ring slot carries its
 * own texture, so "already uploaded" is a property of the slot.
 *
 * @return 1 when the driver should Import (texture still 0) or Update.
 */
i32_t cast_core_upload_pending(CAST_CORE *self, i32_t ch, const LBX_IMAGE *img);
/** Records that @p img was uploaded at its current sequence. */
void  cast_core_upload_done(CAST_CORE *self, i32_t ch, const LBX_IMAGE *img);

/**
 * @brief Lists a channel's ring slot images so the driver can release the
 *        GPU resources it had imported (gfx DestroyImage) at Close.
 * @return How many were written to @p out.
 */
i32_t cast_core_slots(CAST_CORE *self, i32_t ch, LBX_IMAGE **out, i32_t max);

/** Installs the encoded-packet tap. NULL removes it. */
void  cast_core_set_packet_tap(CAST_CORE *self, CAST_PACKET_FN fn, void *user);

#ifdef __cplusplus
}
#endif

#endif /* cast_coreH */
