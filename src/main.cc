#include <cstdlib>

#include <deko3d.hpp>
#include <imgui.h>
#include <switch.h>

#include "imgui_impl_deko3d.h"
#include "util.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NEON
#define STBI_ONLY_JPEG
#include "stb_image.h"

extern "C" void userAppInit() {
  plInitialize(PlServiceType_User);
  romfsInit();
#ifdef DEBUG
  socketInitializeDefault();
  nxlinkStdio();
#endif
}

extern "C" void userAppExit() {
  plExit();
  romfsExit();
#ifdef DEBUG
  socketExit();
#endif
}

ImTextureID load_background() {
  int width, height, nchan;
  auto data =
      stbi_load("romfs:/res/background.jpg", &width, &height, &nchan, 4);
  assert(data);
  auto bg_tex_id = ImGui_ImplDeko3d_CreateTexture(data, width, height);
  stbi_image_free(data);
  return ImGui_ImplDeko3d_GetTextureId(bg_tex_id);
}

int main(int argc, char *argv[]) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();

  ImGui_ImplDeko3d_Init();

  auto bd_tex_id = load_background();

  while (appletMainLoop()) {
    u64 down = ImGui_ImplDeko3d_UpdatePad();
    if (down & HidNpadButton_Plus) // "+" to exit
      break;

    ImGui_ImplDeko3d_NewFrame();
    ImGui::NewFrame();

    ImGui::GetBackgroundDrawList()->AddImage(bd_tex_id, ImVec2(0, 0),
                                             ImGui::GetIO().DisplaySize);

    bool open;
    ImGui::ShowDemoWindow(&open);

    ImGui::Render();
    ImGui_ImplDeko3d_RenderDrawData(ImGui::GetDrawData());
  }

  ImGui_ImplDeko3d_Shutdown();
  ImGui::DestroyContext();
  return 0;
}
