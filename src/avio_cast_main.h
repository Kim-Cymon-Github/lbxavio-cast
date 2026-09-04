//---------------------------------------------------------------------------
/**
 * @file avio_cast_main.h
 * @brief Live avio source that receives the board's cast.* TCP streams.
 *
 * The board encodes each camera once for recording and tees the same packets
 * onto a TCP port (drv/avsink-mpp, avsink_cast.h). This driver is the other
 * end of that wire: one device per channel, each opening its own port. To a
 * host it is an ordinary live avio source -- indistinguishable from
 * avio-v4l2 except that the cable is USB.
 *
 *      board: capture -> MPP encode -+-> MKV file   (rec)
 *                                    +-> TCP socket (cast)
 *                                              |
 *      PC:                       avio-cast <---+  -> LBX_IMAGE (YUV planes)
 *
 * The receive and decode core comes from tool/clink (clink_cast.cpp), which
 * had it first. clink keeps the control plane -- it decides *when* to stream
 * -- and becomes the first consumer of this driver instead of carrying a
 * private copy.
 *
 * ## Layer map
 *
 *   avio_cast_main   AVIO vtable -> core. The only file that knows AVIO
 *                    types, exactly as avio_play_main maps onto play_core.
 *   avio_cast_ui     ProcessUI panel: per-channel link state, fps, errors.
 *   cast_core        Ring, refcounts, drain-to-latest, packet tap. Pure
 *                    state -- no AVIO types.
 *   cast_link        Transport: adb forward, per-channel TCP demux thread.
 *   cast_dec         Decoder backend seam (avcodec now, gst/mpp later).
 *
 * ## Division of responsibility against the host
 *
 * Transport is the driver's, policy is the host's.
 *
 *  - `adb forward` is set up here. It is not a policy choice but the way a
 *    TCP port on a USB-attached board is reached at all, and a missing
 *    forward is indistinguishable from a dead board (connect fails, no
 *    picture, nothing in the log). The side that knows the transport owes
 *    that diagnosis.
 *  - Turning channels on and off is NOT done here. Which channels to stream
 *    is a bandwidth decision -- ADB over USB2 measures ~205 Mbit/s -- and
 *    only the host knows what is being looked at. The host sends its own
 *    control command (clink_send `s1`/`s0`) as it does today.
 *
 * ## Port layout
 *
 * From avsink `cast.caps` (2026-09-02): slot 0 is the board's own display,
 * slots 1.. are cameras in registration order, and the port is
 * base_port + slot. Slot 0 moved to the front because "cameras first,
 * display last" put the display on a port that differed between the 4- and
 * 8-camera profiles.
 *
 * Open() takes "cam0".."cam7" and "screen" -- the vocabulary a host already
 * speaks (svmdemo opens "play0".."play7" on its play slot).
 *
 * ## Scope
 *
 * PC only. The board has no reason to receive its own cast, and libav* is a
 * PC-side dependency. Colour conversion is not done here: planes stay
 * I420/NV12 and the GPU converts (LBX_GFX_SERVICES CPU YUV import), which is
 * 1.5 bytes per pixel on the bus instead of 4.
 */
//---------------------------------------------------------------------------
#ifndef avio_cast_mainH
#define avio_cast_mainH

#include "lbx_core.h"
#include "intf/lbx_intf_avio.h"

#ifdef _WIN32
#  ifdef AVIO_CAST_DLL
#    define AVIO_CAST_EXPORT __declspec(dllexport)
#  else
#    define AVIO_CAST_EXPORT __declspec(dllimport)
#  endif
#else
#  define AVIO_CAST_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Default first port. Moved from 5560 (2026-08-28) because Windows
 *  Hyper-V/WinNAT reserves 5458~5657 on some hosts, which breaks the local
 *  bind of `adb forward`. 25xxx sits outside the observed clusters. */
#define AVIO_CAST_BASE_PORT (25560)
/** The screen plus eight cameras. */
#define AVIO_CAST_MAX_CH    (9)
/** The board's own display -- feeds a main view, not a camera tile. */
#define AVIO_CAST_SCREEN_CH (0)
/** First camera slot; camera k (0-based) sits on base_port + this + k. */
#define AVIO_CAST_CAM_FIRST (1)

AVIO_CAST_EXPORT version_t   LBX_API avio_cast_version(void);
AVIO_CAST_EXPORT const char *LBX_API avio_cast_version_str(void);

/** Module entry, as required by LBX_MODULE_INTERFACE_Open. */
AVIO_CAST_EXPORT var_t LBX_API lbx_avio_entry(LBX_MODULE_INTERFACE *intf,
                                              LbxModuleRequest request, var_t opt);

AVIO_CAST_EXPORT i32_t LBX_API init_avio_cast_interface(LBX_MODULE_INTERFACE *intf,
                                                        fourcc_t intf_id,
                                                        u32_t intf_version);

/** The live core, so the driver's own UI can read link state. NULL before
 *  mrInitialize and after mrTerminate. Module-internal, not exported. */
struct tagCAST_CORE *avio_cast_core(void);

#ifdef __cplusplus
}
#endif

#endif /* avio_cast_mainH */
