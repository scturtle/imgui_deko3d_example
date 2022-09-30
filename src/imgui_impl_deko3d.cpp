#include "imgui_impl_deko3d.h"

#include <deko3d.hpp>
#include <stdio.h>
#include <switch.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>

#define FB_NUM 2
#define FB_WIDTH 1280
#define FB_HEIGHT 720
#define CODEMEMSIZE (4 * 1024)
#define CMDMEMSIZE (16 * 1024)

struct VertUBO {
  glm::mat4 proj;
};

struct Descriptors {
  dk::SamplerDescriptor sampler;
  dk::ImageDescriptor image;
};

struct ImGui_ImplDeko3d_Data {
  dk::UniqueDevice device;
  dk::UniqueQueue queue;

  dk::UniqueMemBlock fbMemBlock;
  dk::Image framebuffers[FB_NUM];
  dk::UniqueSwapchain swapchain;

  dk::UniqueMemBlock depthMemBlock;
  dk::Image depthbuffer;

  dk::UniqueMemBlock codeMemBlock;
  dk::Shader vertexShader;
  dk::Shader fragmentShader;

  dk::UniqueMemBlock uboMemBlock;
  dk::UniqueMemBlock vtxMemBlock[FB_NUM];
  dk::UniqueMemBlock idxMemBlock[FB_NUM];

  dk::UniqueMemBlock descriptorsMemBlock;
  dk::UniqueMemBlock fontImageMemBlock;
  DkResHandle fontTextureHandle;

  dk::UniqueMemBlock cmdbufMemBlock[FB_NUM];
  dk::UniqueCmdBuf cmdbuf[FB_NUM];

  PadState pad;
  u64 last_tick = armGetSystemTick();
};

static ImGui_ImplDeko3d_Data *getBackendData() {
  return ImGui::GetCurrentContext()
             ? (ImGui_ImplDeko3d_Data *)ImGui::GetIO().BackendRendererUserData
             : nullptr;
}

static constexpr u32 align(u32 size, u32 align) {
  return (size + align - 1) & ~(align - 1);
}

static u32 loadShader(dk::Shader &shader, const char *path,
                      DkMemBlock codeMemBlock, u32 codeOffset) {
  FILE *f = fopen(path, "rb");
  fseek(f, 0, SEEK_END);
  u32 size = ftell(f);
  rewind(f);
  fread((char *)dkMemBlockGetCpuAddr(codeMemBlock) + codeOffset, size, 1, f);
  fclose(f);
  // init sahder
  dk::ShaderMaker(codeMemBlock, codeOffset).initialize(shader);
  return align(size, DK_SHADER_CODE_ALIGNMENT);
}

static void InitDeko3Shaders(ImGui_ImplDeko3d_Data *bd) {
  DkDevice device = bd->device;
  // create memory block for shader code
  static_assert(CODEMEMSIZE == align(CODEMEMSIZE, DK_MEMBLOCK_ALIGNMENT), "");
  bd->codeMemBlock =
      dk::MemBlockMaker(device, CODEMEMSIZE)
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                    DkMemBlockFlags_Code)
          .create();

  // load shaders
  u32 codeMemOffset = 0;
  codeMemOffset += loadShader(bd->vertexShader, "romfs:/shaders/imgui_vsh.dksh",
                              bd->codeMemBlock, codeMemOffset);
  codeMemOffset +=
      loadShader(bd->fragmentShader, "romfs:/shaders/imgui_fsh.dksh",
                 bd->codeMemBlock, codeMemOffset);
  IM_ASSERT(codeMemOffset + DK_SHADER_CODE_UNUSABLE_SIZE <= CODEMEMSIZE);
}

