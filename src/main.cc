#include "imgui_impl_deko3d.h"
#include <imgui.h>
#include <switch.h>

extern "C" void userAppInit() {
  setsysInitialize();
  plInitialize(PlServiceType_User);
  romfsInit();
#ifdef DEBUG
  socketInitializeDefault();
  nxlinkStdio();
#endif
}

extern "C" void userAppExit() {
  setsysExit();
  plExit();
  romfsExit();
#ifdef DEBUG
  socketExit();
#endif
}

int main(int argc, char *argv[]) {
  // Setup Dear ImGui
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGuiIO &io = ImGui::GetIO();
  (void)io;

  ImGui_ImplDeko3d_Init();

  while (appletMainLoop()) {
    u64 down = ImGui_ImplDeko3d_UpdatePad();
    if (down & HidNpadButton_Plus)
      break;

    ImGui_ImplDeko3d_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Hello, world!");
    ImGui::End();

    ImGui::Render();
    ImGui_ImplDeko3d_RenderDrawData(ImGui::GetDrawData());
  }

  ImGui_ImplDeko3d_Shutdown();
  ImGui::DestroyContext();
  return 0;
}
