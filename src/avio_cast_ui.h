#ifndef avio_cast_uiH
#define avio_cast_uiH

#include "intf/lbx_intf_avio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Driver status panel, drawn inside the host's ImGui context. */
void LBX_API process_ui(LBX_HANDLE dev, LBX_HANDLE host_ui_handle);

#ifdef __cplusplus
}
#endif

#endif
