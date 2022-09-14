#include "imgui_impl_deko3d.h"

#include <deko3d.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>

// #define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
// #define GLM_FORCE_INTRINSICS
// #include <glm/gtc/matrix_transform.hpp>
// #include <glm/gtc/type_ptr.hpp>
// #include <glm/mat4x4.hpp>

#define FB_NUM 2

#define CODEMEMSIZE (4 * 1024)
#define CMDMEMSIZE (16 * 1024)
#define UBOMEMSIZE (4 * 1024)
#define VTXMEMSIZE (1024 * 1024)
#define IDXMEMSIZE (1024 * 1024)

// #define FB_WIDTH 1920
// #define FB_HEIGHT 1080
#define FB_WIDTH 1280
#define FB_HEIGHT 720

struct ImGui_ImplDeko3d_Data {
  dk::UniqueDevice g_device;

  dk::UniqueMemBlock g_fbMemBlock;
  dk::Image g_framebuffers[FB_NUM];
  dk::Swapchain g_swapchain;

  dk::UniqueMemBlock g_codeMemBlock;
  dk::Shader g_vertexShader;
  dk::Shader g_fragmentShader;

  dk::UniqueMemBlock g_uboMemBlock;
  dk::UniqueMemBlock g_vtxMemBlock[FB_NUM];
  dk::UniqueMemBlock g_idxMemBlock[FB_NUM];

  dk::UniqueMemBlock g_depthMemBlock;
  dk::Image g_depthbuffer;

  dk::UniqueMemBlock g_cmdbufMemBlock;
  DkCmdList g_cmdsBindFramebuffer[FB_NUM];
  DkCmdList g_cmdsRender;

  dk::UniqueCmdBuf g_cmdbuf;
  dk::UniqueQueue g_renderQueue;

  PadState pad;

  ImGui_ImplDeko3d_Data() { memset((void *)this, 0, sizeof(*this)); }
};

static ImGui_ImplDeko3d_Data *ImGui_ImplDeko3d_GetBackendData() {
  return ImGui::GetCurrentContext()
             ? (ImGui_ImplDeko3d_Data *)ImGui::GetIO().BackendRendererUserData
             : NULL;
}

static constexpr u32 align(u32 size, u32 align) {
  return (size + align - 1) & ~(align - 1);
}

static u32 loadShader(dk::Shader &shader, const char *path,
                      DkMemBlock g_codeMemBlock, u32 codeOffset) {
  FILE *f = fopen(path, "rb");
  fseek(f, 0, SEEK_END);
  u32 size = ftell(f);
  rewind(f);
  fread((char *)dkMemBlockGetCpuAddr(g_codeMemBlock) + codeOffset, size, 1, f);
  fclose(f);
  dk::ShaderMaker(g_codeMemBlock, codeOffset).initialize(shader);
  return align(size, DK_SHADER_CODE_ALIGNMENT);
}

static void InitDeko3Shaders(ImGui_ImplDeko3d_Data *bd) {
  DkDevice g_device = bd->g_device;
  // Create a memory block onto which we will load shader code
  static_assert(CODEMEMSIZE == align(CODEMEMSIZE, DK_MEMBLOCK_ALIGNMENT),
                "not aligned");
  bd->g_codeMemBlock =
      dk::MemBlockMaker(g_device, CODEMEMSIZE)
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                    DkMemBlockFlags_Code)
          .create();

  // Load our shaders (both vertex and fragment)
  u32 codeMemOffset = 0;
  codeMemOffset +=
      loadShader(bd->g_vertexShader, "romfs:/shaders/imgui_vsh.dksh",
                 bd->g_codeMemBlock, codeMemOffset);
  codeMemOffset +=
      loadShader(bd->g_fragmentShader, "romfs:/shaders/imgui_fsh.dksh",
                 bd->g_codeMemBlock, codeMemOffset);
  IM_ASSERT(codeMemOffset + DK_SHADER_CODE_UNUSABLE_SIZE <= CODEMEMSIZE);
}

