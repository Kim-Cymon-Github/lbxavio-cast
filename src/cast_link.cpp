//---------------------------------------------------------------------------
// cast_link.cpp — adb 포트 포워딩과 채널별 TCP demux 스레드.
//
// adb 는 자식 프로세스로 돌린다. 클라이언트 라이브러리가 없고 CLI 출력 형식이
// 사실상 그 인터페이스다. 파싱하는 것은 두 가지뿐 —
//
//   adb devices           "<serial>\t<state>"                (첫 줄은 안내문)
//   adb forward --list    "<serial> tcp:<local> tcp:<remote>"
//
// 둘 다 오래된 형식이라 바뀔 일이 없고, 못 읽어도 포워딩을 한 번 더 걸 뿐이라
// 손해가 없다(adb forward 는 멱등이다). tool/clink 의 clink_adb.cpp 를 옮겼다.
//
// 채널 스레드는 붙고, demux 하고, 패킷을 두 곳으로 흘린다 — 디코더와, 걸려
// 있으면 원시 패킷 탭(녹화가 붙을 자리). 재접속은 스스로 한다. 보드가 아직
// 스트리밍을 안 켰거나 rec 이 내려가 스트림이 끊긴 상태가 정상 상황이므로,
// 실패는 오류가 아니라 "아직" 이다.
//---------------------------------------------------------------------------
#include "cast_link.h"
#include "avio_cast_main.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <chrono>

#include "system/lbx_log.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "ws2_32.lib")

/** adb 한 번 호출에 허용하는 시간. 데몬이 안 떠 있으면 첫 호출이 길다. */
#define ADB_TIMEOUT_MS (8000)

//===========================================================================
// 자식 프로세스로 adb 돌리기
//===========================================================================

/**
 * adb 를 실행하고 표준출력을 out 에 담는다. 반환은 종료 코드(음수면 실행
 * 자체가 안 된 것). stderr 도 같은 파이프로 합친다 — 에러 문구가 진단의 전부다.
 */
static int adb_run(const CAST_ADB *a, const char *args, char *out, int out_size)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    HANDLE  rd = NULL, wr = NULL;
    char    cmd[512];
    int     len = 0;
    DWORD   t0, code = 0;
    BOOL    ok;

    if (out != NULL && out_size > 0) { out[0] = '\0'; }
    if (a == NULL || a->exe[0] == '\0') { return -1; }

    memset(&sa, 0, sizeof(sa));
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 64 * 1024)) { return -1; }
    // 읽는 쪽까지 상속되면 자식이 끝나도 파이프가 안 닫혀 EOF 가 오지 않는다.
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    snprintf(cmd, sizeof(cmd), "\"%s\" %s", a->exe, args);

    memset(&si, 0, sizeof(si));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    memset(&pi, 0, sizeof(pi));

    // CREATE_NO_WINDOW — adb 가 새 창을 깜빡이며 띄우지 않게.
    ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi);
    CloseHandle(wr);   /* 부모 쪽 쓰기 끝을 놓아야 EOF 가 온다 */
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }

    t0 = GetTickCount();
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            char  buf[1024];
            DWORD want = (avail > (DWORD)sizeof(buf)) ? (DWORD)sizeof(buf) : avail;
            DWORD got  = 0;
            if (!ReadFile(rd, buf, want, &got, NULL) || got == 0) { break; }
            if (out != NULL && len < out_size - 1) {
                int room = out_size - 1 - len;
                int n    = ((int)got < room) ? (int)got : room;
                memcpy(out + len, buf, (size_t)n);
                len += n;
                out[len] = '\0';
            }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) || avail == 0) { break; }
            continue;
        }
        if (GetTickCount() - t0 > ADB_TIMEOUT_MS) {
            Warn_("adb: %s - %d ms 무응답", args, ADB_TIMEOUT_MS);
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        Sleep(5);
    }

    WaitForSingleObject(pi.hProcess, 1000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return (int)code;
}

/** `-s <serial>` 을 앞에 붙인 인자 문자열. serial 이 비면 그대로 둔다. */
static void adb_args(const CAST_ADB *a, const char *tail, char *buf, int size)
{
    if (a->serial[0] != '\0') {
        snprintf(buf, (size_t)size, "-s %s %s", a->serial, tail);
    } else {
        snprintf(buf, (size_t)size, "%s", tail);
    }
}