static void InitDeko3dSwapchain(ImGui_ImplDeko3d_Data *bd) {
  DkDevice device = bd->device;

  // create depth layout
  dk::ImageLayout depthLayout;
  dk::ImageLayoutMaker(device)
      .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
      .setFormat(DkImageFormat_Z24S8)
      .setDimensions(FB_WIDTH, FB_HEIGHT)
      .initialize(depthLayout);

  // create depth memblock
  bd->depthMemBlock =
      dk::MemBlockMaker(device, align(align(depthLayout.getSize(),
                                            depthLayout.getAlignment()),
                                      (u32)DK_MEMBLOCK_ALIGNMENT))
          .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
          .create();

  // create depth image
  bd->depthbuffer.initialize(depthLayout, bd->depthMemBlock, 0);

  // create framebuffer layout
  dk::ImageLayout fbLayout;
  dk::ImageLayoutMaker(device)
      .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent |
                DkImageFlags_HwCompression)
      .setFormat(DkImageFormat_RGBA8_Unorm)
      .setDimensions(FB_WIDTH, FB_HEIGHT)
      .initialize(fbLayout);

  u32 fbSize = align(align(fbLayout.getSize(), fbLayout.getAlignment()),
                     (u32)DK_MEMBLOCK_ALIGNMENT);

  // create framebuffer memblock
  bd->fbMemBlock =
      dk::MemBlockMaker(device, FB_NUM * fbSize)
          .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
          .create();

  // create framebuffer images
  std::array<DkImage const *, FB_NUM> swapchainImages;
  for (unsigned i = 0; i < FB_NUM; i++) {
    swapchainImages[i] = &bd->framebuffers[i];
    bd->framebuffers[i].initialize(fbLayout, bd->fbMemBlock, i * fbSize);
  }

  // create a swapchain
  bd->swapchain =
      dk::SwapchainMaker(device, nwindowGetDefault(), swapchainImages).create();

  // Create command buffer and memory block
  for (int i = 0; i < FB_NUM; ++i) {
    bd->cmdbufMemBlock[i] =
        dk::MemBlockMaker(device, align(CMDMEMSIZE, DK_MEMBLOCK_ALIGNMENT))
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();
    bd->cmdbuf[i] = dk::CmdBufMaker(device).create();
    bd->cmdbuf[i].addMemory(bd->cmdbufMemBlock[i], 0, CMDMEMSIZE);
  }
}

static void ImGui_LoadSwitchFonts(ImGuiIO &io) {
  PlFontData standard, extended, chinese, korean;
  ImWchar extended_range[] = {0xe000, 0xe152};
  bool ok = R_SUCCEEDED(
                plGetSharedFontByType(&standard, PlSharedFontType_Standard)) &&
            R_SUCCEEDED(plGetSharedFontByType(&extended,
                                              PlSharedFontType_NintendoExt)) &&
            R_SUCCEEDED(plGetSharedFontByType(
                &chinese, PlSharedFontType_ChineseSimplified)) &&
            R_SUCCEEDED(plGetSharedFontByType(&korean, PlSharedFontType_KO));
  IM_ASSERT(ok);

  ImFontConfig font_cfg;
  font_cfg.FontDataOwnedByAtlas = false;
  io.Fonts->AddFontFromMemoryTTF(standard.address, standard.size, 18.0f,
                                 &font_cfg, io.Fonts->GetGlyphRangesDefault());
  font_cfg.MergeMode = true;
  io.Fonts->AddFontFromMemoryTTF(extended.address, extended.size, 18.0f,
                                 &font_cfg, extended_range);
  // NOTE: uncomment to enable Chinese/Korean support but with slow startup time
  /*
  io.Fonts->AddFontFromMemoryTTF(
      chinese.address, chinese.size, 18.0f, &font_cfg,
      io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
  io.Fonts->AddFontFromMemoryTTF(korean.address, korean.size, 18.0f, &font_cfg,
                                 io.Fonts->GetGlyphRangesKorean());
  */

  io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
  io.Fonts->Build();
}