static void InitDeko3dSwapchain(ImGui_ImplDeko3d_Data *bd) {
  DkDevice g_device = bd->g_device;

  // create depth layout
  dk::ImageLayout depthLayout;
  dk::ImageLayoutMaker(g_device)
      .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
      .setFormat(DkImageFormat_Z24S8)
      .setDimensions(FB_WIDTH, FB_HEIGHT)
      .initialize(depthLayout);

  u32 depthSize = depthLayout.getSize();
  u32 depthAlign = depthLayout.getAlignment();
  depthSize =
      align(depthSize, std::max(depthAlign, (u32)DK_MEMBLOCK_ALIGNMENT));

  // create depth memblock
  bd->g_depthMemBlock =
      dk::MemBlockMaker(g_device, depthSize)
          .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
          .create();

  // create depth image
  bd->g_depthbuffer.initialize(depthLayout, bd->g_depthMemBlock, 0);

  // create framebuffer layout
  dk::ImageLayout fbLayout;
  dk::ImageLayoutMaker(g_device)
      .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent |
                DkImageFlags_HwCompression)
      .setFormat(DkImageFormat_RGBA8_Unorm)
      .setDimensions(FB_WIDTH, FB_HEIGHT)
      .initialize(fbLayout);

  u32 fbSize = fbLayout.getSize();
  u32 fbAlign = fbLayout.getAlignment();
  fbSize = align(fbSize, std::max(fbAlign, (u32)DK_MEMBLOCK_ALIGNMENT));

  // create framebuffer memblock
  bd->g_fbMemBlock =
      dk::MemBlockMaker(g_device, FB_NUM * fbSize)
          .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
          .create();

  // create framebuffer images
  std::array<DkImage const *, FB_NUM> swapchainImages;
  for (unsigned i = 0; i < FB_NUM; i++) {
    swapchainImages[i] = &bd->g_framebuffers[i];
    bd->g_framebuffers[i].initialize(fbLayout, bd->g_fbMemBlock, i * fbSize);
  }

  // create a swapchain
  bd->g_swapchain =
      dk::SwapchainMaker(g_device, nwindowGetDefault(), swapchainImages)
          .create();
}

static void InitDeko3dData(ImGui_ImplDeko3d_Data *bd) {
  // Create the device, which is the root object
  bd->g_device = dk::DeviceMaker().create();
  DkDevice g_device = bd->g_device;
  // DkMemBlock g = bd->g_device;

  InitDeko3Shaders(bd);

  InitDeko3dSwapchain(bd);

  // Create a memory block for recording command lists
  static_assert(CMDMEMSIZE == align(CMDMEMSIZE, DK_MEMBLOCK_ALIGNMENT),
                "not aligned");
  bd->g_cmdbufMemBlock =
      dk::MemBlockMaker(g_device, CMDMEMSIZE)
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
          .create();

  // Create a command buffer object
  bd->g_cmdbuf = dk::CmdBufMaker(g_device).create();

  // Feed our memory to the command buffer
  bd->g_cmdbuf.addMemory(bd->g_cmdbufMemBlock, 0, CMDMEMSIZE);

  // Generate a command list for each framebuffer, which will bind each of them
  // as a render target
  for (unsigned i = 0; i < FB_NUM; i++) {
    dk::ImageView imageView(bd->g_framebuffers[i]);
    bd->g_cmdbuf.bindRenderTargets(&imageView, nullptr);
    bd->g_cmdsBindFramebuffer[i] = bd->g_cmdbuf.finishList();
  }

  // Declare structs that will be used for binding state
  DkViewport viewport = {0.0f, 0.0f, (float)FB_WIDTH, (float)FB_HEIGHT,
                         0.0f, 1.0f};
  DkScissor scissor = {0, 0, FB_WIDTH, FB_HEIGHT};

  // Generate the main rendering command list
  bd->g_cmdbuf.setViewports(0, viewport);
  bd->g_cmdbuf.setScissors(0, scissor);
  bd->g_cmdbuf.clearColor(0, DkColorMask_RGBA, 0.125f, 0.294f, 0.478f, 1.0f);
  bd->g_cmdbuf.bindShaders(DkStageFlag_GraphicsMask,
                           {&bd->g_vertexShader, &bd->g_fragmentShader});
  bd->g_cmdbuf.bindRasterizerState(dk::RasterizerState());
  bd->g_cmdbuf.bindColorState(dk::ColorState());
  bd->g_cmdbuf.bindColorWriteState(dk::ColorWriteState());
  bd->g_cmdbuf.draw(DkPrimitive_Triangles, 3, 1, 0, 0);
  bd->g_cmdsRender = bd->g_cmdbuf.finishList();

  // Create a queue, to which we will submit our command lists
  bd->g_renderQueue =
      dk::QueueMaker(g_device).setFlags(DkQueueFlags_Graphics).create();

  // Create a memory block for UBO
  bd->g_uboMemBlock =
      dk::MemBlockMaker(g_device, UBOMEMSIZE)
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
          .create();

  // Create memblock for vertex/index data for each frame
  for (int i = 0; i < FB_NUM; ++i) {
    bd->g_vtxMemBlock[i] =
        dk::MemBlockMaker(g_device, VTXMEMSIZE)
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();
    bd->g_idxMemBlock[i] =
        dk::MemBlockMaker(g_device, IDXMEMSIZE)
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();
  }

  // Initialize the default gamepad
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&bd->pad);
}

