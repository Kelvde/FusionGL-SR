// FusionGL - FSR3 帧生成（P0 探测 + P1 每帧插值）
// P0: 验证 FSR3.1.6 帧生成上下文能否在目标 GPU 上创建成功。
// P1: 常驻 context，每帧 Prepare(光流) + Dispatch(生成插值帧到独立 output)。
// 边界：全部受 MOBILEGL_FFX_PROBE 编译开关 + 运行时开关控制，默认关闭；
//       P1 生成的插值帧当前只落盘验证，不接 present、不改变任何画面。
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

/* FusionGL 超分：FSR3 upscaler（AMD 技术，与帧生成同一 bridge 体系）。
 * 渲染分辨率缩放由渲染器 viewport 完成（FUSION_UPSCALE_PCT），本模块把
 * 缩放区域画面经 FSR3 upscaler 重建到全屏（同类超分能力）。 */

namespace MobileGL::MG_Backend::DirectVulkan {
    // 每个进程只尝试一次。返回 true 表示本次执行了探测。
    // P0：create 成功后 context 常驻（不再 destroy），供 P1 每帧复用。
    bool FfxProbeMaybeRun(VkPhysicalDevice physicalDevice, VkDevice device,
                          VkFormat colorFormat, uint32_t width, uint32_t height);

    // P1：每帧插值。在调用方已 begin 的 commandBuffer 上记录 Prepare + Dispatch。
    //   color      : 本帧场景色图（display 尺寸，colorFormat，SAMPLED）
    //   depth      : 本帧深度（R32_SFLOAT，render 尺寸，SAMPLED）—— 由调用方转换好
    //   motionVec  : 运动矢量（R16G16_SFLOAT，render 尺寸，SAMPLED）—— 可传全零 dummy
    //   output     : 插值输出图（display 尺寸，colorFormat，SAMPLED|STORAGE），必须与输入不同
    //   frameId    : 单调递增帧号（从 1 开始，reset 时传 1 并置 reset=true）
    // 返回 true 表示本帧成功记录了一对 prepare+dispatch。
    bool FfxFramegenRecord(VkCommandBuffer commandBuffer,
                           VkImage colorImage, VkFormat colorFormat,
                           VkImage depthImage,
                           VkImage motionImage,
                           VkImage outputImage,
                           uint32_t displayWidth, uint32_t displayHeight,
                           uint32_t renderWidth, uint32_t renderHeight,
                           uint64_t frameId, bool reset);

    // P2：把已生成的插值帧作为"额外一帧"呈现到屏幕上，实现真正的帧率翻倍。
    // 在调用方 present 完真实帧之后调用：自己 acquire 一张 swapchain image，
    // 把 FSR3 output 拷贝进去，再 present 一次。
    // 返回取得的 swapchain image 索引（调用方需要更新自己的 layout/age 跟踪），
    // 返回 UINT32_MAX 表示本次没有呈现（任何一步失败都静默跳过，绝不影响正常出画）。
    uint32_t FfxPresentGeneratedFrame(VkDevice device, VkQueue graphicsQueue, uint32_t graphicsFamily,
                                      VkQueue presentQueue, VkSwapchainKHR swapchain,
                                      const VkImage* swapchainImages, uint32_t imageCount,
                                      uint32_t width, uint32_t height,
                                      VkFence frameFence);

    // P2b：为"插值帧"取一张额外的 swapchain 图。0 超时——没空闲图就返回 false（这一帧不插），
    // 绝不阻塞主线程。成功后 outAcquire 是这张图的 acquire 信号量，outFinished 是本次提交
    // 要 signal 的完成信号量（present 时等它）。
    bool FfxAcquireInterpTarget(VkDevice device, VkSwapchainKHR swapchain,
                                const VkImage* swapchainImages, uint32_t imageCount,
                                uint32_t mainImageIndex, VkImage* outImage, uint32_t* outIndex,
                                VkSemaphore* outAcquire, VkSemaphore* outFinished);

    // P2b：在主渲染命令缓冲里录制"插值帧 output -> 目标交换链图"的拷贝并把目标转成
    // PRESENT_SRC。与主渲染同一次提交，因此不需要 CPU 等待。
    // 返回 false 表示没录制成功 —— 此时调用方绝不能 present，且必须把这张图归还
    // （直接 present 一次即可），否则图泄漏，几帧后主流程将永远取不到图（ANR）。
    bool FfxRecordInterpBlit(VkCommandBuffer cmd, VkImage dst, uint32_t width, uint32_t height);

    // FusionGL 统一诊断落盘（/sdcard/FusionGL/ffx_probe.log）。供 present 路径等
    // 无 FFXLOG 宏的翻译单元使用。
    void FfxDiagLog(const char* fmt, ...);

    // P1 offscreen resources (validation-only: output is not presented).
    extern bool g_fgContextReady;
    extern bool g_fgPresentEnabled;
    VkImage FfxFramegenDepthImage();
    VkImage FfxFramegenMotionImage();
    VkImage FfxFramegenOutputImage();
    // RGBA8 中转图：本帧画面先 blit 成这个格式再喂 FSR3（FSR3 只认 RGBA8/RGBA16F）。
    VkImage FfxFramegenColorInImage();
    VkFormat FfxFramegenColorFormat();
    // 渲染分辨率缩放系数（FUSION_UPSCALE_PCT / 100，100% 时为 1.0）。
    // viewport/scissor 按它缩放（渲染量直降），present 侧把小区域拉伸回全屏。
    float FfxUpscaleFactor();

    // FusionSR 超分：Catmull-Rom 空间放大 + 时间积累 + 锐化。
    // 在插帧之前调用，把低分辨率 colorIn 重建到全屏 out。无 motion/depth 依赖。
    bool FfxUpscaleRecord(VkCommandBuffer cmd, VkImage src, VkImage out,
                          uint32_t renderW, uint32_t renderH,
                          uint32_t displayW, uint32_t displayH, bool reset);
    bool FfxUpscaleEnabled();
    // 超分输出图（全屏 RGBA8，GENERAL 布局）。由 FfxUpscaleRecord 写入。
    VkImage FfxUpscaleOutputImage();
    // 检测版：超分诊断日志（落到 /sdcard/FusionGL/ffx_probe.log 的 [FFX-UP] 段）。
    void FfxDiagLogUpscale(const char* fmt, ...);
    // 检测版:阶段标记(崩溃报告会带出当前阶段)
    void __FfxMarkStage(const char* s);
} // namespace MobileGL::MG_Backend::DirectVulkan