//===========================================================================
// adb 실행 파일 찾기
//===========================================================================

static int try_exe(CAST_ADB *a, const char *path)
{
    if (path == NULL || path[0] == '\0') { return 0; }
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) { return 0; }
    snprintf(a->exe, sizeof(a->exe), "%s", path);
    return 1;
}

static int try_sdk_dir(CAST_ADB *a, const char *dir, const char *sub)
{
    char path[260];
    if (dir == NULL || dir[0] == '\0') { return 0; }
    snprintf(path, sizeof(path), "%s%s\\adb.exe", dir, sub);
    return try_exe(a, path);
}

void cast_adb_init(CAST_ADB *a)
{
    char found[MAX_PATH];

    if (a == NULL) { return; }
    memset(a, 0, sizeof(*a));
    a->enabled = 1;

    // 1) 명시 지정이 최우선 — SDK 를 여럿 오가는 자리에서 필요하다.
    if (try_exe(a, getenv("AVIO_CAST_ADB"))) { goto done; }
    if (try_exe(a, getenv("CLINK_ADB")))     { goto done; }   /* clink 설정 계승 */

    // 2) PATH. platform-tools 를 PATH 에 넣어 쓰는 것이 보통이다.
    if (SearchPathA(NULL, "adb.exe", NULL, MAX_PATH, found, NULL) > 0) {
        if (try_exe(a, found)) { goto done; }
    }

    // 3) SDK 표준 위치.
    if (try_sdk_dir(a, getenv("ANDROID_HOME"),     "\\platform-tools")) { goto done; }
    if (try_sdk_dir(a, getenv("ANDROID_SDK_ROOT"), "\\platform-tools")) { goto done; }
    if (try_sdk_dir(a, getenv("LOCALAPPDATA"), "\\Android\\Sdk\\platform-tools")) { goto done; }

    snprintf(a->note, sizeof(a->note), u8"adb 없음 (PATH/ANDROID_HOME/AVIO_CAST_ADB)");
    return;

done:
    snprintf(a->note, sizeof(a->note), "adb: %s", a->exe);
}

void cast_adb_set_exe(CAST_ADB *a, const char *path)
{
    if (a == NULL || path == NULL) { return; }
    snprintf(a->exe, sizeof(a->exe), "%s", path);
    snprintf(a->note, sizeof(a->note), "adb: %s", a->exe);
}

void cast_adb_set_serial(CAST_ADB *a, const char *serial)
{
    if (a == NULL) { return; }
    snprintf(a->serial, sizeof(a->serial), "%s", (serial != NULL) ? serial : "");
}

i32_t cast_adb_available(const CAST_ADB *a)
{
    return (a != NULL && a->enabled && a->exe[0] != '\0') ? 1 : 0;
}

//===========================================================================
// devices / forward
//===========================================================================

/** 한 줄에서 공백·탭으로 끊어 n 번째 토큰을 뽑는다. 없으면 0. */
static int line_token(const char *line, const char *end, int n, char *out, int size)
{
    const char *p = line;
    int i;

    out[0] = '\0';
    for (i = 0; i <= n; ++i) {
        const char *s;
        while (p < end && (*p == ' ' || *p == '\t')) { ++p; }
        s = p;
        while (p < end && *p != ' ' && *p != '\t') { ++p; }
        if (s == p) { return 0; }
        if (i == n) {
            int len = (int)(p - s);
            if (len > size - 1) { len = size - 1; }
            memcpy(out, s, (size_t)len);
            out[len] = '\0';
            return 1;
        }
    }
    return 0;
}

static const char *line_end(const char *p)
{
    while (*p != '\0' && *p != '\r' && *p != '\n') { ++p; }
    return p;
}

static const char *line_next(const char *e)
{
    while (*e == '\r' || *e == '\n') { ++e; }
    return e;
}

