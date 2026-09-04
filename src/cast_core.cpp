//---------------------------------------------------------------------------
// cast_core.cpp — 채널별 프레임 링, refcount, 패킷 탭.
//
// 링 회계는 전부 채널 뮤텍스 안에서 돈다. interlocked 를 쓰지 않는 것은 어차피
// 슬롯 선택·기하 변경·복사가 같은 락을 필요로 해서, refcount 만 원자적으로
// 만들어 봐야 얻는 게 없기 때문이다. 경합은 채널당 초당 30회 수준이다.
//
// avio-v4l2 와 다른 점 하나 — 거기서는 "표시는 최신만, 녹화는 전량" 이라
// drain 이 버리는 프레임을 인코더에 따로 먹여야 했다. 여기서는 녹화가 프레임이
// 아니라 **패킷**을 받는다(보드가 인코딩한 것을 그대로 mux 한다). 그래서 링은
// 표시와 알고리즘 hold 만 감당하면 되고, 아무도 안 가져간 슬롯은 그냥 덮어써도
// 잃는 것이 없다. 패킷은 이미 탭으로 나갔다.
//---------------------------------------------------------------------------
#include "cast_core.h"
#include "cast_link.h"
#include "avio_cast_main.h"

#include <string.h>
#include <stdlib.h>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "system/lbx_log.h"
#include "gfx/lbx_gfx_yuv.h"   /* GFX_YUV_LAYOUT — gfx CPU YUV import 와 같은 표 */

/* lbx-core 에 있으나 아직 lib/lbx 로 배포되지 않은 플래그. 값은 lbx-core
 * src/image/lbx_image.h 가 정한 것과 같다. 배포가 따라오면 이 블록은 저절로
 * 사라진다(#ifndef). */
#ifndef LBX_IMAGE_FLAG_YUV_BT709
#define LBX_IMAGE_FLAG_YUV_BT709 0x20u
#endif

//===========================================================================
// 내부 구조
//===========================================================================

namespace {

struct CAST_SLOT {
    LBX_IMAGE img;        /* 영속 — 주소가 gfx 자원 캐시의 키다. 통째 복사 금지 */
    u8_t     *buf;        /* 플레인 전체를 담는 단일 할당                        */
    u32_t     buf_size;
    i32_t     ref_cnt;
    u32_t     fill;       /* 채워진 순번 — drain-to-latest 판정                  */
    i32_t     ready;      /* 1 = 완성 프레임 보유, 아직 안 가져감                */
};

struct CAST_CH {
    CAST_SLOT slot[CAST_RING_DEPTH];
    fourcc_t  fourcc;     /* 현재 기하 — 바뀌면 슬롯을 다시 깐다 */
    i32_t     w, h;
    u32_t     fill_seq;
    i32_t     open;
    u32_t     starved;    /* 모든 슬롯이 잡혀 프레임을 버린 횟수 */
    std::mutex              lock;
    std::condition_variable cv;
};

} // namespace

struct tagCAST_CORE {
    CAST_LINK     *link;
    CAST_CH        ch[AVIO_CAST_MAX_CH];
    CAST_PACKET_FN pkt_fn;
    void          *pkt_user;
};

//===========================================================================
// 슬롯 레이아웃
//===========================================================================

