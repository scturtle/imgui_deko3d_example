#include "imgui_impl_deko3d.h"

#include <deko3d.h>
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
  DkDevice g_device;
  DkMemBlock g_framebufferMemBlock;
  DkImage g_framebuffers[FB_NUM];
  DkSwapchain g_swapchain;

  DkMemBlock g_codeMemBlock;
  DkShader g_vertexShader;
  DkShader g_fragmentShader;

  DkMemBlock g_uboMemBlock;
  DkMemBlock g_vtxMemBlock[FB_NUM];
  DkMemBlock g_idxMemBlock[FB_NUM];

  DkMemBlock g_depthMemBlock;
  DkImage g_depthbuffer;

  DkMemBlock g_cmdbufMemBlock;
  DkCmdBuf g_cmdbuf;
  DkCmdList g_cmdsBindFramebuffer[FB_NUM];
  DkCmdList g_cmdsRender;

  DkQueue g_renderQueue;

  PadState pad;

  ImGui_ImplDeko3d_Data() { memset((void *)this, 0, sizeof(*this)); }
};

static ImGui_ImplDeko3d_Data *ImGui_ImplDeko3d_GetBackendData() {
  return ImGui::GetCurrentContext()
             ? (ImGui_ImplDeko3d_Data *)ImGui::GetIO().BackendRendererUserData
             : NULL;
}

static inline uint32_t align(uint32_t size, uint32_t align) {
  return (size + align - 1) & ~(align - 1);
}

static void loadShader(DkShader *shader, const char *path,
                       DkMemBlock g_codeMemBlock, uint32_t &codeMemOffset) {
  FILE *f = fopen(path, "rb");
  fseek(f, 0, SEEK_END);
  uint32_t size = ftell(f);
  rewind(f);

  uint32_t codeOffset = codeMemOffset;
  codeMemOffset += align(size, DK_SHADER_CODE_ALIGNMENT);

  fread((char *)dkMemBlockGetCpuAddr(g_codeMemBlock) + codeOffset, size, 1, f);
  fclose(f);

  DkShaderMaker shaderMaker;
  dkShaderMakerDefaults(&shaderMaker, g_codeMemBlock, codeOffset);
  dkShaderInitialize(shader, &shaderMaker);
}

static void InitDeko3Shaders(ImGui_ImplDeko3d_Data *bd) {
  DkDevice g_device = bd->g_device;
  // Create a memory block onto which we will load shader code
  DkMemBlockMaker memBlockMaker;
  IM_ASSERT(CODEMEMSIZE == align(CODEMEMSIZE, DK_MEMBLOCK_ALIGNMENT));
  dkMemBlockMakerDefaults(&memBlockMaker, g_device, CODEMEMSIZE);
  memBlockMaker.flags = DkMemBlockFlags_CpuUncached |
                        DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
  bd->g_codeMemBlock = dkMemBlockCreate(&memBlockMaker);

  // Load our shaders (both vertex and fragment)
  uint32_t codeMemOffset = 0;
  loadShader(&bd->g_vertexShader, "romfs:/shaders/imgui_vsh.dksh",
             bd->g_codeMemBlock, codeMemOffset);
  loadShader(&bd->g_fragmentShader, "romfs:/shaders/imgui_fsh.dksh",
             bd->g_codeMemBlock, codeMemOffset);
  IM_ASSERT(codeMemOffset + DK_SHADER_CODE_UNUSABLE_SIZE <= CODEMEMSIZE);
}

