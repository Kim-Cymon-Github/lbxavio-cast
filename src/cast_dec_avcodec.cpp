//---------------------------------------------------------------------------
// cast_dec_avcodec.cpp — libavcodec 백엔드.
//
// 저지연이 여기서 갈린다. 프레임 단위 스레딩(FF_THREAD_FRAME)은 스레드 수만큼
// 프레임을 미리 물고 시작해야 병렬이 되므로 그만큼 화면이 늦게 나온다 — 8채널을
// 빨리 풀자고 thread_count 를 코어 수로 두면 코어 수만큼 지연이 붙는 셈이다.
// 실시간 관찰에서는 처리량보다 "지금 화면" 이 중요하다. 슬라이스 스레딩은 한
// 프레임 안에서 나누므로 지연을 만들지 않는다. LOW_DELAY 는 디코더에게 재정렬
// 버퍼를 두지 말라고 이르는 것이고, demux 쪽 nobuffer/low_delay 와 짝이 맞아야
// 실제로 줄어든다(clink 실측에서 확인된 조합 그대로 옮겼다).
//
// 4:2:0 8bit 이 아닌 출력(10bit, 4:2:2)만 여기서 CPU 변환한다. 보드 스트림은
// H.265 8bit 4:2:0 이라 그 길로 오지 않지만, 남의 스트림을 물렸을 때 조용히
// 검은 화면이 되는 것보다 낫다.
//---------------------------------------------------------------------------
#include "cast_dec.h"

#include <string.h>
#include <stdlib.h>

#include "system/lbx_log.h"

#ifdef CONFIG_AVCODEC_DECODER

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swscale.lib")

namespace {

struct AVC_IMPL {
    AVCodecContext *dec;
    AVPacket       *pkt;
    AVFrame        *frm;
    AVFrame        *cvt;      /* sws 출력 — 위 주석의 예외 경로에서만 */
    SwsContext     *sws;
};

/** Matroska CodecID → AVCodecID. 짧은 이름도 받는다 — demux 가 컨테이너를
 *  직접 파싱하지 않고 avformat 이름을 넘겨줄 수도 있기 때문이다. */
AVCodecID codec_id_of(const char *s)
{
    if (s == NULL) { return AV_CODEC_ID_NONE; }
    if (strcmp(s, "V_MPEGH/ISO/HEVC") == 0 || strcmp(s, "hevc") == 0
        || strcmp(s, "h265") == 0) {
        return AV_CODEC_ID_HEVC;
    }
    if (strcmp(s, "V_MPEG4/ISO/AVC") == 0 || strcmp(s, "h264") == 0
        || strcmp(s, "avc") == 0) {
        return AV_CODEC_ID_H264;
    }
    return AV_CODEC_ID_NONE;
}

/** libav 픽셀 포맷 → 우리가 그대로 받는 fourcc. 0 = 변환 필요. */
fourcc_t fourcc_of(int av_fmt)
{
    switch (av_fmt) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P: return fourcc_('I', '4', '2', '0');
    case AV_PIX_FMT_NV12:     return fourcc_('N', 'V', '1', '2');
    case AV_PIX_FMT_NV21:     return fourcc_('N', 'V', '2', '1');
    default:                  return 0;
    }
}

void impl_free(AVC_IMPL *im)
{
    if (im == NULL) { return; }
    sws_freeContext(im->sws);
    av_frame_free(&(im->cvt));
    av_frame_free(&(im->frm));
    av_packet_free(&(im->pkt));
    avcodec_free_context(&(im->dec));
    free(im);
}

