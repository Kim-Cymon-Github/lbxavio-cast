//---------------------------------------------------------------------------
// avio_cast_main.cpp — avio-cast 드라이버 본체 (AVIO v0.4 → cast_core 배선).
//
// 보드가 cast 로 티핑하는 카메라 스트림을 PC 에서 라이브 소스로 받는다.
//
//   Open("cam0".."cam7" | "screen")  — 채널 = 디바이스. 포트는 base_port+슬롯.
//   Do("cast.*")                     — caps/config/status. **켜고 끄기는 없다**
//                                      (호스트 제어 링크 몫 — avio_cast_main.h).
//   Grab                             — 링에서 최신 프레임 한 장 → OnFrame.
//
// 드라이버는 GL/VK 를 모른다 — GPU 업로드는 host_api->gfx 경유다. 프레임은
// I420/NV12 평면 그대로 올라간다.
//---------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// version.txt 를 lbx_type.h(→lbx_version.h) 보다 먼저 include (임베드 매크로 활성화)
#define LBX_MODULE_NAME "avio-cast"
#include "version.txt"
#include "avio_cast_main.h"
#include "avio_cast_ui.h"
#include "cast_core.h"
#include "cast_link.h"
#include "system/lbx_log.h"
#include "intf/lbx_intf.h"
#include "lbx_core.h"      // tick_now()

// 빌드 산출물(.dll)에 "로딩 없이" 읽을 수 있는 버전 정보를 박는다.
LBX_EMBED_VERSION_INFO();

version_t LBX_API avio_cast_version(void)
{
    return version_(VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, BUILD_NUMBER);
}

const char *LBX_API avio_cast_version_str(void)
{
    return lbx_version_info;
}

const char module_name[] = "LBX-AVIO-CAST";
const char module_description[] = "Board cast stream live source for SVM SDK";

//---------------------------------------------------------------------------
// 드라이버 전역 — 코어는 드라이버당 1개(채널이 디바이스다)
//---------------------------------------------------------------------------

static CAST_CORE          *s_core     = NULL;
static const LBX_HOST_API *s_host_api = NULL;
static LBX_AVIO_DRIVER    *s_drv      = NULL;

struct tagCAST_CORE *avio_cast_core(void)
{
    return s_core;
}

/** 한 디바이스 = 한 채널. dev_id 는 호스트 소유라 그대로 들고 있지 않고
 *  복사한다 — 이벤트에 되돌려 줄 문자열이 여기서 죽으면 안 된다. */
typedef struct tagCAST_DEV {
    char  id[24];
    i32_t ch;
    void *user;
} CAST_DEV;

/** "cam0".."cam7" → 슬롯 1.., "screen" → 슬롯 0. 그 외는 -1.
 *  호스트가 이미 쓰는 어휘라 새로 배울 것이 없다(svmdemo 의 "play0"..). */
static i32_t channel_of(const char *dev_id)
{
    if (dev_id == NULL) { return -1; }
    if (strcmp(dev_id, "screen") == 0) { return AVIO_CAST_SCREEN_CH; }
    if (strncmp(dev_id, "cam", 3) == 0 && dev_id[3] >= '0' && dev_id[3] <= '9'
        && dev_id[4] == '\0') {
        const i32_t k = dev_id[3] - '0';
        const i32_t ch = AVIO_CAST_CAM_FIRST + k;
        if (ch < AVIO_CAST_MAX_CH) { return ch; }
    }
    return -1;
}

//---------------------------------------------------------------------------
// 디바이스 수명
//---------------------------------------------------------------------------