namespace {

/** 기하가 바뀌면 슬롯 버퍼를 다시 잡고 영속 LBX_IMAGE 의 레이아웃을 갱신한다.
 *  planes[].texture 는 건드리지 않는다 — gfx 백엔드 소유다. */
bool relayout(CAST_CH *c, fourcc_t fcc, i32_t w, i32_t h)
{
    const GFX_YUV_LAYOUT *lay = gfx_yuv_layout_find(fcc);
    i32_t pw[3], ph[3], bpp[3];
    u32_t total = 0;

    if (lay == NULL) {
        Err_("cast: 모르는 플레인 배치 " FOURCC_VFMT, FOURCC_VARG(fcc));
        return false;
    }
    {
        const i32_t cw  = (w + (1 << lay->chroma_shift_x) - 1) >> lay->chroma_shift_x;
        const i32_t chh = (h + (1 << lay->chroma_shift_y) - 1) >> lay->chroma_shift_y;
        pw[0] = w;  ph[0] = h;   bpp[0] = 1;
        pw[1] = cw; ph[1] = chh; bpp[1] = lay->semi_planar ? 2 : 1;
        pw[2] = cw; ph[2] = chh; bpp[2] = 1;
    }
    for (i32_t p = 0; p < lay->plane_count; ++p) {
        total += (u32_t)(pw[p] * bpp[p] * ph[p]);
    }

    for (i32_t s = 0; s < CAST_RING_DEPTH; ++s) {
        CAST_SLOT *sl = &c->slot[s];
        u32_t      off = 0;

        if (sl->buf_size != total) {
            free(sl->buf);
            sl->buf = (u8_t *)malloc(total);
            if (sl->buf == NULL) { sl->buf_size = 0; return false; }
            sl->buf_size = total;
        }
        /* 영속 LBX_IMAGE — memset 하지 않는다. texture 슬롯이 날아간다. */
        sl->img.pixel_format = fcc;
        sl->img.buffer_size  = total;
        sl->img.plane_count  = lay->plane_count;
        sl->img.codec        = 0;
        for (i32_t p = 0; p < lay->plane_count; ++p) {
            sl->img.planes[p].size.width  = pw[p];
            sl->img.planes[p].size.height = ph[p];
            sl->img.planes[p].stride.x    = bpp[p];
            sl->img.planes[p].stride.y    = pw[p] * bpp[p];
            sl->img.planes[p].data        = (intptr_t)(sl->buf + off);
            off += (u32_t)(pw[p] * bpp[p] * ph[p]);
        }
        sl->ready   = 0;
        sl->ref_cnt = 0;
    }
    c->fourcc = fcc;
    c->w = w;
    c->h = h;
    return true;
}

/** 채울 슬롯을 고른다(락 안). 안 잡힌 것 중 ready 아닌 것 우선, 없으면 가장
 *  오래된 ready. 전부 잡혀 있으면 NULL — 그때는 이번 프레임을 버린다. */
CAST_SLOT *pick_fill_slot(CAST_CH *c)
{
    CAST_SLOT *oldest = NULL;
    for (i32_t s = 0; s < CAST_RING_DEPTH; ++s) {
        CAST_SLOT *sl = &c->slot[s];
        if (sl->ref_cnt != 0) { continue; }
        if (!sl->ready) { return sl; }
        if (oldest == NULL || sl->fill < oldest->fill) { oldest = sl; }
    }
    return oldest;
}

void copy_plane(u8_t *dst, i32_t dst_stride, const u8_t *src, i32_t src_stride,
                i32_t row_bytes, i32_t rows)
{
    if (dst_stride == src_stride) {
        memcpy(dst, src, (size_t)dst_stride * rows);
        return;
    }
    for (i32_t y = 0; y < rows; ++y) {
        memcpy(dst + (size_t)y * dst_stride, src + (ptrdiff_t)y * src_stride,
               (size_t)row_bytes);
    }
}

/** 링크 스레드에서 들어오는 디코드 프레임 — 슬롯 하나를 채운다. */
void on_link_frame(void *user, i32_t idx, const CAST_DEC_FRAME *f)
{
    CAST_CORE *self = (CAST_CORE *)user;
    CAST_CH   *c;
    CAST_SLOT *sl;

    if (self == NULL || idx < 0 || idx >= AVIO_CAST_MAX_CH || f == NULL) { return; }
    c = &self->ch[idx];

    std::unique_lock<std::mutex> g(c->lock);
    if (!c->open) { return; }

    if (c->fourcc != f->format || c->w != f->width || c->h != f->height) {
        /* 첫 프레임이 여기다. 스트림 중간의 기하 변경은 보드가 프로파일을
         * 바꿔야 나오는 드문 일이고, 그때 gfx 가 캐시한 텍스처는 크기가 어긋
         * 난다 — 관측되면 그때 호스트에 알릴 길을 내기로 하고 지금은 남긴다. */
        if (c->fourcc != 0) {
            Warn_("cast: ch%d 기하 변경 %dx%d " FOURCC_VFMT " → %dx%d " FOURCC_VFMT,
                  idx, c->w, c->h, FOURCC_VARG(c->fourcc),
                  f->width, f->height, FOURCC_VARG(f->format));
        }
        if (!relayout(c, f->format, f->width, f->height)) { return; }
    }

    sl = pick_fill_slot(c);
    if (sl == NULL) {
        /* 링 전체가 hold 중 — 호스트가 UnrefFrame 을 안 짝지었거나 알고리즘이
         * 깊이보다 많이 쥐고 있다. 조용히 굶지 말고 세어서 드러낸다. */
        ++c->starved;
        WarnIf_((c->starved % 100) == 1,
                "cast: ch%d 링 %d개가 전부 hold - 프레임 버림 (누적 %u)",
                idx, CAST_RING_DEPTH, c->starved);
        return;
    }

    for (i32_t p = 0; p < sl->img.plane_count; ++p) {
        if (f->plane[p] == NULL) { return; }
        copy_plane((u8_t *)sl->img.planes[p].data, sl->img.planes[p].stride.y,
                   f->plane[p], f->stride[p],
                   sl->img.planes[p].size.width * sl->img.planes[p].stride.x,
                   sl->img.planes[p].size.height);
    }
    sl->img.flags = (u8_t)((f->full_range ? LBX_IMAGE_FLAG_YUV_FULL_RANGE : 0u)
                         | (f->bt709      ? LBX_IMAGE_FLAG_YUV_BT709      : 0u));
    sl->img.sequence = ++c->fill_seq;   /* 내용 버전 — 백엔드 재업로드 신호 */
    sl->fill  = c->fill_seq;
    sl->ready = 1;
    c->cv.notify_one();
}

/** 링크 스레드에서 들어오는 원시 패킷 — 탭이 걸려 있으면 그대로 넘긴다. */
void on_link_packet(void *user, i32_t idx, const u8_t *pkt, u32_t size,
                    i64_t pts_ms, bool key)
{
    CAST_CORE *self = (CAST_CORE *)user;
    if (self == NULL || self->pkt_fn == NULL) { return; }
    self->pkt_fn(self->pkt_user, idx, pkt, size, pts_ms, key);
}

} // namespace

