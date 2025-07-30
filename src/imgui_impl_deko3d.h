#pragma once

#include "imgui.h"

IMGUI_IMPL_API void ImGui_ImplDeko3d_Init();
IMGUI_IMPL_API void ImGui_ImplDeko3d_Shutdown();
IMGUI_IMPL_API void ImGui_ImplDeko3d_NewFrame();
IMGUI_IMPL_API void ImGui_ImplDeko3d_RenderDrawData(ImDrawData *drawData);

IMGUI_IMPL_API uint64_t ImGui_ImplDeko3d_UpdatePad();

IMGUI_IMPL_API int ImGui_ImplDeko3d_CreateTexture(const void *data, int width,
                                                  int height);
IMGUI_IMPL_API ImTextureID ImGui_ImplDeko3d_GetTextureId(int tex_id);