static void InitDeko3dSwapchain(ImGui_ImplDeko3d_Data *bd) {
  DkDevice g_device = bd->g_device;
  DkMemBlockMaker memBlockMaker;
  DkImageLayoutMaker layoutMaker;

  // Create depth memblock/buffer
  dkImageLayoutMakerDefaults(&layoutMaker, g_device);
  layoutMaker.flags = DkImageFlags_UsageRender | DkImageFlags_HwCompression;
  layoutMaker.format = DkImageFormat_Z24S8;
  layoutMaker.dimensions[0] = FB_WIDTH;
  layoutMaker.dimensions[1] = FB_HEIGHT;

  DkImageLayout depthLayout;
  dkImageLayoutInitialize(&depthLayout, &layoutMaker);

  uint32_t depthSize = dkImageLayoutGetSize(&depthLayout);
  uint32_t depthAlign = dkImageLayoutGetAlignment(&depthLayout);
  depthSize = align(depthSize, depthAlign);

  dkMemBlockMakerDefaults(&memBlockMaker, g_device, depthSize);
  memBlockMaker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
  bd->g_depthMemBlock = dkMemBlockCreate(&memBlockMaker);

  dkImageInitialize(&bd->g_depthbuffer, &depthLayout, bd->g_depthMemBlock, 0);

  // Create framebuffer memblock/buffer
  dkImageLayoutMakerDefaults(&layoutMaker, g_device);
  layoutMaker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent |
                      DkImageFlags_HwCompression;
  layoutMaker.format = DkImageFormat_RGBA8_Unorm;
  layoutMaker.dimensions[0] = FB_WIDTH;
  layoutMaker.dimensions[1] = FB_HEIGHT;

  DkImageLayout fbLayout;
  dkImageLayoutInitialize(&fbLayout, &layoutMaker);

  uint32_t fbSize = dkImageLayoutGetSize(&fbLayout);
  uint32_t fbAlign = dkImageLayoutGetAlignment(&fbLayout);
  fbSize = align(fbSize, fbAlign);

  dkMemBlockMakerDefaults(&memBlockMaker, g_device, FB_NUM * fbSize);
  memBlockMaker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
  bd->g_framebufferMemBlock = dkMemBlockCreate(&memBlockMaker);

  DkImage const *swapchainImages[FB_NUM];
  for (unsigned i = 0; i < FB_NUM; i++) {
    swapchainImages[i] = &bd->g_framebuffers[i];
    dkImageInitialize(&bd->g_framebuffers[i], &fbLayout,
                      bd->g_framebufferMemBlock, i * fbSize);
  }

  // Create a swapchain out of the framebuffers we've just initialized
  DkSwapchainMaker swapchainMaker;
  dkSwapchainMakerDefaults(&swapchainMaker, g_device, nwindowGetDefault(),
                           swapchainImages, FB_NUM);
  bd->g_swapchain = dkSwapchainCreate(&swapchainMaker);
}