i32_t cast_adb_probe(CAST_ADB *a)
{
    char out[4096];
    char first[64] = { 0 };
    char want[64]  = { 0 };
    int  n_dev = 0, n_bad = 0;
    const char *p;

    if (!cast_adb_available(a)) { return 0; }

    // 사용자가 시리얼을 못박았으면 그 장치만 센다.
    snprintf(want, sizeof(want), "%s", a->serial);

    if (adb_run(a, "devices", out, sizeof(out)) < 0) {
        snprintf(a->note, sizeof(a->note), u8"adb 실행 실패 (%s)", a->exe);
        a->n_dev = 0;
        return 0;
    }

    for (p = out; *p != '\0'; ) {
        const char *e = line_end(p);
        char serial[64], state[32];
        if (line_token(p, e, 0, serial, sizeof(serial)) &&
            line_token(p, e, 1, state,  sizeof(state))) {
            // 안내문("List of devices attached")은 둘째 토큰이 상태가 아니라 걸러진다.
            if (strcmp(state, "device") == 0) {
                if (want[0] == '\0' || strcmp(want, serial) == 0) {
                    if (first[0] == '\0') { snprintf(first, sizeof(first), "%s", serial); }
                    ++n_dev;
                }
            } else if (strcmp(state, "unauthorized") == 0 || strcmp(state, "offline") == 0) {
                ++n_bad;
            }
        }
        p = line_next(e);
    }

    a->n_dev = n_dev;
    if (n_dev == 0) {
        a->n_fwd = 0;
        if (n_bad > 0) {
            snprintf(a->note, sizeof(a->note), u8"adb: 장치 %d대 unauthorized/offline", n_bad);
        } else if (want[0] != '\0') {
            snprintf(a->note, sizeof(a->note), u8"adb: %s 안 붙어 있음", want);
        } else {
            snprintf(a->note, sizeof(a->note), u8"adb: 장치 없음");
        }
        return 0;
    }

    // 고정하지 않았어도 고른 시리얼을 남겨 -s 로 넘긴다. 두 대째가 꽂히는
    // 순간 인자 없는 adb 는 "more than one device" 로 전부 실패하기 때문이다.
    snprintf(a->serial, sizeof(a->serial), "%s", first);
    if (n_dev > 1 && want[0] == '\0') {
        Info_(u8"adb: 장치 %d대 - %s 사용", n_dev, a->serial);
    }
    return n_dev;
}

i32_t cast_adb_forward_range(CAST_ADB *a, i32_t base, i32_t count)
{
    char list[8192];
    char args[256];
    int  added = 0;
    int  i;

    if (!cast_adb_available(a) || count <= 0) { return 0; }
    if (a->serial[0] == '\0' && a->n_dev <= 0) { return 0; }

    // 이미 서 있는 것은 건드리지 않는다 — 다시 걸면 adb 가 리스너를 새로
    // 세우면서 그 순간 열려 있던 연결이 끊긴다.
    adb_args(a, "forward --list", args, sizeof(args));
    if (adb_run(a, args, list, sizeof(list)) < 0) { return -1; }

    for (i = 0; i < count; ++i) {
        char        key[32];
        const char *p;
        int         have = 0;

        snprintf(key, sizeof(key), "tcp:%d", base + i);
        for (p = list; *p != '\0' && !have; ) {
            const char *e = line_end(p);
            char local[32];
            if (line_token(p, e, 1, local, sizeof(local)) && strcmp(local, key) == 0) {
                have = 1;
            }
            p = line_next(e);
        }
        if (have) { continue; }

        {
            char  tail[64];
            char  msg[256];
            char *nl;
            snprintf(tail, sizeof(tail), "forward tcp:%d tcp:%d", base + i, base + i);
            adb_args(a, tail, args, sizeof(args));
            if (adb_run(a, args, msg, sizeof(msg)) == 0) {
                ++added;
                continue;
            }
            // 실패를 삼키면 이 포트만 안 붙는 상태가 되어 진단이 어렵다.
            nl = strpbrk(msg, "\r\n");
            if (nl != NULL) { *nl = '\0'; }
            Warn_("adb: forward tcp:%d 실패%s%s", base + i,
                  (msg[0] != '\0') ? " - " : "", msg);
        }
    }

    a->n_fwd += added;
    return added;
}