static void InitDeko3dFontTexture(ImGui_ImplDeko3d_Data *bd) {
  DkDevice device = bd->device;
  dk::CmdBuf cmdbuf = bd->cmdbuf[0];

  // initialize memblock for descriptors
  bd->descriptorsMemBlock =
      dk::MemBlockMaker(device,
                        align(sizeof(Descriptors), DK_MEMBLOCK_ALIGNMENT))
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
          .create();
  Descriptors *descCpuAddr =
      (Descriptors *)bd->descriptorsMemBlock.getCpuAddr();
  DkGpuAddr descGpuAddr = bd->descriptorsMemBlock.getGpuAddr();

  // bind all descriptors
  cmdbuf.bindSamplerDescriptorSet(descGpuAddr, 1);
  constexpr int offset = offsetof(Descriptors, image);
  cmdbuf.bindImageDescriptorSet(descGpuAddr + offset, 1);

  // generate font texture id
  ImGuiIO &io = ImGui::GetIO();
  ImGui_LoadSwitchFonts(io);
  bd->fontTextureHandle = dkMakeTextureHandle(0, 0);
  io.Fonts->SetTexID(&bd->fontTextureHandle);

  // copy font data to scratch buffer
  unsigned char *pixels;
  int width, height, channel;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &channel);
  dk::UniqueMemBlock scratchMemBlock =
      dk::MemBlockMaker(device,
                        align(width * height * channel, DK_MEMBLOCK_ALIGNMENT))
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
          .create();
  memcpy(scratchMemBlock.getCpuAddr(), pixels, width * height * channel);

  // create font image memblock
  dk::ImageLayout layout;
  dk::ImageLayoutMaker{device}
      .setFlags(0)
      .setFormat(DkImageFormat_RGBA8_Unorm)
      .setDimensions(width, height)
      .initialize(layout);
  bd->fontImageMemBlock =
      dk::MemBlockMaker{
          device, align(layout.getSize(), std::max(layout.getAlignment(),
                                                   (u32)DK_MEMBLOCK_ALIGNMENT))}
          .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
          .create();

  dk::Image fontImage;
  fontImage.initialize(layout, bd->fontImageMemBlock, 0);

  // init font descriptors
  descCpuAddr->image.initialize(fontImage);
  descCpuAddr->sampler.initialize(
      dk::Sampler{}
          .setFilter(DkFilter_Linear, DkFilter_Linear)
          .setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge,
                       DkWrapMode_ClampToEdge));

  // copy from scratch buffer to font image
  cmdbuf.copyBufferToImage({scratchMemBlock.getGpuAddr()},
                           dk::ImageView{fontImage},
                           {0, 0, 0, u32(width), u32(height), 1});

  bd->queue.submitCommands(cmdbuf.finishList());
  bd->queue.waitIdle();
}

static void InitDeko3dData(ImGui_ImplDeko3d_Data *bd) {
  bd->device = dk::DeviceMaker().create();
  bd->queue =
      dk::QueueMaker(bd->device).setFlags(DkQueueFlags_Graphics).create();

  InitDeko3Shaders(bd);

  InitDeko3dSwapchain(bd);

  InitDeko3dFontTexture(bd);

  // Create a memory block for UBO
  bd->uboMemBlock =
      dk::MemBlockMaker(bd->device,
                        align(align(sizeof(VertUBO), DK_UNIFORM_BUF_ALIGNMENT),
                              DK_MEMBLOCK_ALIGNMENT))
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
          .create();
}

void ImGui_ImplDeko3d_Init() {
  ImGuiIO &io = ImGui::GetIO();
  IM_ASSERT(!io.BackendRendererUserData &&
            "Already initialized a renderer backend!");

  io.BackendPlatformName = "Switch";
  io.BackendRendererName = "imgui_impl_deko3d";
  io.IniFilename = nullptr;
  io.MouseDrawCursor = false;
  io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

  ImGui_ImplDeko3d_Data *bd = new ImGui_ImplDeko3d_Data();
  io.BackendRendererUserData = (void *)bd;

  // init all resources of deko3d
  InitDeko3dData(bd);

  // init the gamepad
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&bd->pad);
}