static void InitDeko3dData(ImGui_ImplDeko3d_Data *bd) {
  // Create the device, which is the root object
  DkDeviceMaker deviceMaker;
  dkDeviceMakerDefaults(&deviceMaker);
  DkDevice g_device = bd->g_device = dkDeviceCreate(&deviceMaker);

  InitDeko3Shaders(bd);

  InitDeko3dSwapchain(bd);

  DkMemBlockMaker memBlockMaker;

  // Create a memory block for recording command lists
  dkMemBlockMakerDefaults(&memBlockMaker, g_device, CMDMEMSIZE);
  memBlockMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  bd->g_cmdbufMemBlock = dkMemBlockCreate(&memBlockMaker);

  // Create a command buffer object
  DkCmdBufMaker cmdbufMaker;
  dkCmdBufMakerDefaults(&cmdbufMaker, g_device);
  DkCmdBuf g_cmdbuf = bd->g_cmdbuf = dkCmdBufCreate(&cmdbufMaker);

  // Feed our memory to the command buffer
  dkCmdBufAddMemory(g_cmdbuf, bd->g_cmdbufMemBlock, 0, CMDMEMSIZE);

  // Generate a command list for each framebuffer, which will bind each of them
  // as a render target
  for (unsigned i = 0; i < FB_NUM; i++) {
    DkImageView imageView;
    dkImageViewDefaults(&imageView, &bd->g_framebuffers[i]);
    dkCmdBufBindRenderTarget(g_cmdbuf, &imageView, NULL);
    bd->g_cmdsBindFramebuffer[i] = dkCmdBufFinishList(g_cmdbuf);
  }

  // Declare structs that will be used for binding state
  DkViewport viewport = {0.0f, 0.0f, (float)FB_WIDTH, (float)FB_HEIGHT,
                         0.0f, 1.0f};
  DkScissor scissor = {0, 0, FB_WIDTH, FB_HEIGHT};
  DkShader const *shaders[] = {&bd->g_vertexShader, &bd->g_fragmentShader};
  DkRasterizerState rasterizerState;
  DkColorState colorState;
  DkColorWriteState colorWriteState;

  // Initialize state structs with the deko3d defaults
  dkRasterizerStateDefaults(&rasterizerState);
  dkColorStateDefaults(&colorState);
  dkColorWriteStateDefaults(&colorWriteState);

  // Generate the main rendering command list
  dkCmdBufSetViewports(g_cmdbuf, 0, &viewport, 1);
  dkCmdBufSetScissors(g_cmdbuf, 0, &scissor, 1);
  dkCmdBufClearColorFloat(g_cmdbuf, 0, DkColorMask_RGBA, 0.125f, 0.294f, 0.478f,
                          1.0f);
  dkCmdBufBindShaders(g_cmdbuf, DkStageFlag_GraphicsMask, shaders,
                      sizeof(shaders) / sizeof(shaders[0]));
  dkCmdBufBindRasterizerState(g_cmdbuf, &rasterizerState);
  dkCmdBufBindColorState(g_cmdbuf, &colorState);
  dkCmdBufBindColorWriteState(g_cmdbuf, &colorWriteState);
  dkCmdBufDraw(g_cmdbuf, DkPrimitive_Triangles, 3, 1, 0, 0);
  bd->g_cmdsRender = dkCmdBufFinishList(g_cmdbuf);

  // Create a queue, to which we will submit our command lists
  DkQueueMaker queueMaker;
  dkQueueMakerDefaults(&queueMaker, g_device);
  queueMaker.flags = DkQueueFlags_Graphics;
  bd->g_renderQueue = dkQueueCreate(&queueMaker);

  // Create a memory block for UBO
  dkMemBlockMakerDefaults(&memBlockMaker, g_device, UBOMEMSIZE);
  memBlockMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  bd->g_uboMemBlock = dkMemBlockCreate(&memBlockMaker);

  // Create memblock for vertex/index data for each frame
  for (int i = 0; i < FB_NUM; ++i) {
    dkMemBlockMakerDefaults(&memBlockMaker, g_device, VTXMEMSIZE);
    memBlockMaker.flags =
        DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    bd->g_vtxMemBlock[i] = dkMemBlockCreate(&memBlockMaker);

    dkMemBlockMakerDefaults(&memBlockMaker, g_device, IDXMEMSIZE);
    memBlockMaker.flags =
        DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    bd->g_idxMemBlock[i] = dkMemBlockCreate(&memBlockMaker);
  }

  // Initialize the default gamepad
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&bd->pad);
}

static void DestroyDeko3dData(ImGui_ImplDeko3d_Data *bd) {
  // Make sure the rendering queue is idle before destroying anything
  dkQueueWaitIdle(bd->g_renderQueue);
  // Destroy all the resources we've created
  dkQueueDestroy(bd->g_renderQueue);
  dkCmdBufDestroy(bd->g_cmdbuf);
  dkMemBlockDestroy(bd->g_cmdbufMemBlock);
  dkMemBlockDestroy(bd->g_codeMemBlock);
  dkMemBlockDestroy(bd->g_uboMemBlock);
  for (int i = 0; i < FB_NUM; ++i) {
    dkMemBlockDestroy(bd->g_vtxMemBlock[i]);
    dkMemBlockDestroy(bd->g_idxMemBlock[i]);
  }
  dkSwapchainDestroy(bd->g_swapchain);
  dkMemBlockDestroy(bd->g_framebufferMemBlock);
  dkMemBlockDestroy(bd->g_depthMemBlock);
  dkDeviceDestroy(bd->g_device);
}

static void ImGui_LoadSwitchFonts(ImGuiIO &io) {
  PlFontData standard, extended, chinese;
  static ImWchar extended_range[] = {0xe000, 0xe152};
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
  DestroyDeko3dData(bd);
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