//===========================================================================
// 공개 API
//===========================================================================

CAST_CORE *cast_core_create(void)
{
    CAST_CORE *self = new CAST_CORE();
    self->pkt_fn   = NULL;
    self->pkt_user = NULL;
    for (i32_t i = 0; i < AVIO_CAST_MAX_CH; ++i) {
        CAST_CH *c = &self->ch[i];
        for (i32_t s = 0; s < CAST_RING_DEPTH; ++s) {
            memset(&(c->slot[s].img), 0, sizeof(LBX_IMAGE));   /* 최초 1회만 */
            c->slot[s].buf      = NULL;
            c->slot[s].buf_size = 0;
            c->slot[s].ref_cnt  = 0;
            c->slot[s].fill     = 0;
            c->slot[s].ready    = 0;
        }
        c->fourcc = 0; c->w = 0; c->h = 0;
        c->fill_seq = 0; c->open = 0; c->starved = 0;
    }
    self->link = cast_link_create();
    cast_link_set_sinks(self->link, &on_link_frame, &on_link_packet, self);
    return self;
}

void cast_core_destroy(CAST_CORE *self)
{
    if (self == NULL) { return; }
    for (i32_t i = 0; i < AVIO_CAST_MAX_CH; ++i) { cast_core_close_ch(self, i); }
    cast_link_destroy(self->link);
    for (i32_t i = 0; i < AVIO_CAST_MAX_CH; ++i) {
        for (i32_t s = 0; s < CAST_RING_DEPTH; ++s) { free(self->ch[i].slot[s].buf); }
    }
    delete self;
}

void cast_core_config(CAST_CORE *self, const char *host, i32_t base_port,
                      const char *decoder_pref)
{
    if (self == NULL) { return; }
    cast_link_config(self->link, host, base_port, decoder_pref);
}

struct tagCAST_LINK *cast_core_link(CAST_CORE *self)
{
    return (self != NULL) ? self->link : NULL;
}

