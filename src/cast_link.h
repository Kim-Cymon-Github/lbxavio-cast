//---------------------------------------------------------------------------
/**
 * @file cast_link.h
 * @brief Transport: adb forwards, and one TCP demux thread per channel.
 *
 * Everything this driver talks to lives on the board. Over USB none of it is
 * reachable until `adb forward tcp:P tcp:P` exists, and a missing forward
 * looks exactly like a dead board -- connect fails, no picture, nothing in
 * the log. So the forwards are put in place here, before the first connect
 * attempt, and refreshed while a link is down (a board reboot drops them).
 * adb is driven as a child process: there is no client library, and the CLI
 * output format is effectively its interface. Only two shapes are parsed,
 * both ancient and stable, and misreading either only costs one redundant
 * `adb forward`, which is idempotent. Moved from tool/clink (clink_adb.cpp).
 *
 * One thread per channel, because opening a stream blocks until it
 * identifies itself and a stalled channel must not hold up the other eight.
 * The thread demuxes what the board writes and hands each block to two
 * sinks: the decoder, and -- if one is installed -- the raw packet tap that
 * a recorder will use. It decides no policy: it reconnects while its channel
 * is open and stops when it is closed. Failure to connect is not an error
 * but "not yet" -- the board may simply not have been told to stream.
 *
 * Nothing here turns streaming on or off. That command travels on the
 * host's own control link; see avio_cast_main.h.
 */
//---------------------------------------------------------------------------
#ifndef cast_linkH
#define cast_linkH

#include "lbx_type.h"
#include "cast_dec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How often the adb path is allowed to shell out, in ms. */
#define CAST_ADB_RETRY_MS (5000)

typedef struct tagCAST_ADB {
    i32_t enabled;      /**< 0 disables every call                        */
    char  exe[260];     /**< resolved adb path, "" = not found            */
    char  serial[64];   /**< device the forwards are made on, "" = none   */
    i32_t n_dev;        /**< devices in "device" state at the last probe  */
    i32_t n_fwd;        /**< forwards this process put in place           */
    char  note[160];    /**< last outcome, for the status line            */
} CAST_ADB;

/** Locates adb: the AVIO_CAST_ADB environment variable, then PATH, then
 *  ANDROID_HOME / ANDROID_SDK_ROOT / the default SDK location. Opens
 *  nothing and never fails -- a missing adb leaves @ref CAST_ADB::exe empty
 *  and every later call a no-op. */
void  cast_adb_init(CAST_ADB *self);
void  cast_adb_set_exe(CAST_ADB *self, const char *path);
void  cast_adb_set_serial(CAST_ADB *self, const char *serial);
i32_t cast_adb_available(const CAST_ADB *self);
/** Counts attached devices and pins the serial to pass as `-s`. */
i32_t cast_adb_probe(CAST_ADB *self);
/** Adds the missing forwards in [base, base + count). Existing ones are
 *  left alone: re-adding makes adb rebuild the listener, which drops
 *  whatever connection was open through it. */
i32_t cast_adb_forward_range(CAST_ADB *self, i32_t base, i32_t count);
/** probe + forward_range for the cast ports. The control port is the
 *  host's, not ours. */
i32_t cast_adb_autoforward(CAST_ADB *self, i32_t base, i32_t count);

/** Per-channel link state, for display only. */
typedef struct tagCAST_LINK_STAT {
    volatile i32_t connected;   /**< 1 while the stream is open           */
    volatile i32_t want_open;   /**< 1 = thread should keep reconnecting  */
    volatile i32_t running;     /**< 1 while the thread is alive          */
    u32_t          frames;      /**< frames decoded so far                */
    i32_t          width, height;
    char           codec[16];   /**< "hevc" | "h264" -- diagnostics       */
    char           url[128];    /**< "tcp://127.0.0.1:25560"              */
    char           err[128];    /**< last failure, "" when fine           */
} CAST_LINK_STAT;

/** A decoded frame, borrowed for the duration of the call. Fired on the
 *  channel thread; the core copies it into a ring slot under its own lock. */
typedef void (*CAST_LINK_FRAME_FN)(void *user, i32_t ch,
                                   const CAST_DEC_FRAME *frame);

/** The encoded packet exactly as it arrived, before decode -- what a
 *  recorder muxes verbatim. Borrowed for the duration of the call. */
typedef void (*CAST_LINK_PKT_FN)(void *user, i32_t ch, const u8_t *pkt,
                                 u32_t size, i64_t pts_ms, bool key);

struct tagCAST_LINK;
typedef struct tagCAST_LINK CAST_LINK;

CAST_LINK *cast_link_create(void);
void       cast_link_destroy(CAST_LINK *self);

/** @p host is usually "127.0.0.1" -- adb forward does the rest.
 *  @p decoder_pref is the cast.decoder knob ("avcodec" | "gst" | NULL). */
void  cast_link_config(CAST_LINK *self, const char *host, i32_t base_port,
                       const char *decoder_pref);
void  cast_link_set_sinks(CAST_LINK *self, CAST_LINK_FRAME_FN on_frame,
                          CAST_LINK_PKT_FN on_packet, void *user);

/** Starts (or restarts) a channel's thread. Returns immediately -- the
 *  connect happens on the thread, which retries on its own, so starting
 *  before the board streams is fine. */
i32_t cast_link_start(CAST_LINK *self, i32_t ch);
/** Stops one channel and joins its thread. Safe on a channel never started. */
void  cast_link_stop(CAST_LINK *self, i32_t ch);

const CAST_LINK_STAT *cast_link_stat(const CAST_LINK *self, i32_t ch);
CAST_ADB             *cast_link_adb(CAST_LINK *self);

#ifdef __cplusplus
}
#endif

#endif /* cast_linkH */