static LBX_HANDLE LBX_API open_cast_device(const char *dev_id, const LBX_IMAGE *fmt,
                                           var_t opt, void *user)
{
    CAST_DEV *d;
    i32_t     ch;

    (void)fmt;   /* 포맷은 스트림이 정한다 — 요청 계약이 아니다 */

    ch = channel_of(dev_id);
    if (ch < 0) {
        Err_("avio-cast: 모르는 디바이스 '%s' (cam0..cam%d, screen)",
             (dev_id != NULL) ? dev_id : "(null)",
             AVIO_CAST_MAX_CH - AVIO_CAST_CAM_FIRST - 1);
        var_drop(&opt);
        return NULL;
    }
    if (s_core == NULL) {
        Err_("avio-cast: 코어 미초기화");
        var_drop(&opt);
        return NULL;
    }

    /* Open opt 로 오는 것들 — 호스트가 config 를 따로 부르지 않아도 되게. */
    {
        lbx::var o(opt);   /* move — 여기서 소유권을 받는다 */
        const char *host = NULL;
        i32_t base = 0;
        const char *decoder = NULL;
        UString hs = o["host"].str();
        UString ds = o["decoder"].str();
        if (!hs.IsEmpty()) { host = hs.c_str(); }
        if (!ds.IsEmpty()) { decoder = ds.c_str(); }
        base = (i32_t)o["base_port"].i32(0);
        if (host != NULL || base > 0 || decoder != NULL) {
            cast_core_config(s_core, host, base, decoder);
        }
    }

    d = (CAST_DEV *)calloc(1, sizeof(CAST_DEV));
    if (d == NULL) { return NULL; }
    snprintf(d->id, sizeof(d->id), "%s", dev_id);
    d->ch   = ch;
    d->user = user;

    if (!cast_core_open_ch(s_core, ch)) {
        free(d);
        return NULL;
    }
    Info_("avio-cast: %s → ch%d 열림", d->id, ch);
    return (LBX_HANDLE)d;
}

static void LBX_API close_cast_device(LBX_HANDLE dev)
{
    CAST_DEV *d = (CAST_DEV *)dev;
    if (d == NULL) { return; }
    if (s_core != NULL) { cast_core_close_ch(s_core, d->ch); }
    free(d);
}

//---------------------------------------------------------------------------
// Grab
//---------------------------------------------------------------------------

static i32_t LBX_API grab_cast_device(LBX_HANDLE dev, i32_t timeout_ms)
{
    CAST_DEV  *d = (CAST_DEV *)dev;
    LBX_IMAGE *img;

    if (d == NULL || s_core == NULL) { return -1; }

    img = cast_core_take(s_core, d->ch, timeout_ms);
    if (img == NULL) { return 0; }   /* 이번 사이클 미잡 — 정상 */

    if (s_drv != NULL && s_drv->OnFrame != NULL) {
        LBX_AVIO_FRAME_EVENT ev;
        memset(&ev, 0, sizeof(ev));
        ev.dev_id = d->id;
        ev.tick   = tick_now();
        ev.user   = d->user;
        ev.image  = img;
        s_drv->OnFrame(dev, &ev);
    }
    /* driver-implicit ref 해제. 호스트가 OnFrame 안에서 RefFrame 했으면
     * 그 ref 가 남아 슬롯이 링으로 돌아가지 않는다. */
    cast_core_unref(s_core, d->ch, img);
    return 1;
}

/** 이 드라이버는 fd 로 깨울 것이 없다 — 프레임은 디코드 스레드가 만든다.
 *  호스트 디스패처는 timeout 기반 Grab 으로 돈다. */
static i32_t LBX_API get_poll_fd_cast(LBX_HANDLE dev)
{
    (void)dev;
    return -1;
}

static void LBX_API ref_frame_cast(LBX_HANDLE dev, const LBX_IMAGE *frame)
{
    CAST_DEV *d = (CAST_DEV *)dev;
    if (d != NULL && s_core != NULL) { cast_core_ref(s_core, d->ch, frame); }
}

static void LBX_API unref_frame_cast(LBX_HANDLE dev, const LBX_IMAGE *frame)
{
    CAST_DEV *d = (CAST_DEV *)dev;
    if (d != NULL && s_core != NULL) { cast_core_unref(s_core, d->ch, frame); }
}