i32_t cast_core_open_ch(CAST_CORE *self, i32_t ch)
{
    if (self == NULL || ch < 0 || ch >= AVIO_CAST_MAX_CH) { return 0; }
    {
        std::lock_guard<std::mutex> g(self->ch[ch].lock);
        self->ch[ch].open    = 1;
        self->ch[ch].starved = 0;
    }
    return cast_link_start(self->link, ch);
}

void cast_core_close_ch(CAST_CORE *self, i32_t ch)
{
    if (self == NULL || ch < 0 || ch >= AVIO_CAST_MAX_CH) { return; }
    /* 스레드를 먼저 세운다 — open=0 만 내리고 join 을 안 하면 스레드가 락을
     * 잡은 채 죽은 코어를 만질 수 있다. */
    cast_link_stop(self->link, ch);
    {
        std::lock_guard<std::mutex> g(self->ch[ch].lock);
        self->ch[ch].open = 0;
        for (i32_t s = 0; s < CAST_RING_DEPTH; ++s) {
            self->ch[ch].slot[s].ready = 0;
        }
    }
}

LBX_IMAGE *cast_core_take(CAST_CORE *self, i32_t ch, i32_t timeout_ms)
{
    CAST_CH   *c;
    CAST_SLOT *best = NULL;

    if (self == NULL || ch < 0 || ch >= AVIO_CAST_MAX_CH) { return NULL; }
    c = &self->ch[ch];

    std::unique_lock<std::mutex> g(c->lock);
    if (timeout_ms > 0) {
        c->cv.wait_for(g, std::chrono::milliseconds(timeout_ms), [c] {
            for (i32_t s = 0; s < CAST_RING_DEPTH; ++s) {
                if (c->slot[s].ready) { return true; }
            }
            return false;
        });
    }

    /* drain-to-latest — 최신 한 장만 내보내고, 남은 ready 는 표시에는 낡았다.
     * 녹화는 패킷 경로가 이미 받았으므로 여기서 버려도 잃는 것이 없다. */
    for (i32_t s = 0; s < CAST_RING_DEPTH; ++s) {
        CAST_SLOT *sl = &c->slot[s];
        if (!sl->ready) { continue; }
        if (best == NULL || sl->fill > best->fill) { best = sl; }
    }
    if (best == NULL) { return NULL; }

    for (i32_t s = 0; s < CAST_RING_DEPTH; ++s) {
        CAST_SLOT *sl = &c->slot[s];
        if (sl->ready && sl != best) { sl->ready = 0; }   /* stale — 반납 */
    }
    best->ready   = 0;
    best->ref_cnt = 1;              /* driver-implicit ref */
    return &(best->img);
}

void cast_core_ref(CAST_CORE *self, i32_t ch, const LBX_IMAGE *img)
{
    if (self == NULL || ch < 0 || ch >= AVIO_CAST_MAX_CH || img == NULL) { return; }
    {
        CAST_CH *c = &self->ch[ch];
        std::lock_guard<std::mutex> g(c->lock);
        for (i32_t s = 0; s < CAST_RING_DEPTH; ++s) {
            if (&(c->slot[s].img) == img) { ++c->slot[s].ref_cnt; return; }
        }
    }
    Warn_("cast: ch%d 모르는 프레임에 RefFrame", ch);
}

void cast_core_unref(CAST_CORE *self, i32_t ch, const LBX_IMAGE *img)
{
    if (self == NULL || ch < 0 || ch >= AVIO_CAST_MAX_CH || img == NULL) { return; }
    {
        CAST_CH *c = &self->ch[ch];
        std::lock_guard<std::mutex> g(c->lock);
        for (i32_t s = 0; s < CAST_RING_DEPTH; ++s) {
            if (&(c->slot[s].img) != img) { continue; }
            if (--c->slot[s].ref_cnt <= 0) {
                c->slot[s].ref_cnt = 0;   /* 슬롯이 링으로 돌아온다 */
            }
            return;
        }
    }
    Warn_("cast: ch%d 모르는 프레임에 UnrefFrame", ch);
}

void cast_core_set_packet_tap(CAST_CORE *self, CAST_PACKET_FN fn, void *user)
{
    if (self == NULL) { return; }
    self->pkt_fn   = fn;
    self->pkt_user = user;
}