static void ImGui_LoadSwitchFonts(ImGuiIO &io) {
  PlFontData standard, extended, chinese;
  ImWchar extended_range[] = {0xe000, 0xe152};
  IM_ASSERT(
      R_SUCCEEDED(plGetSharedFontByType(&standard, PlSharedFontType_Standard)));
  IM_ASSERT(R_SUCCEEDED(
      plGetSharedFontByType(&extended, PlSharedFontType_NintendoExt)));
  IM_ASSERT(R_SUCCEEDED(
      plGetSharedFontByType(&chinese, PlSharedFontType_ChineseSimplified)));

  ImFontConfig font_cfg;
  font_cfg.FontDataOwnedByAtlas = false;
  io.Fonts->AddFontFromMemoryTTF(standard.address, standard.size, 20.0f,
                                 &font_cfg, io.Fonts->GetGlyphRangesDefault());
  font_cfg.MergeMode = true;
  io.Fonts->AddFontFromMemoryTTF(extended.address, extended.size, 20.0f,
                                 &font_cfg, extended_range);
  io.Fonts->AddFontFromMemoryTTF(
      chinese.address, chinese.size, 20.0f, &font_cfg,
      io.Fonts->GetGlyphRangesChineseSimplifiedCommon());

  unsigned char *px;
  int w, h, bpp;
  io.Fonts->GetTexDataAsAlpha8(&px, &w, &h, &bpp);
  io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
  io.Fonts->Build();
  IM_ASSERT(io.Fonts->IsBuilt());
}

bool ImGui_ImplDeko3d_Init() {
  ImGuiIO &io = ImGui::GetIO();
  IM_ASSERT(io.BackendRendererUserData == NULL &&
            "Already initialized a renderer backend!");

  ImGui_LoadSwitchFonts(io);

  io.BackendRendererName = "imgui_impl_deko3d";
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  io.MouseDrawCursor = false;

  io.DisplaySize = ImVec2(FB_WIDTH, FB_HEIGHT);
  io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

  ImGui_ImplDeko3d_Data *bd = new ImGui_ImplDeko3d_Data();
  io.BackendRendererUserData = (void *)bd;
  InitDeko3dData(bd);
  return true;
}

void ImGui_ImplDeko3d_Shutdown() {
  ImGui_ImplDeko3d_Data *bd = ImGui_ImplDeko3d_GetBackendData();
  dkQueueWaitIdle(bd->g_renderQueue);
  delete bd;
}