i32_t avc_open(CAST_DECODER *self, const char *codec_id, const u8_t *priv,
               u32_t priv_len, i32_t w, i32_t h)
{
    AVC_IMPL     *im;
    const AVCodec *codec;
    AVCodecID      id = codec_id_of(codec_id);

    if (self == NULL) { return 0; }
    if (id == AV_CODEC_ID_NONE) {
        Err_("cast-dec: 모르는 codec id '%s'", (codec_id != NULL) ? codec_id : "(null)");
        return 0;
    }
    codec = avcodec_find_decoder(id);
    if (codec == NULL) { Err_("cast-dec: 디코더 없음 (%s)", codec_id); return 0; }

    im = (AVC_IMPL *)calloc(1, sizeof(AVC_IMPL));
    if (im == NULL) { return 0; }

    im->dec = avcodec_alloc_context3(codec);
    im->pkt = av_packet_alloc();
    im->frm = av_frame_alloc();
    if (im->dec == NULL || im->pkt == NULL || im->frm == NULL) {
        impl_free(im);
        return 0;
    }
    im->dec->width  = w;
    im->dec->height = h;

    /* CodecPrivate(avcC/hvcC) — 스트림 중간에 합류해도 파라미터셋을 여기서
     * 받으므로 첫 키프레임을 기다리는 것 말고는 할 일이 없다. */
    if (priv != NULL && priv_len > 0) {
        im->dec->extradata = (u8_t *)av_mallocz(priv_len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (im->dec->extradata == NULL) { impl_free(im); return 0; }
        memcpy(im->dec->extradata, priv, priv_len);
        im->dec->extradata_size = (int)priv_len;
    }

    im->dec->flags       |= AV_CODEC_FLAG_LOW_DELAY;
    im->dec->thread_type  = FF_THREAD_SLICE;
    im->dec->thread_count = 4;

    if (avcodec_open2(im->dec, codec, NULL) < 0) {
        Err_("cast-dec: avcodec_open2 실패 (%s)", codec_id);
        impl_free(im);
        return 0;
    }
    self->impl = im;
    return 1;
}

i32_t avc_send(CAST_DECODER *self, const u8_t *pkt, u32_t size, i64_t pts_ms)
{
    AVC_IMPL *im = (AVC_IMPL *)self->impl;
    int r;

    if (im == NULL) { return -1; }
    if (pkt == NULL) {
        r = avcodec_send_packet(im->dec, NULL);   /* EOF */
    } else {
        im->pkt->data = (u8_t *)pkt;
        im->pkt->size = (int)size;
        im->pkt->pts  = pts_ms;
        im->pkt->dts  = pts_ms;
        r = avcodec_send_packet(im->dec, im->pkt);
        av_packet_unref(im->pkt);
    }
    if (r == 0)                 { return 1; }
    if (r == AVERROR(EAGAIN))   { return 0; }   /* 받아 비운 뒤 같은 패킷 재시도 */
    /* 중간 합류 초반의 참조 실패는 흔하다 — 오류로 올리면 매 재접속마다
     * 링크가 죽는다. 키프레임이 오면 저절로 맞는다. */
    return 0;
}

i32_t avc_receive(CAST_DECODER *self, CAST_DEC_FRAME *out)
{
    AVC_IMPL      *im = (AVC_IMPL *)self->impl;
    const AVFrame *f;
    fourcc_t       fcc;

    if (im == NULL || out == NULL) { return -1; }
    if (avcodec_receive_frame(im->dec, im->frm) != 0) { return 0; }

    f   = im->frm;
    fcc = fourcc_of(im->frm->format);

    if (fcc == 0) {
        AVFrame *c = im->cvt;
        if (c == NULL || c->width != im->frm->width || c->height != im->frm->height) {
            av_frame_free(&(im->cvt));
            c = av_frame_alloc();
            if (c == NULL) { return -1; }
            c->format = AV_PIX_FMT_YUV420P;
            c->width  = im->frm->width;
            c->height = im->frm->height;
            if (av_frame_get_buffer(c, 32) < 0) { av_frame_free(&c); return -1; }
            im->cvt = c;
            sws_freeContext(im->sws);
            im->sws = sws_getContext(im->frm->width, im->frm->height,
                                     (AVPixelFormat)im->frm->format,
                                     im->frm->width, im->frm->height,
                                     AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                                     NULL, NULL, NULL);
            if (im->sws == NULL) { return -1; }
        }
        sws_scale(im->sws, im->frm->data, im->frm->linesize, 0, im->frm->height,
                  c->data, c->linesize);
        f   = c;
        fcc = fourcc_('I', '4', '2', '0');
    }

    memset(out, 0, sizeof(*out));
    out->width  = f->width;
    out->height = f->height;
    out->format = fcc;
    out->pts_ms = im->frm->pts;
    /* 색 규약은 원본 프레임에서 읽는다 — sws 를 거쳐도 의미는 그대로다.
     * 미지정(UNSPECIFIED)은 601 로 둔다. */
    out->full_range = (im->frm->color_range == AVCOL_RANGE_JPEG
                       || im->frm->format == AV_PIX_FMT_YUVJ420P) ? 1 : 0;
    out->bt709      = (im->frm->colorspace == AVCOL_SPC_BT709) ? 1 : 0;
    for (i32_t i = 0; i < 3; ++i) {
        out->plane[i]  = f->data[i];
        out->stride[i] = f->linesize[i];
    }
    return 1;
}

void avc_flush(CAST_DECODER *self)
{
    AVC_IMPL *im = (AVC_IMPL *)self->impl;
    if (im != NULL && im->dec != NULL) { avcodec_flush_buffers(im->dec); }
}

void avc_close(CAST_DECODER *self)
{
    if (self == NULL) { return; }
    impl_free((AVC_IMPL *)self->impl);
    self->impl = NULL;
}

const CAST_DEC_VTBL s_avc_vtbl = {
    "avcodec", &avc_open, &avc_send, &avc_receive, &avc_flush, &avc_close
};

} // namespace

i32_t cast_dec_create_avcodec(CAST_DECODER *self)
{
    if (self == NULL) { return 0; }
    self->vt   = &s_avc_vtbl;
    self->impl = NULL;
    return 1;
}

#else /* !CONFIG_AVCODEC_DECODER — 백엔드를 뺀 빌드 */

i32_t cast_dec_create_avcodec(CAST_DECODER *self) { (void)self; return 0; }

#endif

#ifndef CONFIG_GST_DECODER
i32_t cast_dec_create_gst(CAST_DECODER *self) { (void)self; return 0; }
#endif

i32_t cast_dec_create(CAST_DECODER *self, const char *pref)
{
    if (self == NULL) { return 0; }
    memset(self, 0, sizeof(*self));

    if (pref != NULL && pref[0] != '\0' && strcmp(pref, "auto") != 0) {
        if (strcmp(pref, "avcodec") == 0) { return cast_dec_create_avcodec(self); }
        if (strcmp(pref, "gst") == 0)     { return cast_dec_create_gst(self); }
        Warn_("cast-dec: 모르는 백엔드 '%s' - auto 로 진행", pref);
    }
    if (cast_dec_create_avcodec(self)) { return 1; }
    if (cast_dec_create_gst(self))     { return 1; }
    Err_("cast-dec: 쓸 수 있는 디코더 백엔드가 없다");
    return 0;
}