i32_t cast_adb_autoforward(CAST_ADB *a, i32_t base, i32_t count)
{
    int added;

    if (!cast_adb_available(a)) { return 0; }
    if (cast_adb_probe(a) <= 0) { return 0; }

    added = cast_adb_forward_range(a, base, count);
    if (added > 0) {
        Info_("adb: %s - forward %d개 추가 (tcp:%d..%d)", a->serial, added,
              base, base + count - 1);
    }
    snprintf(a->note, sizeof(a->note), u8"adb: %s%s", a->serial,
             (added > 0) ? u8" (forward 새로 걺)" : u8" (forward 준비됨)");
    return 1;
}

//===========================================================================
// 채널 스레드
//===========================================================================

struct CAST_CH_LINK {
    CAST_LINK_STAT  st;
    CAST_DECODER    dec;
    i32_t           dec_open;
    std::thread    *th;
};

struct tagCAST_LINK {
    char           host[64];
    i32_t          base_port;
    char           dec_pref[16];
    CAST_ADB       adb;
    u32_t          adb_last_tick;
    CAST_LINK_FRAME_FN on_frame;
    CAST_LINK_PKT_FN   on_packet;
    void              *user;
    CAST_CH_LINK   ch[AVIO_CAST_MAX_CH];
};

namespace {

/** AVCodecID → Matroska CodecID 문자열. 디코더 seam 이 play_dec 와 같은 말을
 *  쓰도록(P3 승격 때 번역 단계가 생기지 않도록) 여기서 되돌린다. */
const char *mkv_codec_id(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_HEVC: return "V_MPEGH/ISO/HEVC";
    case AV_CODEC_ID_H264: return "V_MPEG4/ISO/AVC";
    default:               return NULL;
    }
}

/** 채널 한 회차 — 열기부터 스트림이 끝날 때까지. 반환은 실패 사유(NULL=정상). */
const char *run_once(CAST_LINK *self, i32_t idx)
{
    CAST_CH_LINK    *c   = &self->ch[idx];
    AVFormatContext *fmt = NULL;
    AVPacket        *pkt = NULL;
    AVDictionary    *opts = NULL;
    const char      *fail = NULL;
    const char      *mkv_id;
    int              vs = -1;

    /* 서버가 아직 안 떴을 때 오래 매달리지 않도록 상한을 둔다. tcp 프로토콜의
     * listen_timeout 이 아니라 열기 전체(연결+판별)에 거는 시계다. */
    av_dict_set(&opts, "timeout", "3000000", 0);      /* 3s, microseconds */
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "flags", "low_delay", 0);
    /* 스트리밍 MKV 는 Cluster 가 unknown-size 라 앞을 오래 읽어도 얻는 게
     * 없다. 판별에 필요한 최소만 보고 바로 흘린다. */
    av_dict_set(&opts, "analyzeduration", "1000000", 0);
    av_dict_set(&opts, "probesize", "1000000", 0);

    /* 인터럽트 콜백 — want_open 이 내려가면 열기·읽기 어디서든 빠져나온다.
     * 이게 없으면 보드가 조용히 멈췄을 때 av_read_frame 이 영원히 기다리고
     * join 도 같이 굳는다(clink 2026-09-02 실측). 위 timeout 은 열기 전용이다. */
    fmt = avformat_alloc_context();
    if (fmt == NULL) { av_dict_free(&opts); return "alloc failed"; }
    fmt->interrupt_callback.callback = [](void *p) -> int {
        return ((CAST_LINK_STAT *)p)->want_open ? 0 : 1;
    };
    fmt->interrupt_callback.opaque = &(c->st);

    if (avformat_open_input(&fmt, c->st.url, NULL, &opts) < 0) {
        av_dict_free(&opts);
        return "connect failed";   /* 실패 시 libav 가 fmt 를 해제한다 */
    }
    av_dict_free(&opts);

    if (avformat_find_stream_info(fmt, NULL) < 0) { fail = "no stream info"; goto done; }

    vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vs < 0) { fail = "no video stream"; goto done; }

    mkv_id = mkv_codec_id(fmt->streams[vs]->codecpar->codec_id);
    if (mkv_id == NULL) { fail = "unsupported codec"; goto done; }

    if (!cast_dec_create(&(c->dec), self->dec_pref)) { fail = "no decoder backend"; goto done; }
    if (!c->dec.vt->Open(&(c->dec), mkv_id,
                         fmt->streams[vs]->codecpar->extradata,
                         (u32_t)fmt->streams[vs]->codecpar->extradata_size,
                         fmt->streams[vs]->codecpar->width,
                         fmt->streams[vs]->codecpar->height)) {
        fail = "decoder open failed";
        goto done;
    }
    c->dec_open = 1;

    pkt = av_packet_alloc();
    if (pkt == NULL) { fail = "alloc failed"; goto done; }

    c->st.connected = 1;
    c->st.err[0]    = '\0';
    c->st.width     = fmt->streams[vs]->codecpar->width;
    c->st.height    = fmt->streams[vs]->codecpar->height;
    snprintf(c->st.codec, sizeof(c->st.codec), "%s",
             avcodec_get_name(fmt->streams[vs]->codecpar->codec_id));
    /* 창을 안 보고 돌릴 때(스모크·원격) 이 줄이 유일한 진행 신호다. */
    Info_("cast: ch%d open %s - %s %dx%d", idx, c->st.url, c->st.codec,
          c->st.width, c->st.height);

    while (c->st.want_open) {
        int r = av_read_frame(fmt, pkt);
        if (r < 0) { fail = (r == AVERROR_EOF) ? "stream ended" : "read error"; break; }
        if (pkt->stream_index != vs) { av_packet_unref(pkt); continue; }

        {
            const i64_t pts_ms = (pkt->pts != AV_NOPTS_VALUE)
                ? (i64_t)(pkt->pts * av_q2d(fmt->streams[vs]->time_base) * 1000.0) : 0;
            const bool  key    = (pkt->flags & AV_PKT_FLAG_KEY) != 0;

            /* 원시 패킷을 먼저 흘린다 — 녹화는 디코드 성패와 무관해야 한다
             * (보드가 인코딩한 것을 그대로 mux 하는 길이다). */
            if (self->on_packet != NULL) {
                self->on_packet(self->user, idx, pkt->data, (u32_t)pkt->size,
                                pts_ms, key);
            }

            /* Send 가 0(파이프라인 가득)을 주면 받아 비운 뒤 같은 패킷을
             * 다시 넣는다 — 계약이 그렇다. */
            for (int guard = 0; guard < 8; ++guard) {
                int s = c->dec.vt->Send(&(c->dec), pkt->data, (u32_t)pkt->size, pts_ms);
                if (s != 0) { break; }
                CAST_DEC_FRAME f;
                if (c->dec.vt->Receive(&(c->dec), &f) == 1) {
                    ++c->st.frames;
                    if (self->on_frame != NULL) { self->on_frame(self->user, idx, &f); }
                } else {
                    break;
                }
            }
            av_packet_unref(pkt);

            for (;;) {
                CAST_DEC_FRAME f;
                if (c->dec.vt->Receive(&(c->dec), &f) != 1) { break; }
                ++c->st.frames;
                if (self->on_frame != NULL) { self->on_frame(self->user, idx, &f); }
            }
        }
    }