/** 명시 재시작 — 링크 스레드는 스스로 재접속하므로 보통 필요 없다. 보드를
 *  갈아 끼웠을 때처럼 adb 포워드부터 다시 깔아야 하는 경우를 위한 것. */
static i32_t LBX_API recover_cast(LBX_HANDLE dev)
{
    CAST_DEV *d = (CAST_DEV *)dev;
    if (d == NULL || s_core == NULL) { return -1; }
    cast_core_close_ch(s_core, d->ch);
    return cast_core_open_ch(s_core, d->ch) ? 0 : -1;
}

//---------------------------------------------------------------------------
// Do
//---------------------------------------------------------------------------

var_t LBX_API do_command(LBX_HANDLE dev, const char *cmd, var_t args)
{
    CAST_DEV *d = (CAST_DEV *)dev;

    if (cmd == NULL) { var_drop(&args); return var_err_(LBX_ERROR_INVALID_VALUE, "no command"); }

    if (strcmp(cmd, "cast.caps") == 0) {
        var_drop(&args);
        var_t r = var_obj_(NULL);
        var_obj_add(&r, "max_channels", var_i32_(AVIO_CAST_MAX_CH));
        var_obj_add(&r, "display_slot", var_i32_(AVIO_CAST_SCREEN_CH));
        var_obj_add(&r, "camera_first", var_i32_(AVIO_CAST_CAM_FIRST));
        var_obj_add(&r, "ring_depth",   var_i32_(CAST_RING_DEPTH));
        var_obj_add(&r, "container",    var_str_("mkv", true));
        /* 켜고 끄기는 여기 없다 — 호스트가 자기 제어 링크로 보낸다. 이 키가
         * 그 사실을 계약으로 못박는다(호스트가 헛되이 찾지 않게). */
        var_obj_add(&r, "controls_stream", var_bool_(false));
        return r;
    }
    if (strcmp(cmd, "cast.config") == 0) {
        lbx::var  o(args);
        UString   hs = o["host"].str();
        UString   ds = o["decoder"].str();
        cast_core_config(s_core, hs.IsEmpty() ? NULL : hs.c_str(),
                         (i32_t)o["base_port"].i32(0),
                         ds.IsEmpty() ? NULL : ds.c_str());
        return VAR_OK;
    }
    if (strcmp(cmd, "cast.status") == 0) {
        var_drop(&args);
        var_t r  = var_obj_(NULL);
        var_t cs = var_arr_(NULL);
        CAST_LINK *link = cast_core_link(s_core);
        for (i32_t i = 0; i < AVIO_CAST_MAX_CH; ++i) {
            const CAST_LINK_STAT *st = cast_link_stat(link, i);
            if (st == NULL) { continue; }
            var_t e = var_obj_(NULL);
            var_obj_add(&e, "ch",        var_i32_(i));
            var_obj_add(&e, "connected", var_bool_(st->connected != 0));
            var_obj_add(&e, "frames",    var_u32_(st->frames));
            var_obj_add(&e, "width",     var_i32_(st->width));
            var_obj_add(&e, "height",    var_i32_(st->height));
            var_obj_add(&e, "url",       var_str_(st->url, true));
            var_obj_add(&e, "err",       var_str_(st->err, true));
            var_append(&cs, e);
        }
        var_obj_add(&r, "ch", cs);
        {
            const CAST_ADB *adb = cast_link_adb(link);
            if (adb != NULL) {
                var_obj_add(&r, "adb", var_str_(adb->note, true));
            }
        }
        return r;
    }
    if (strcmp(cmd, "adb.forward") == 0) {
        var_drop(&args);
        CAST_LINK *link = cast_core_link(s_core);
        CAST_ADB  *adb  = cast_link_adb(link);
        return var_i32_(cast_adb_autoforward(adb, AVIO_CAST_BASE_PORT,
                                             AVIO_CAST_MAX_CH));
    }

    (void)d;
    var_drop(&args);
    return var_printf_("Invalid command %s", cmd);
}

