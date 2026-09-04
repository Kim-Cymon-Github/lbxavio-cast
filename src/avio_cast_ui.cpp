//---------------------------------------------------------------------------
// avio_cast_ui.cpp — 드라이버 상태 패널.
//
// 창을 띄우고 보는 사람이 알아야 하는 것은 세 가지뿐이다 — adb 포워드가 섰는지,
// 채널이 붙었는지, 안 붙었으면 왜인지. 이 셋이 안 보이면 "그림이 안 나온다" 는
// 증상 하나로 보드·USB·포워드·인코더를 전부 의심하게 된다.
//---------------------------------------------------------------------------
#include "avio_cast_ui.h"
#include "avio_cast_main.h"
#include "cast_core.h"
#include "cast_link.h"

#include "imgui.h"
#include "system/lbx_log.h"

void LBX_API process_ui(LBX_HANDLE dev, LBX_HANDLE host_ui_handle)
{
    CAST_CORE *core;
    CAST_LINK *link;
    CAST_ADB  *adb;

    if (dev == NULL || host_ui_handle == NULL) { return; }
    core = avio_cast_core();
    if (core == NULL) { return; }
    link = cast_core_link(core);
    if (link == NULL) { return; }

    ImGui::SetCurrentContext((ImGuiContext *)host_ui_handle);

    adb = cast_link_adb(link);
    if (adb != NULL) {
        ImGui::TextUnformatted(adb->note);
        ImGui::SameLine();
        if (ImGui::SmallButton("forward")) {
            cast_adb_autoforward(adb, AVIO_CAST_BASE_PORT, AVIO_CAST_MAX_CH);
        }
    }

    if (ImGui::BeginTable("avio-cast", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("ch");
        ImGui::TableSetupColumn("state");
        ImGui::TableSetupColumn("size");
        ImGui::TableSetupColumn("frames");
        ImGui::TableSetupColumn("note");
        ImGui::TableHeadersRow();

        for (i32_t i = 0; i < AVIO_CAST_MAX_CH; ++i) {
            const CAST_LINK_STAT *st = cast_link_stat(link, i);
            if (st == NULL || (!st->running && st->frames == 0 && !st->want_open)) {
                continue;   /* 열지도 않은 채널로 표를 채우지 않는다 */
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (i == AVIO_CAST_SCREEN_CH) {
                ImGui::TextUnformatted("screen");
            } else {
                ImGui::Text("cam%d", i - AVIO_CAST_CAM_FIRST);
            }
            ImGui::TableNextColumn();
            if (st->connected) {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", st->codec);
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1.0f), "%s",
                                   st->want_open ? "connecting" : "closed");
            }
            ImGui::TableNextColumn();
            ImGui::Text("%dx%d", st->width, st->height);
            ImGui::TableNextColumn();
            ImGui::Text("%u", st->frames);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted((st->err[0] != '\0') ? st->err : st->url);
        }
        ImGui::EndTable();
    }
}