done:
    c->st.connected = 0;
    av_packet_free(&pkt);
    if (c->dec_open) { c->dec.vt->Close(&(c->dec)); c->dec_open = 0; }
    if (fmt != NULL) { avformat_close_input(&fmt); }
    return fail;
}

void ch_thread(CAST_LINK *self, i32_t idx)
{
    CAST_CH_LINK *c = &self->ch[idx];
    char last_err[128] = { 0 };

    c->st.running = 1;
    while (c->st.want_open) {
        const char *fail = run_once(self, idx);
        if (!c->st.want_open) { break; }
        snprintf(c->st.err, sizeof(c->st.err), "%s", (fail != NULL) ? fail : "closed");
        /* 같은 사유가 반복되는 동안은 조용히 — 보드가 아직 안 켜진 상태로
         * 몇 분씩 돌 수 있고, 그때 로그가 같은 줄로 덮이면 쓸모가 없다. */
        if (strcmp(c->st.err, last_err) != 0) {
            Info_("cast: ch%d %s (%s)", idx, c->st.err, c->st.url);
            snprintf(last_err, sizeof(last_err), "%s", c->st.err);
        }
        /* 보드가 아직 켜라는 명령을 못 받았거나 rec 이 내려간 상태가 정상
         * 상황이다. 조용히 다시 시도한다. */
        for (int i = 0; i < 10 && c->st.want_open; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    c->st.running = 0;
}

} // namespace

//===========================================================================
// 공개 API
//===========================================================================

CAST_LINK *cast_link_create(void)
{
    CAST_LINK *self = new CAST_LINK();
    memset(&(self->adb), 0, sizeof(self->adb));
    snprintf(self->host, sizeof(self->host), "127.0.0.1");
    self->base_port = AVIO_CAST_BASE_PORT;
    self->dec_pref[0] = '\0';
    self->on_frame  = NULL;
    self->on_packet = NULL;
    self->user      = NULL;
    self->adb_last_tick = 0;
    for (i32_t i = 0; i < AVIO_CAST_MAX_CH; ++i) {
        CAST_CH_LINK *c = &self->ch[i];
        memset(&(c->st), 0, sizeof(c->st));
        memset(&(c->dec), 0, sizeof(c->dec));
        c->dec_open = 0;
        c->th = NULL;
        snprintf(c->st.url, sizeof(c->st.url), "tcp://%s:%d", self->host,
                 self->base_port + i);
    }
    cast_adb_init(&(self->adb));
    Info_("cast: %s", self->adb.note);
    return self;
}

void cast_link_destroy(CAST_LINK *self)
{
    if (self == NULL) { return; }
    for (i32_t i = 0; i < AVIO_CAST_MAX_CH; ++i) { cast_link_stop(self, i); }
    delete self;
}

void cast_link_config(CAST_LINK *self, const char *host, i32_t base_port,
                      const char *decoder_pref)
{
    if (self == NULL) { return; }
    if (host != NULL && host[0] != '\0') {
        snprintf(self->host, sizeof(self->host), "%s", host);
    }
    if (base_port > 0) { self->base_port = base_port; }
    if (decoder_pref != NULL) {
        snprintf(self->dec_pref, sizeof(self->dec_pref), "%s", decoder_pref);
    }
    for (i32_t i = 0; i < AVIO_CAST_MAX_CH; ++i) {
        snprintf(self->ch[i].st.url, sizeof(self->ch[i].st.url), "tcp://%s:%d",
                 self->host, self->base_port + i);
    }
}

void cast_link_set_sinks(CAST_LINK *self, CAST_LINK_FRAME_FN on_frame,
                         CAST_LINK_PKT_FN on_packet, void *user)
{
    if (self == NULL) { return; }
    self->on_frame  = on_frame;
    self->on_packet = on_packet;
    self->user      = user;
}

i32_t cast_link_start(CAST_LINK *self, i32_t ch)
{
    CAST_CH_LINK *c;

    if (self == NULL || ch < 0 || ch >= AVIO_CAST_MAX_CH) { return 0; }
    c = &self->ch[ch];
    if (c->th != NULL) { return 1; }   /* 이미 돈다 */

    /* 포워드는 붙기 전에. 실패는 치명적이지 않다 — 로컬 스트림이거나 이미
     * 손으로 걸어 둔 경우가 있다. rate limit 은 adb 를 매 채널마다 부르지
     * 않기 위한 것. */
    {
        const u32_t now = (u32_t)GetTickCount();
        if (self->adb_last_tick == 0 || (now - self->adb_last_tick) > CAST_ADB_RETRY_MS) {
            self->adb_last_tick = now;
            cast_adb_autoforward(&(self->adb), self->base_port, AVIO_CAST_MAX_CH);
        }
    }

    c->st.want_open = 1;
    c->st.frames    = 0;
    c->th = new std::thread(ch_thread, self, ch);
    return 1;
}

void cast_link_stop(CAST_LINK *self, i32_t ch)
{
    CAST_CH_LINK *c;

    if (self == NULL || ch < 0 || ch >= AVIO_CAST_MAX_CH) { return; }
    c = &self->ch[ch];
    c->st.want_open = 0;
    if (c->th != NULL) {
        if (c->th->joinable()) { c->th->join(); }
        delete c->th;
        c->th = NULL;
    }
}

const CAST_LINK_STAT *cast_link_stat(const CAST_LINK *self, i32_t ch)
{
    if (self == NULL || ch < 0 || ch >= AVIO_CAST_MAX_CH) { return NULL; }
    return &(self->ch[ch].st);
}

CAST_ADB *cast_link_adb(CAST_LINK *self)
{
    return (self != NULL) ? &(self->adb) : NULL;
}