uint64_t ImGui_ImplDeko3d_UpdatePad() {
  ImGuiIO &io = ImGui::GetIO();
  ImGui_ImplDeko3d_Data *bd = ImGui_ImplDeko3d_GetBackendData();

  padUpdate(&bd->pad);
  const u64 down = padGetButtonsDown(&bd->pad);
  const u64 up = padGetButtonsUp(&bd->pad);

  HidTouchScreenState state = {0};
  hidGetTouchScreenStates(&state, 1);
  static bool touch_down = false;
  if (state.count < 1) {
    if (touch_down) {
      touch_down = false;
      io.AddMouseButtonEvent(0, false);
    }
  } else {
    float x, y;
    x = state.touches[0].x;
    y = state.touches[0].y;
    io.AddMousePosEvent(x, y);
    touch_down = true;
    io.AddMouseButtonEvent(0, true);
  }

  constexpr int mapping[][2] = {
      {ImGuiKey_GamepadFaceDown, HidNpadButton_A},
      {ImGuiKey_GamepadFaceRight, HidNpadButton_B},
      {ImGuiKey_GamepadFaceUp, HidNpadButton_X},
      {ImGuiKey_GamepadFaceLeft, HidNpadButton_Y},
      {ImGuiKey_GamepadL1, HidNpadButton_L},
      {ImGuiKey_GamepadR1, HidNpadButton_R},
      {ImGuiKey_GamepadL2, HidNpadButton_ZL},
      {ImGuiKey_GamepadR2, HidNpadButton_ZR},
      {ImGuiKey_GamepadStart, HidNpadButton_Plus},
      {ImGuiKey_GamepadBack, HidNpadButton_Minus},
      {ImGuiKey_GamepadDpadLeft, HidNpadButton_Left},
      {ImGuiKey_GamepadDpadRight, HidNpadButton_Right},
      {ImGuiKey_GamepadDpadUp, HidNpadButton_Up},
      {ImGuiKey_GamepadDpadDown, HidNpadButton_Down},
      {ImGuiKey_GamepadLStickLeft, HidNpadButton_StickLLeft},
      {ImGuiKey_GamepadLStickRight, HidNpadButton_StickLRight},
      {ImGuiKey_GamepadLStickUp, HidNpadButton_StickLUp},
      {ImGuiKey_GamepadLStickDown, HidNpadButton_StickLDown},
  };

  for (int i = 0; i < IM_ARRAYSIZE(mapping); ++i) {
    int im_k = mapping[i][0], nx_k = mapping[i][1];
    if (down & nx_k)
      io.AddKeyEvent(im_k, true);
    else if (up & nx_k)
      io.AddKeyEvent(im_k, false);
  }

  return up;
}

void ImGui_ImplDeko3d_NewFrame() {
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(FB_WIDTH, FB_HEIGHT);
  // io.DeltaTime = 1.0f / 60.0f; // TODO
}

static void graphicsUpdate(ImGui_ImplDeko3d_Data *bd) {
  // Acquire a framebuffer from the swapchain (and wait for it to be available)
  int slot = dkQueueAcquireImage(bd->g_renderQueue, bd->g_swapchain);
  // Run the command list that binds said framebuffer as a render target
  dkQueueSubmitCommands(bd->g_renderQueue, bd->g_cmdsBindFramebuffer[slot]);
  // Run the main rendering command list
  dkQueueSubmitCommands(bd->g_renderQueue, bd->g_cmdsRender);
  // Now that we are done rendering, present it to the screen
  dkQueuePresentImage(bd->g_renderQueue, bd->g_swapchain, slot);
}

void ImGui_ImplDeko3d_RenderDrawData(ImDrawData *draw_data) {
  ImGui_ImplDeko3d_Data *bd = ImGui_ImplDeko3d_GetBackendData();
  graphicsUpdate(bd);
}