void ImGui_ImplDeko3d_Shutdown() {
  ImGui_ImplDeko3d_Data *bd = getBackendData();
  dkQueueWaitIdle(bd->queue);
  delete bd;
}

uint64_t ImGui_ImplDeko3d_UpdatePad() {
  ImGuiIO &io = ImGui::GetIO();
  ImGui_ImplDeko3d_Data *bd = getBackendData();

  // fetch gamepad state
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
    auto [im_k, nx_k] = mapping[i];
    if (down & nx_k)
      io.AddKeyEvent((ImGuiKey)im_k, true);
    else if (up & nx_k)
      io.AddKeyEvent((ImGuiKey)im_k, false);
  }
  return up;
}

void ImGui_ImplDeko3d_NewFrame() {
  ImGuiIO &io = ImGui::GetIO();
  ImGui_ImplDeko3d_Data *bd = getBackendData();
  io.DisplaySize = ImVec2(FB_WIDTH, FB_HEIGHT);
  u64 tick = armGetSystemTick();
  io.DeltaTime = armTicksToNs(tick - bd->last_tick) / 1e9;
  bd->last_tick = tick;
}

static void SetupDeko3dRenderState(ImGui_ImplDeko3d_Data *bd, dk::CmdBuf cmdbuf,
                                   int slot) {
  ImGuiIO &io = ImGui::GetIO();
  dk::ImageView imageView(bd->framebuffers[slot]);
  dk::ImageView depthView(bd->depthbuffer);
  cmdbuf.bindRenderTargets(&imageView, &depthView);
  cmdbuf.setViewports(0, {{0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y}});
  cmdbuf.setScissors(
      0, DkScissor{0, 0, (u32)io.DisplaySize.x, (u32)io.DisplaySize.y});
  cmdbuf.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
  cmdbuf.clearDepthStencil(true, 1.0f, 0xFF, 0);
  cmdbuf.bindShaders(DkStageFlag_GraphicsMask,
                     {&bd->vertexShader, &bd->fragmentShader});
  cmdbuf.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
  cmdbuf.bindColorState(dk::ColorState{}.setBlendEnable(0, true));
  cmdbuf.bindColorWriteState(dk::ColorWriteState{});
  cmdbuf.bindDepthStencilState(
      dk::DepthStencilState{}.setDepthTestEnable(false));
  cmdbuf.bindBlendStates(0, dk::BlendState{});

  VertUBO vertUBO;
  vertUBO.proj = glm::orthoRH_ZO(0.0f, io.DisplaySize.x, io.DisplaySize.y, 0.0f,
                                 -1.0f, 1.0f);
  DkGpuAddr vertUBOGpuAddr = bd->uboMemBlock.getGpuAddr();
  size_t vertUBOSize = align(sizeof(vertUBO), DK_UNIFORM_BUF_ALIGNMENT);
  cmdbuf.bindUniformBuffer(DkStage_Vertex, 0, vertUBOGpuAddr, vertUBOSize);
  cmdbuf.pushConstants(vertUBOGpuAddr, vertUBOSize, 0, sizeof(VertUBO),
                       &vertUBO);

  cmdbuf.bindVtxAttribState({
      // clang-format off
      DkVtxAttribState{0, 0, offsetof(ImDrawVert, pos), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
      DkVtxAttribState{0, 0, offsetof(ImDrawVert, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
      DkVtxAttribState{0, 0, offsetof(ImDrawVert, col), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
      // clang-format on
  });
  cmdbuf.bindVtxBufferState({DkVtxBufferState{sizeof(ImDrawVert), 0}});
  bd->queue.submitCommands(cmdbuf.finishList());
}

void ImGui_ImplDeko3d_RenderDrawData(ImDrawData *drawData) {
  ImGui_ImplDeko3d_Data *bd = getBackendData();

  // acquire a framebuffer from the swapchain (and wait for it to be available)
  int slot = dkQueueAcquireImage(bd->queue, bd->swapchain);
  dk::CmdBuf cmdbuf = bd->cmdbuf[slot];
  cmdbuf.clear();
  SetupDeko3dRenderState(bd, cmdbuf, slot);

  // init or grow vertex/index buffer if not enough
  size_t totVtxSize = std::max(drawData->TotalVtxCount * sizeof(ImDrawVert),
                               (size_t)DK_MEMBLOCK_ALIGNMENT * 16);
  if (!bd->vtxMemBlock[slot] || bd->vtxMemBlock[slot].getSize() < totVtxSize) {
    bd->vtxMemBlock[slot] = nullptr; // destroy
    bd->vtxMemBlock[slot] =
        dk::MemBlockMaker(bd->device,
                          align(2 * totVtxSize, DK_MEMBLOCK_ALIGNMENT))
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();
  }
  size_t totIdxSize = std::max(drawData->TotalIdxCount * sizeof(ImDrawIdx),
                               (size_t)DK_MEMBLOCK_ALIGNMENT * 16);
  if (!bd->idxMemBlock[slot] || bd->idxMemBlock[slot].getSize() < totIdxSize) {
    bd->idxMemBlock[slot] = nullptr; // destroy
    bd->idxMemBlock[slot] =
        dk::MemBlockMaker(bd->device,
                          align(2 * totIdxSize, DK_MEMBLOCK_ALIGNMENT))
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();
  }

  // bind vertex/index buffer
  static_assert(sizeof(ImDrawIdx) == sizeof(uint16_t), "");
  cmdbuf.bindVtxBuffer(0, bd->vtxMemBlock[slot].getGpuAddr(),
                       bd->vtxMemBlock[slot].getSize());
  cmdbuf.bindIdxBuffer(DkIdxFormat_Uint16, bd->idxMemBlock[slot].getGpuAddr());

  DkResHandle boundTextureHandle = ~0;
  size_t vtxOffset = 0, idxOffset = 0;
  for (int i = 0; i < drawData->CmdListsCount; ++i) {
    const ImDrawList &cmdList = *drawData->CmdLists[i];
    size_t vtxSize = cmdList.VtxBuffer.Size * sizeof(ImDrawVert);
    size_t idxSize = cmdList.IdxBuffer.Size * sizeof(ImDrawIdx);
    // copy vertex/index data to buffer
    memcpy((char *)bd->vtxMemBlock[slot].getCpuAddr() + vtxOffset,
           cmdList.VtxBuffer.Data, vtxSize);
    memcpy((char *)bd->idxMemBlock[slot].getCpuAddr() + idxOffset,
           cmdList.IdxBuffer.Data, idxSize);

    for (auto const &cmd : cmdList.CmdBuffer) {
      ImVec4 clip = cmd.ClipRect;
      cmdbuf.setScissors(0,
                         DkScissor{u32(clip.x), u32(clip.y),
                                   u32(clip.z - clip.x), u32(clip.w - clip.y)});
      DkResHandle textureHandle = *(DkResHandle *)cmd.TextureId;
      // check if we need to bind a new texture
      if (textureHandle != boundTextureHandle) {
        boundTextureHandle = textureHandle;
        cmdbuf.bindTextures(DkStage_Fragment, 0, textureHandle);
      }
      // draw the triangle list
      cmdbuf.drawIndexed(DkPrimitive_Triangles, cmd.ElemCount, 1,
                         cmd.IdxOffset + idxOffset / sizeof(ImDrawIdx),
                         cmd.VtxOffset + vtxOffset / sizeof(ImDrawVert), 0);
    }
    vtxOffset += vtxSize;
    idxOffset += idxSize;
  }

  cmdbuf.barrier(DkBarrier_Fragments, 0);
  cmdbuf.discardDepthStencil();

  bd->queue.submitCommands(cmdbuf.finishList());
  bd->queue.presentImage(bd->swapchain, slot);
}