//---------------------------------------------------------------------------
// 모듈 entry
//---------------------------------------------------------------------------

i32_t LBX_API init_avio_cast_interface(LBX_MODULE_INTERFACE *intf,
                                       fourcc_t intf_id, u32_t intf_version)
{
    LBX_AVIO_DRIVER *drv = (LBX_AVIO_DRIVER *)intf;
    if (intf_id != LBX_AVIO_DRIVER_ID) {
        Err_("Invalid interface id " FOURCC_VFMT, FOURCC_VARG(intf_id));
        return 0;
    }
    intf->intf_id = intf_id;
    if (intf_version != LBX_AVIO_DRIVER_VERSION) {
        Err_("Interface version mismatch: %X", intf_version);
        return 0;
    }
    intf->intf_version = intf_version;
    drv->Open       = &open_cast_device;
    drv->Close      = &close_cast_device;
    drv->Do         = &do_command;
    drv->Grab       = &grab_cast_device;
    drv->GetPollFd  = &get_poll_fd_cast;
    drv->RefFrame   = &ref_frame_cast;
    drv->UnrefFrame = &unref_frame_cast;
    drv->Recover    = &recover_cast;
    drv->ProcessUI  = &process_ui;
    return 1;
}

var_t LBX_API lbx_avio_entry(LBX_MODULE_INTERFACE *intf,
                             LbxModuleRequest request, var_t opt)
{
    LBX_AVIO_DRIVER *drv = (LBX_AVIO_DRIVER *)intf;
    var_drop(&opt);   // opt 미사용 — move 규약상 소유권 인수 후 즉시 해제
    if (intf == NULL) {
        return var_i32_(LBX_ERROR_INVALID_VALUE);
    }
    if (!LBX_AVIO_DRIVER_IsCompatible(intf)) {
        Err_("AVIO interface mismatch: got id=" FOURCC_VFMT " ver=" LBX_VERSION_VFMT
             ", need id=" FOURCC_VFMT " ver=" LBX_VERSION_VFMT " (major must match).",
             FOURCC_VARG(intf->intf_id), LBX_VERSION_VARG(intf->intf_version),
             FOURCC_VARG(LBX_AVIO_DRIVER_ID), LBX_VERSION_VARG(LBX_AVIO_DRIVER_VERSION));
        return var_i32_(LBX_ERROR_INVALID_VERSION);
    }
    switch (request) {
    case mrInitialize:
        if (!LBX_HOST_API_IsCompatible(intf->host_api)) {
            Err_("host_api major version mismatch (got 0x%x, need 0x%x)",
                 intf->host_api ? intf->host_api->intf_version : 0u,
                 LBX_HOST_API_VERSION);
            return var_i32_(LBX_ERROR_INVALID_VERSION);
        }
        intf->entry = &lbx_avio_entry;
        intf->module_version = LBX_MAKE_VERSION(VERSION_MAJOR, VERSION_MINOR,
                                                VERSION_PATCH, BUILD_NUMBER);
        intf->module_name = module_name;
        intf->module_description = module_description;
        init_avio_cast_interface(intf, intf->intf_id, intf->intf_version);
        s_host_api = intf->host_api;
        s_drv      = drv;
        s_core     = cast_core_create();
        break;
    case mrTerminate:
        cast_core_destroy(s_core);
        s_core     = NULL;
        s_host_api = NULL;
        s_drv      = NULL;
        unregister_interface(&init_avio_cast_interface);
        break;
    case mrQuery:
        break;
    default:
        return var_err_(LBX_ERROR_INVALID_ENUM, "Invalid request");
    }
    return var_i32_(LBX_NO_ERROR);
}
