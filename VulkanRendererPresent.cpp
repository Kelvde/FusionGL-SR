// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRendererPresent.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VulkanRendererInternal.h"
#if defined(MOBILEGL_FFX_PROBE) && MOBILEGL_FFX_PROBE
#include <chrono>
#include "FfxProbe.h"
#endif

namespace MobileGL::MG_Backend::DirectVulkan {
#if defined(MOBILEGL_FFX_PROBE) && MOBILEGL_FFX_PROBE
    namespace {
        // 插帧呈现频率闸门的时间戳：上一次插值帧成功 present 的时刻。
        // 相邻两次呈现至少间隔 4ms（≈240fps 上限）——FIFO 队列永不堆积，
        // 同时高帧率场景（关光影 120fps）也能正常插帧到 240。
        std::chrono::steady_clock::time_point g_lastFgPresent{};
    }
#endif

    void VulkanRenderer::Present() {
#if defined(MOBILEGL_FFX_PROBE) && MOBILEGL_FFX_PROBE
        // P2b：本帧插值帧的目标图与同步量。插值帧的拷贝在主渲染的同一个命令缓冲里录制、
        // 同一次 vkQueueSubmit 里提交，CPU 不等待 GPU —— 游戏原始帧率才不会被拖慢。
        VkImage interpImage = VK_NULL_HANDLE;
        uint32_t interpIndex = UINT32_MAX;
        VkSemaphore interpAcquire = VK_NULL_HANDLE;
        VkSemaphore interpFinished = VK_NULL_HANDLE;
#endif
#if MOBILEGL_BUILD_DISAGGREGATED
        // A lost device presents nothing (its session is latched and closing).
        if (m_deviceLost && LatchWireDeviceLoss("present")) return;
        // Present has its own submit path. Tag its last wire pass before that
        // submission, rather than retaining it until a draw in the next frame.
        RetireWireDrawPass();
        // A window color clear no pass drew after still has to reach the presented image. Its
        // depth/stencil half is dropped: a swap leaves those undefined (see the content flags
        // reset below). A frame that cannot be presented drops the whole clear.
        if (m_swapchainObject.GetHandle() == VK_NULL_HANDLE || m_presentSuspended) {
            m_defaultWirePendingClear = {};
        } else {
            m_defaultWirePendingClear.depthStencil = 0;
            MaterializeDefaultWirePendingClear();
        }
#endif
        if (m_swapchainObject.GetHandle() == VK_NULL_HANDLE || m_presentSuspended) {
            // No usable swapchain: the window was zero-area at initialization, or
            // presentation was suspended when the window minimized. Try to bring a
            // swapchain up now that the window may have a real size; until then, drop
            // this frame's recording instead of submitting - a submit would wait on a
            // never-signaled acquire semaphore and reuse a still-signaled fence.
            if (!RecreateSwapchain()) {
                auto& suspendedFrame = m_frameContext.GetCurrent();
                if (VkRenderPassManager::GetActiveRenderPass()) {
                    VkRenderPassManager::EndRenderPass(suspendedFrame.commandBuffer);
                }
                if (suspendedFrame.isCommandRecording) {
                    m_frameContext.EndCommandRecording();
                }
                m_frameContext.AbandonPreCommandRecording();
                suspendedFrame.isCommandRecording = false;
                suspendedFrame.hasCommandBufferRecorded = false;
                InvalidatePipelineMemo();
#if MOBILEGL_BUILD_DISAGGREGATED
                AbandonSharedImageReads();
#endif
                // The dropped recording is never submitted, so once the fence
                // poll shows the pre-suspension submissions complete the frame
                // transients (descriptor sets, transient arenas, deferred
                // releases, conversion caches) can rewind; without this a
                // minimized-window app accumulates them for the whole
                // suspension.
                TryDrainFrameTransients();
                // The application counted this swap; no image did.
                m_swapchainObject.OnFrameDropped();
                MGLOG_D("Present skipped: no usable swapchain (zero-area window)");
                return;
            }
            m_presentSuspended = false;
            // A slot's Present fence alone does not prove its older pooled
            // flushes retired. Wait the full prefix before FrameContext may
            // free its retired command buffers and reset the slot fence.
            const Uint64 slotSubmit = m_frameContext.GetCurrent().lastSubmitIndex;
            if (slotSubmit > m_completedSubmitCounter && !WaitForSubmitsUpTo(slotSubmit, UINT64_MAX)) {
                if (LatchWireDeviceLoss("deferred-acquire-submit-wait")) return;
                MagmaWireFatal("deferred-acquire-submit-wait");
            }
            const VkResult acquireResult =
                m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
            if (acquireResult == VK_SUBOPTIMAL_KHR) {
                // Usable image with its acquire signal already armed; a rebuild is scheduled
                // only if the surface genuinely no longer matches (see step 4 of Present).
                m_swapchainResizeRequested = m_swapchainResizeRequested || SwapchainIsOutOfDate();
            } else {
                VK_VERIFY(acquireResult, "Present, deferred first WaitAndAcquireNextImage");
            }
        }
        MOBILEGL_ASSERT(m_imageIndexAcquired < m_swapchainObject.GetImageCount(),
                        "Present, acquired image index out of range");
#if defined(MOBILEGL_FFX_PROBE) && MOBILEGL_FFX_PROBE
        // FusionGL P0：一次性验证 FSR3 帧生成上下文能否在本机创建。
        // 只 create/destroy，不参与本帧任何绘制或呈现，失败也绝不影响正常出画。
        FfxProbeMaybeRun(m_physicalDevice.handle, m_device,
                         m_swapchainObject.GetSurfaceFormat().format,
                         m_swapchainObject.GetExtent().width,
                         m_swapchainObject.GetExtent().height);
#endif
        // A frame that never drew into the window still presents its image: what it recorded
        // goes out first, unwaited, and the present's own submission waits for the release.
        SubmitAheadOfAcquiredImage();
        // A real presented frame is the canonical aging cadence; mid-frame drains
        // count against this and only age when presents stop coming.
        m_drainsSinceLastPresent = 0;
        // Age the content-addressed caches on the same frame-boundary cadence. Each
        // keeps its own internal 256-sweep gate, so the per-frame cost is one counter
        // increment and compare per cache; entries used by this frame's still-
        // unsubmitted recording were stamped this boundary (every command-buffer
        // boundary drops the pipeline memo, so the first draw of each recording
        // performs a real, stamping lookup) and can never age out.
        m_programFactory->OnFrameBoundary();
        if (m_pipelineFactory->OnFrameBoundary() > 0) {
            InvalidatePipelineMemo(); // an aged-out pipeline may still be memoized
            // A recreated pipeline could reuse a freed handle value and alias
            // the bind-dedup shadow; force the next draw to re-bind.
            g_dynamicStateShadow->graphicsPipelineValid = false;
        }
        m_samplerManager->OnFrameBoundary();
        auto& frame = m_frameContext.GetCurrent();
        auto* activeRenderPass = VkRenderPassManager::GetActiveRenderPass();
        if (activeRenderPass) VkRenderPassManager::EndRenderPass(frame.commandBuffer);

#if defined(MOBILEGL_FFX_PROBE) && MOBILEGL_FFX_PROBE
        // FusionGL P1/P2: 用本帧画面生成一帧插值（P2 时随后上屏）。
        // FSR3 只认 R8G8B8A8_UNORM，而本机 swapchain 是 10bit，所以先把画面 blit 成 RGBA8
        // 再喂给它，否则它写出来的是黑图，上屏就表现为一闪一闪的黑屏。
        //
        // 低帧率自动暂停（进世界卡顿优化）——带滞回的稳定状态机，防恶性 bug：
        //   * 单次瞬时卡顿（一个长帧）绝不触发暂停 —— 必须连续 8 帧间隔 >50ms
        //     （持续低帧率，如区块构建期）才暂停；
        //   * 暂停后必须连续 12 帧恢复正常帧率才恢复插帧 —— 防"暂停后间隔抖动
        //     导致反复开关"的恶性循环。
        // 区块构建/重载期原始帧率本来就低，每帧再跑 FSR3 光流 compute（5-20ms
        // GPU）会占掉帧时间 20-40% 进一步拖慢原始帧；此时整条插帧链路暂停，
        // GPU 让给区块构建，原始帧跑满，构建完帧率恢复插帧自动恢复。
        static std::chrono::steady_clock::time_point s_fgPrevFrame{};
        static int s_fgLowRun = 0;
        static int s_fgNormalRun = 0;
        static bool s_fgSuspended = false;
        // 最近 8 帧间隔的滑动窗口：区分"区块构建期（该暂停）"和"高压场景（该插帧）"。
        //   构建期瓶颈在 CPU：帧间隔全部 >50ms 且剧烈波动（max-min > 30ms）
        //   高压场景瓶颈在 GPU：帧间隔稳定（波动小）—— 这正是最需要插帧的场景，
        //   绝不暂停。用户指出：高压场景急需插帧，停了就本末倒置。
        static std::chrono::milliseconds s_gapHist[8]{};
        static int s_gapIdx = 0;
        const auto fgFrameNow = std::chrono::steady_clock::now();
        const auto curGap = std::chrono::duration_cast<std::chrono::milliseconds>(
            fgFrameNow - s_fgPrevFrame);
        s_fgPrevFrame = fgFrameNow;
        if (s_fgPrevFrame.time_since_epoch().count() != 0 && curGap.count() >= 0 &&
            curGap.count() < 1000) {
            s_gapHist[s_gapIdx] = curGap;
            s_gapIdx = (s_gapIdx + 1) % 8;
        }
        bool fgLoadPattern = false;
        // 进世界判定（实测修正）：区块构建期帧间隔是 35-60ms（20-28fps），满足不了
        // ">50ms+波动"条件。改用最直接的事件特征：出现 >100ms 长帧（进世界/加载/
        // 传送的标志，与 history reset 日志一一对应）后，暂停插帧 30 秒给区块构建
        // 让路，到时自动恢复。高压场景帧间隔稳定、不会出现这种长帧，永不误触发。
        // 暂停时长自适应（前人实测方案 PS5SX2）：暂停结束后对比暂停前/后的帧间隔，
        // 若恢复插帧后帧间隔明显变长（帧生成拖慢游戏的证据），下次暂停翻倍（上限 300s）；
        // 若没变长（区块构建是 CPU 问题），保持 90s 不变。暂停时长由证据驱动而非拍脑袋。
        static std::chrono::steady_clock::time_point s_loadUntil{};
        static std::chrono::steady_clock::time_point s_pauseStartedAt{}; // 本轮暂停首次触发时刻
        static bool s_loadPause = false;
        static int s_pauseSeconds = 90;      // 用户指定 1 分半起跳
        // 硬上限：单次暂停从首次触发起最多这么久，之后强制恢复一次，绝不无限续期。
        // 600s 远大于 JVM/世界初始化的正常时长（通常 30~120s），启动期保护不受影响；
        // 只防止「愚者」这类慢速场景保存期被长帧续期拖成几分钟~几十分钟的插帧全停。
        static constexpr int s_pauseHardCapSeconds = 600;
        static int s_recoveryFrames = 0;     // 恢复后已累计的帧数
        static int64_t s_recoveryGapSum = 0; // 恢复后帧间隔总和（ms）
        static int64_t s_pauseBaseline = 0;  // 暂停期（无插帧）的帧间隔基准（ms）
        // 修：暂停期采到 <8ms 的帧间隔是失真值（如空闲时 500fps 的 2ms），拿它当
        // “无插帧基准”会让恢复后的正常帧（如 19ms）被误判成“慢了 9 倍”，进而把
        // 暂停时长一路翻倍到 300s。只采可信区间 [8ms, 2000ms) 作为基准。
        // 注意：这里只影响“恢复后是否再翻倍”的判据，不改暂停/续期本身——
        // JVM/世界初始化期的插帧暂停保护（下方 s_loadUntil 续期）完全不变。
        if (s_loadPause && s_recoveryFrames == 0 && curGap.count() >= 8 && curGap.count() < 2000) {
            s_pauseBaseline = s_pauseBaseline ? (s_pauseBaseline + curGap.count()) / 2 : curGap.count();
        }
        // 长帧窗口计数（10 秒滑窗）：区分"JVM/世界初始化（长帧密集）"和"平时区块
        // 加载（偶发长帧）"。初始化期 history reset 连发，平时只偶发 1-2 个。
        static std::chrono::steady_clock::time_point s_longFrames[8]{};
        static int s_lfIdx = 0;
        if (curGap > std::chrono::milliseconds(100)) {
            s_longFrames[s_lfIdx] = fgFrameNow;
            s_lfIdx = (s_lfIdx + 1) % 8;
        }
        int recentLong = 0;
        for (int i = 0; i < 8; ++i)
            if (s_longFrames[i].time_since_epoch().count() != 0 &&
                fgFrameNow - s_longFrames[i] <= std::chrono::seconds(10)) ++recentLong;
        const bool initPattern = recentLong >= 3;   // 10 秒内 >=3 个长帧 = 初始化特征
        if (initPattern && !s_loadPause) {
            FfxDiagLog("[FFX-DIAG] init pattern detected (%d long frames/10s) -> framegen paused %ds",
                       recentLong, s_pauseSeconds);
            s_pauseBaseline = 0;
            s_recoveryFrames = 0;
            s_recoveryGapSum = 0;
            s_loadPause = true;
            s_pauseStartedAt = fgFrameNow; // 记录本轮暂停起点，用于硬上限
        }
        if (s_loadPause) {
            if (curGap > std::chrono::milliseconds(100)) {
                // 初始化仍在进行（还有长帧）-> 自动续期，但受硬上限约束：
                // 续期目标不得晚于“首次暂停 + 600s”，防止慢速场景（如愚者保存期
                // 持续 2fps 长帧）把暂停无限续下去。启动期远短于 600s，保护不变。
                const auto renewTo = fgFrameNow + std::chrono::seconds(s_pauseSeconds);
                const auto capTo = s_pauseStartedAt + std::chrono::seconds(s_pauseHardCapSeconds);
                s_loadUntil = renewTo < capTo ? renewTo : capTo;
            } else if (fgFrameNow >= s_loadUntil) {
                // 长帧已消失且窗口到期 = 初始化结束 -> 恢复插帧
                s_loadPause = false;
                s_recoveryFrames = 0;
                s_recoveryGapSum = 0;
                FfxDiagLog("[FFX-DIAG] init phase over -> framegen resumed (baseline %lldms)",
                           static_cast<long long>(s_pauseBaseline));
            }
        }
        // 恢复插帧后的证据采样：累计 30 帧后判定
        if (!s_loadPause && s_pauseBaseline > 0 && curGap.count() > 0 && curGap.count() < 2000) {
            if (s_recoveryFrames < 30) {
                s_recoveryFrames++;
                s_recoveryGapSum += curGap.count();
                if (s_recoveryFrames == 30) {
                    const int64_t withFg = s_recoveryGapSum / 30;
                    // 修：只有基准可信（>=16ms，即真实 <=60fps 的帧间隔）且插帧确实
                    // 明显拖慢（>1.5 倍）时才翻倍。失真的低基准（如上面被过滤后仍
                    // 残留的极小值）不再触发 90->180->300s 的暴涨——那会把「愚者」
                    // 这类慢速场景的保存期插帧暂停越拉越长。启动期保护不受影响：
                    // 本段只在恢复插帧后执行，暂停期走的是上面的 s_loadUntil 续期。
                    if (s_pauseBaseline >= 16 && withFg > s_pauseBaseline * 15 / 10) {
                        s_pauseSeconds = s_pauseSeconds * 2 < 300 ? s_pauseSeconds * 2 : 300;
                        FfxDiagLog("[FFX-DIAG] framegen slowed game (withFg %lldms vs %lldms baseline) "
                                   "-> next pause doubled to %ds",
                                   static_cast<long long>(withFg),
                                   static_cast<long long>(s_pauseBaseline), s_pauseSeconds);
                    } else {
                        FfxDiagLog("[FFX-DIAG] framegen cost acceptable (withFg %lldms vs %lldms) "
                                   "-> pause stays %ds",
                                   static_cast<long long>(withFg),
                                   static_cast<long long>(s_pauseBaseline), s_pauseSeconds);
                    }
                    s_pauseBaseline = 0; // 本轮证据采集完毕
                }
            }
        }
        if (s_gapHist[7].count() > 0) { // 窗口已填满过
            bool allLow = true;
            auto gapMax = s_gapHist[0], gapMin = s_gapHist[0];
            for (int i = 0; i < 8; ++i) {
                if (s_gapHist[i].count() <= 0 || s_gapHist[i] > std::chrono::milliseconds(50)) allLow = false;
                if (s_gapHist[i] > gapMax) gapMax = s_gapHist[i];
                if (s_gapHist[i] < gapMin) gapMin = s_gapHist[i];
            }
            fgLoadPattern = allLow && (gapMax - gapMin) > std::chrono::milliseconds(30);
        }
        fgLoadPattern = fgLoadPattern || s_loadPause;
        if (!s_fgSuspended) {
            s_fgLowRun = fgLoadPattern ? s_fgLowRun + 1 : 0;
            s_fgNormalRun = 0;
            if (s_fgLowRun >= 8) {
                s_fgSuspended = true;
                s_fgLowRun = 0;
                FfxDiagLog("[FFX-DIAG] load-phase pattern (8 windows: low+jittery) -> framegen paused for world build");
            }
        } else {
            s_fgNormalRun = fgLoadPattern ? 0 : s_fgNormalRun + 1;
            if (s_fgNormalRun >= 12) {
                s_fgSuspended = false;
                s_fgNormalRun = 0;
                FfxDiagLog("[FFX-DIAG] load phase over (12 windows normal) -> framegen resumed");
            }
        }
        if (g_fgContextReady && !s_fgSuspended) {
            static uint64_t s_fgFrame = 0;
            static bool s_fgReset = true;
            const uint32_t dw = m_swapchainObject.GetExtent().width;
            const uint32_t dh = m_swapchainObject.GetExtent().height;
            VkImage colorImg = m_swapchainObject.GetImage(m_imageIndexAcquired);
            VkImage colorIn = FfxFramegenColorInImage();
            const VkImageLayout colorOld = m_swapchainObject.GetImageLayout(m_imageIndexAcquired);
            auto mkBar = [](VkImage img, VkImageLayout o, VkImageLayout n,
                            VkAccessFlags sa, VkAccessFlags da) {
                VkImageMemoryBarrier b{};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.oldLayout = o; b.newLayout = n;
                b.srcAccessMask = sa; b.dstAccessMask = da;
                b.image = img;
                b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                b.subresourceRange.baseMipLevel = 0; b.subresourceRange.levelCount = 1;
                b.subresourceRange.baseArrayLayer = 0; b.subresourceRange.layerCount = 1;
                return b;
            };
            // A) swapchain 画面转 TRANSFER_SRC；RGBA8 中转图转 TRANSFER_DST；离屏资源就位
            VkImageMemoryBarrier bars[5] = {
                mkBar(colorImg, colorOld, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
                mkBar(colorIn, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT),
                mkBar(FfxFramegenDepthImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT),
                mkBar(FfxFramegenMotionImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT),
                mkBar(FfxFramegenOutputImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_GENERAL, 0,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
            };
            vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 5, bars);

            // B) 把本帧画面 blit 成 RGBA8（10bit -> 8bit，尺寸不变，用 NEAREST 避免混色）
            VkImageBlit toRgba8{};
            toRgba8.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toRgba8.srcSubresource.mipLevel = 0;
            toRgba8.srcSubresource.baseArrayLayer = 0;
            toRgba8.srcSubresource.layerCount = 1;
            toRgba8.srcOffsets[0] = {0, 0, 0};
            // 超分：游戏渲染在左下角的缩放区域（viewport 已按 FUSION_UPSCALE_PCT 缩放），
            // 只取该区域拉伸成全屏 RGBA8 喂给 FSR3。
            const float uf = FfxUpscaleFactor();
            const int32_t srcW = static_cast<int32_t>(static_cast<float>(dw) * uf);
            const int32_t srcH = static_cast<int32_t>(static_cast<float>(dh) * uf);
            toRgba8.srcOffsets[1] = {srcW > 0 ? srcW : static_cast<int32_t>(dw),
                                     srcH > 0 ? srcH : static_cast<int32_t>(dh), 1};
            toRgba8.dstSubresource = toRgba8.srcSubresource;
            toRgba8.dstOffsets[0] = {0, 0, 0};
            toRgba8.dstOffsets[1] = {static_cast<int32_t>(dw), static_cast<int32_t>(dh), 1};
            vkCmdBlitImage(frame.commandBuffer, colorImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           colorIn, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &toRgba8, VK_FILTER_NEAREST);

            // C) 中转图转 SHADER_READ 供 FSR3 采样；swapchain 画面转 TRANSFER_DST
            //    （拉伸 blit 的目标）。
            VkImageMemoryBarrier post[2] = {
                mkBar(colorIn, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
                mkBar(colorImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, 0),
            };
            vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 2, post);

            // FusionSR 超分：colorIn 已经是拉伸到全屏(dw×dh)的画面 —— 放大由上面的
            // blit 完成。超分在此做 1:1 重建（时间积累 + 锐化），绝不能再放大：
            // render 尺寸必须与 display 尺寸相同，否则画面被二次放大
            // 导致鼠标错位 / 画面越界（实测确认过的 bug）。
            // 失败自动回退 colorIn；插帧暂停时同步暂停；插帧输入保持 colorIn 不变。
            __FfxMarkStage("present-upscale-section");
            VkImage presentColor = colorIn;
            if (FfxUpscaleEnabled() && g_fgContextReady && !s_fgSuspended) {
                static int s_upFail = 0;
                // 真超分: MC 在 87.5% viewport 区域渲染(render 尺寸), 超分重建到
                // 全屏 buffer(display 尺寸)。buffer 恒全屏 -> 鼠标域=显示域, 零错位。
                const int32_t rw = static_cast<int32_t>(static_cast<float>(dw) * uf);
                const int32_t rh = static_cast<int32_t>(static_cast<float>(dh) * uf);
                if (FfxUpscaleRecord(frame.commandBuffer, colorIn, FfxUpscaleOutputImage(),
                                     rw > 0 ? rw : dw, rh > 0 ? rh : dh, dw, dh, s_fgReset)) {
                    presentColor = FfxUpscaleOutputImage();
                    static bool s_consOnce = false;
                    if (!s_consOnce) { s_consOnce = true; FfxDiagLogUpscale("[CONS] upscale active (render->display 1:1 ok)"); }
                } else if (++s_upFail == 1 || s_upFail % 300 == 0) {
                    FfxDiagLogUpscale("[CHECK] record failed x%d -> fallback to colorIn (framegen unaffected)",
                                      s_upFail);
                }
            }

            __FfxMarkStage("present-framegen-record");
            const uint64_t fid = ++s_fgFrame;
            const bool fgRecorded =
                FfxFramegenRecord(frame.commandBuffer, colorIn, FfxFramegenColorFormat(),
                                  VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                  dw, dh, dw, dh, fid, s_fgReset);
            s_fgReset = false;
            (void)fgRecorded;

            // 超分拉伸闭环：colorIn（全屏 RGBA8，内容=缩放区域放大后的画面）blit 回
            // swapchain 图全屏（RGBA8->10bit 转换 blit）——真实帧呈现的就是拉伸后的
            // 全屏画面，黑边不存在。tracker 同步为 TRANSFER_DST（present 转换交给
            // 后面的 TransitionToPresent）。
                __FfxMarkStage("present-stretch-blit");
        m_swapchainObject.SetImageLayout(m_imageIndexAcquired,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            if (uf < 1.0f) {
                VkImageBlit stretch{};
                stretch.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                stretch.srcSubresource.mipLevel = 0;
                stretch.srcSubresource.baseArrayLayer = 0;
                stretch.srcSubresource.layerCount = 1;
                stretch.srcOffsets[0] = {0, 0, 0};
                stretch.srcOffsets[1] = {static_cast<int32_t>(dw), static_cast<int32_t>(dh), 1};
                stretch.dstSubresource = stretch.srcSubresource;
                stretch.dstOffsets[0] = {0, 0, 0};
                stretch.dstOffsets[1] = {static_cast<int32_t>(dw), static_cast<int32_t>(dh), 1};
                vkCmdBlitImage(frame.commandBuffer, presentColor,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               colorImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &stretch, VK_FILTER_LINEAR);
            }

            // P2b：取一张额外的交换链图，把刚生成的插值帧拷进去。
            // 拷贝录制在同一个命令缓冲里，和主渲染同批提交，CPU 不等待 GPU。
            //
            // 频率闸门（ANR 根治）：FIFO 呈现模式下 present 会阻塞等刷新周期。
            // 游戏渲染快（主菜单上百帧）时，双呈现的频率会超过屏幕刷新率，
            // FIFO 队列永远满 -> 主线程死等 -> ANR。这里强制任何两次呈现之间
            // 至少间隔一个刷新周期（按 120Hz 屏保守取 8ms）：游戏快时插帧自动
            // 让路（此时屏幕本来就流畅，插帧无意义），游戏慢（如 42fps，帧间隔
            // 23.8ms）时每帧照插，总呈现频率恒 <= 刷新率。
            const auto fgNow = std::chrono::steady_clock::now();
            const bool fgPaceOk =
                (g_lastFgPresent.time_since_epoch().count() == 0) ||
                (fgNow - g_lastFgPresent) >= std::chrono::milliseconds(4);
            if (fgRecorded && g_fgPresentEnabled && fgPaceOk) {
                if (FfxAcquireInterpTarget(
                        m_device, m_swapchainObject.GetHandle(),
                        m_swapchainObject.GetImages().data(),
                        static_cast<uint32_t>(m_swapchainObject.GetImageCount()),
                        m_imageIndexAcquired,
                        &interpImage, &interpIndex, &interpAcquire, &interpFinished)) {
                    if (!FfxRecordInterpBlit(frame.commandBuffer, interpImage, dw, dh)) {
                        // 录制失败：这张图已经被 acquire，绝不能弄丢 —— 立刻 present
                        // 归还（内容未定义，最多闪一帧灰），否则几帧后交换链无图可取，
                        // 主线程死等 -> ANR。这就是上一版"跑几分钟就未响应"的根因。
                        VkPresentInfoKHR ret{};
                        ret.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                        ret.swapchainCount = 1;
                        VkSwapchainKHR sc = m_swapchainObject.GetHandle();
                        ret.pSwapchains = &sc;
                        ret.pImageIndices = &interpIndex;
                        const VkResult retResult = vkQueuePresentKHR(m_presentQueue, &ret);
                        FfxDiagLog("[FFX-DIAG] interp blit not recorded -> image #%u returned (rc=%d)",
                                   interpIndex, static_cast<int>(retResult));
                        interpImage = VK_NULL_HANDLE;
                    }
                }
            }
        }
#endif

#if MOBILEGL_BUILD_DISAGGREGATED
        // The frame boundary of the shared images this frame read (WireSharedImage.cpp): handed
        // back to the foreign family in this recording, and this submission signals the read
        // fence their next writer waits for.
        ReleaseHeldSharedImages();
        if (m_sharedImageSyncFd && m_sharedImageReads.Pending() > 0 && m_sharedImageSignal == VK_NULL_HANDLE)
            m_sharedImageSignal = TakeSharedImageSemaphore(/*exportable=*/true);
#endif

        // Transition while this frame's recording is still open. A frame that
        // rendered only into FBOs has no default-framebuffer render pass, and that
        // pass's finalLayout is the only other thing that carries the swapchain
        // image to PRESENT_SRC_KHR - so closing the buffer first, which made
        // TransitionToPresent refuse to record, handed the image to
        // vkQueuePresentKHR in the layout it was acquired in (UNDEFINED on a fresh
        // swapchain). The SetImageLayout below then made the tracker's
        // disagreement with reality permanent for that image index.
        const auto acquiredImageLayout = m_swapchainObject.GetImageLayout(m_imageIndexAcquired);
        m_frameContext.TransitionToPresent(m_swapchainObject.GetImage(m_imageIndexAcquired), acquiredImageLayout);

        if (frame.isCommandRecording) {
            m_frameContext.EndCommandRecording();
            frame.hasCommandBufferRecorded = true;
            InvalidatePipelineMemo(); // command-buffer boundary: drop the pipeline memo
        }
        m_frameContext.EndPreCommandRecordingIfOpen();

        const Bool shouldSubmitCommandBuffer = frame.hasCommandBufferRecorded;

        // 1) Submit current frame work (the pre-pass stream, when recorded,
        //    rides the same submission strictly ahead of the frame commands).
        //    Batched texture uploads go first: the frame's commands may sample
        //    images whose texels only exist in the open upload batch, and
        //    flushing here also bounds upload latency to one frame.
        if (m_textureManager) {
            m_textureManager->FlushPendingUploads();
        }
        auto submitPacket = m_frameContext.GetSubmitInfo(shouldSubmitCommandBuffer, m_imageIndexAcquired);
#if defined(MOBILEGL_FFX_PROBE) && MOBILEGL_FFX_PROBE
        // P2b：把插值帧的 acquire / finished 信号量并进同一次提交，
        // 这样插值帧拷贝与主渲染同批完成，CPU 不用等 GPU。
        VkSemaphore fgWaitSems[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkSemaphore fgSignalSems[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkPipelineStageFlags fgWaitStages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                VK_PIPELINE_STAGE_TRANSFER_BIT};
        if (interpImage != VK_NULL_HANDLE) {
            const uint32_t baseWaits = submitPacket.submitInfo.waitSemaphoreCount;
            const uint32_t baseSignals = submitPacket.submitInfo.signalSemaphoreCount;
            if (baseWaits > 0 && submitPacket.submitInfo.pWaitSemaphores) {
                fgWaitSems[0] = submitPacket.submitInfo.pWaitSemaphores[0];
            }
            fgWaitSems[1] = interpAcquire;
            if (baseSignals > 0 && submitPacket.submitInfo.pSignalSemaphores) {
                fgSignalSems[0] = submitPacket.submitInfo.pSignalSemaphores[0];
            }
            fgSignalSems[1] = interpFinished;
            if (baseWaits > 0) {
                submitPacket.submitInfo.waitSemaphoreCount = 2;
                submitPacket.submitInfo.pWaitSemaphores = fgWaitSems;
                submitPacket.submitInfo.pWaitDstStageMask = fgWaitStages;
            } else {
                submitPacket.submitInfo.waitSemaphoreCount = 1;
                submitPacket.submitInfo.pWaitSemaphores = &fgWaitSems[1];
                submitPacket.submitInfo.pWaitDstStageMask = &fgWaitStages[1];
            }
            if (baseSignals > 0) {
                submitPacket.submitInfo.signalSemaphoreCount = 2;
                submitPacket.submitInfo.pSignalSemaphores = fgSignalSems;
            } else {
                submitPacket.submitInfo.signalSemaphoreCount = 1;
                submitPacket.submitInfo.pSignalSemaphores = &fgSignalSems[1];
            }
        }
#endif
#if MOBILEGL_BUILD_DISAGGREGATED
        if (LatchIfGpuHung("present-submit")) return;
        SharedImageSubmitSync sharedImageSync;
        AttachSharedImageSync(submitPacket.submitInfo, sharedImageSync);
        const GpuProgressMarkers::Bracket bracket = m_progressMarkers.Acquire(m_completedSubmitCounter);
        const VkResult presentSubmit = GpuProgressMarkers::SubmitBracketed(m_graphicsQueue, submitPacket.submitInfo,
                                                                           frame.imageInFlightFence, bracket);
        if (presentSubmit == VK_SUCCESS)
            m_progressMarkers.Submitted(bracket, m_submitCounter + 1); // RegisterSubmit below
        else
            m_progressMarkers.Abandon(bracket);
#else
        const VkResult presentSubmit =
            vkQueueSubmit(m_graphicsQueue, 1, &submitPacket.submitInfo, frame.imageInFlightFence);
#endif
        NoteDeviceLoss(presentSubmit, "Present vkQueueSubmit");
        if (presentSubmit != VK_SUCCESS && LatchWireDeviceLoss("present-submit")) return;
        VK_VERIFY(presentSubmit);
        RegisterSubmit(frame.imageInFlightFence, /*pooledFence=*/false);
        frame.lastSubmitIndex = m_submitCounter;
#if MOBILEGL_BUILD_DISAGGREGATED
        CommitSharedImageSync(sharedImageSync);
        PublishSharedImageReads();
#endif
        frame.isCommandRecording = false;
        frame.hasCommandBufferRecorded = false;
        frame.hasPreCommandBufferRecorded = false;
        m_swapchainObject.SetImageLayout(m_imageIndexAcquired, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

#if defined(MOBILEGL_FFX_PROBE) && MOBILEGL_FFX_PROBE
        // P2b：先呈现插值帧，再呈现真实帧。
        // 顺序关键：插值帧表示"上一帧 -> 本帧"的中间状态，必须排在本帧真实帧之前，
        // 否则画面会先到本帧再倒回中间状态，表现为闪烁。
        if (interpImage != VK_NULL_HANDLE && interpFinished != VK_NULL_HANDLE) {
            VkPresentInfoKHR interpPresent{};
            interpPresent.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            interpPresent.waitSemaphoreCount = 1;
            interpPresent.pWaitSemaphores = &interpFinished;
            interpPresent.swapchainCount = 1;
            VkSwapchainKHR fgSwapchain = m_swapchainObject.GetHandle();
            interpPresent.pSwapchains = &fgSwapchain;
            interpPresent.pImageIndices = &interpIndex;
            const VkResult interpResult = vkQueuePresentKHR(m_presentQueue, &interpPresent);
            if (interpResult == VK_SUCCESS || interpResult == VK_SUBOPTIMAL_KHR) {
                // 这张图已被我们 present：同步它在本对象里的账（layout 与年龄）
                m_swapchainObject.SetImageLayout(interpIndex, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                m_swapchainObject.OnImagePresented(interpIndex);
                m_swapchainObject.SetImageContentDefined(interpIndex, false);
                g_lastFgPresent = std::chrono::steady_clock::now();
            } else {
                // present 失败（OUT_OF_DATE 等）：swapchain 马上会重建，图随旧链释放，
                // 但 semaphore 可能残留 signal 状态——记录下来供排查节奏问题。
                FfxDiagLog("[FFX-DIAG] interp present FAILED rc=%d idx=%u (swapchain rebuild expected)",
                           static_cast<int>(interpResult), interpIndex);
            }
            interpImage = VK_NULL_HANDLE;
        }
#endif

        // 2) Present current frame.
        auto presentPacket = m_frameContext.GetPresentInfo(m_swapchainObject.GetHandle(), m_imageIndexAcquired);
        auto result = vkQueuePresentKHR(m_presentQueue, &presentPacket.presentInfo);
        if (result == VK_SUBOPTIMAL_KHR) {
            // Suboptimal is not a reason to rebuild on its own: a driver may report it for a
            // surface whose size and orientation still match what we built from (Android does
            // this routinely), and rebuilding on it alone destroys every pipeline and
            // reallocates the default framebuffer once per frame - flicker, then garbage.
            // Defer to the surface-capabilities comparison below.
            result = VK_SUCCESS;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            MGLOG_D("Present, vkQueuePresentKHR got %d, recreating swapchain", result);
            if (!RecreateSwapchain()) {
                // Window went zero-area (minimize) with the swapchain out of date:
                // stop submitting/acquiring until it has a size again.
                m_presentSuspended = true;
                m_swapchainResizeRequested = false;
                MGLOG_D("Present, zero-area window with out-of-date swapchain; suspending presentation");
                return;
            }
            m_swapchainResizeRequested = false;
            result = VK_SUCCESS;
        }
        NoteDeviceLoss(result, "vkQueuePresentKHR");
        if (result != VK_SUCCESS && LatchWireDeviceLoss("present-queue")) return;
        VK_VERIFY(result, "Present, vkQueuePresentKHR");
        // EGL swap semantics: the presented color buffer's content is undefined the
        // next time this image is acquired (EGL_BUFFER_DESTROYED, the default swap
        // behavior), and EVERY ancillary depth/stencil buffer's content is
        // undefined after any swap. The render-pass manager turns the undefined
        // attachments' next tile loads into LOAD_OP_DONT_CARE.
        //
        // EGL_EXT_buffer_age changes the first half: once the client has asked for an age it
        // repaints only what changed since the image it is given was last shown, so a presented
        // image keeps its content (the wire passes load it; Vulkan preserves an image's content
        // through a present and a layout transition out of PRESENT_SRC) and its age is counted.
        m_swapchainObject.OnImagePresented(m_imageIndexAcquired);
        if (!m_swapchainObject.BufferAgeAsked()) m_swapchainObject.SetImageContentDefined(m_imageIndexAcquired, false);
        m_swapchainObject.SetAllDepthStencilContentUndefined();
        // The authoritative check, done here - after the frame is presented, before the next
        // acquire. This is what makes a launcher-side resolution change take effect: shrinking
        // the window's buffer (SurfaceHolder.setFixedSize) moves currentExtent, the swapchain
        // follows, and the compositor scales the smaller image up to the view for free.
        if (!m_swapchainResizeRequested && SwapchainIsOutOfDate()) {
            m_swapchainResizeRequested = true;
        }
        // A new eglSwapInterval lands here too, between frames: this one is on screen and the
        // next has recorded nothing. Only a different present mode is worth the rebuild (it
        // drops every pipeline); otherwise the live swapchain already paces as asked.
        if (m_config.SwapInterval != m_swapchainSwapInterval) {
            if (SwapchainObject::ChooseSwapchainPresentMode(m_swapchainObject.GetSupportedPresentModes(),
                                                            m_config.SwapInterval) !=
                m_swapchainObject.GetPresentMode()) {
                m_swapchainResizeRequested = true;
            } else {
                MGLOG_D("DirectVulkan: swap interval %d -> %s", *m_config.SwapInterval,
                        SwapchainObject::GetPresentModeName(m_swapchainObject.GetPresentMode()));
                m_swapchainSwapInterval = m_config.SwapInterval;
            }
        }
        if (m_swapchainResizeRequested) {
            MGLOG_D("Present, processing requested swapchain rebuild (resize or swap interval)");
            if (!RecreateSwapchain()) {
                m_presentSuspended = true;
                m_swapchainResizeRequested = false;
                MGLOG_D("Present, zero-area window on requested resize; suspending presentation");
                return;
            }
            m_swapchainResizeRequested = false;
        }

        // 3) Advance frame slot.
        m_frameContext.AdvanceToNext();

        // FrameContext waits then resets the slot fence and frees retired
        // command buffers. A later Present fence is not aggregate proof for
        // earlier pooled flushes, so wait every registered submission in this
        // slot's prefix BEFORE the reset/free happens.
        const Uint64 slotSubmit = m_frameContext.GetCurrent().lastSubmitIndex;
        if (slotSubmit > m_completedSubmitCounter && !WaitForSubmitsUpTo(slotSubmit, UINT64_MAX)) {
            if (LatchWireDeviceLoss("present-slot-submit-wait")) return;
            MagmaWireFatal("present-slot-submit-wait");
        }
        // 4) Wait/reset/acquire for next frame.
        result = m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
        if (result == VK_SUBOPTIMAL_KHR) {
            // An image WAS acquired and its signal is armed on this slot's
            // imageAvailableSemaphore, so the frame proceeds normally. Whether a rebuild is
            // actually needed is decided by the surface-capabilities comparison at the next
            // Present - suboptimal alone must not schedule one, or a driver that reports it
            // every frame would rebuild every frame.
            m_swapchainResizeRequested = m_swapchainResizeRequested || SwapchainIsOutOfDate();
            result = VK_SUCCESS;
        } else if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // Nothing acquired, nothing signaled: safe to rebuild and re-acquire.
            MGLOG_D("Present, vkAcquireNextImageKHR got %d, recreating swapchain", result);
            if (!RecreateSwapchain()) {
                m_presentSuspended = true;
                m_swapchainResizeRequested = false;
                MGLOG_D("Present, zero-area window on next-frame acquire; suspending presentation");
                return;
            }
            m_swapchainResizeRequested = false;
            result =
                m_frameContext.WaitAndAcquireNextImage(m_device, m_swapchainObject.GetHandle(), m_imageIndexAcquired);
        }
        NoteDeviceLoss(result, "vkAcquireNextImageKHR");
        if (result != VK_SUCCESS && LatchWireDeviceLoss("present-acquire")) return;
        VK_VERIFY(result, "Present, vkAcquireNextImageKHR");
        // Pull keeps its historical slot-fence inference. In a disaggregated
        // build the aggregate wait above already retired the registered prefix
        // before FrameContext reset the slot fence; this repeat is idempotent.
        OnSubmitsCompletedUpTo(m_frameContext.GetCurrent().lastSubmitIndex);
        CollectDeferredDepthMipmapCleanup(m_frameContext.GetCurrentFrameIndex());
        // Its previous recording has completed. Drop cached attachment views
        // before the texture manager can release their retired images.
        ClearWireDrawPassCache(m_frameContext.GetCurrentFrameIndex());
        m_textureManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
        m_bufferManager.BeginFrame(m_frameContext.GetCurrentFrameIndex());
        // Descriptor-set reuse cursors rewind exactly once per frame, here,
        // after the slot's fence wait proved its previous sets GPU-idle. (The
        // per-draw-path lazy rewind missed frames whose recording was opened
        // by a staged buffer copy or timer-query timestamp, leaking a fresh
        // descriptor set per draw for the whole frame; it would also be unsafe
        // after a mid-frame FlushPendingCommands, which does not wait.)
        m_uniformManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
    }

    Int32 VulkanRenderer::CurrentDrawBufferAge() {
        if (m_surface == VK_NULL_HANDLE || m_swapchainObject.GetHandle() == VK_NULL_HANDLE || m_presentSuspended)
            return 0;
        m_swapchainObject.NoteBufferAgeAsked();
        if (m_imageIndexAcquired >= m_swapchainObject.GetImageCount()) return 0;
        // The image the next frame's first draw makes the default framebuffer's (it was acquired at
        // the end of the last present). Its content is a frame only if nothing undefined it since.
        if (!m_swapchainObject.IsImageContentDefined(m_imageIndexAcquired)) return 0;
        return m_swapchainObject.BufferAgeOf(m_imageIndexAcquired);
    }

    void VulkanRenderer::ShutdownSwapchain() {
        MOBILEGL_ASSERT(m_renderPassManager != nullptr, "ShutdownSwapchain: render pass manager is null");
        m_renderPassManager->Shutdown();

        m_swapchainObject.Shutdown(m_device);
    }

    Bool VulkanRenderer::RecreateSwapchain() {
        // No surface target is active (its surface was released): nothing to build on.
        if (m_surface == VK_NULL_HANDLE) return false;
        // Handle cases like minimize on Windows, where swapchain could return a 0x0 extent
        const auto swapchainCapabilities =
            SwapchainObject::GetSwapchainCapabilities(m_physicalDevice.handle, m_surface);
        if (swapchainCapabilities.capabilities.currentExtent.width == 0 ||
            swapchainCapabilities.capabilities.currentExtent.height == 0) {
            return false;
        }

        vkDeviceWaitIdle(m_device);
        OnSubmitsCompletedUpTo(m_submitCounter);
        // The old swapchain will be destroyed below, including recordings that
        // are being abandoned instead of submitted. Release all their views now.
        DestroyWireDrawPass();
        CollectWireObjects(m_submitCounter, true);
        ClearAllWireDrawPassCaches();

        if (m_timerQueryManager) {
            // The in-progress command buffer is abandoned below (its recording
            // flags are force-cleared), so timestamp writes recorded into it
            // will never execute; resolve or invalidate all pending records now
            // to keep later waits from hanging on never-available queries.
            m_timerQueryManager->InvalidatePendingRecords();
        }

        DestroyDeferredDepthMipmapCleanup();
        m_deferredDepthMipmapCleanup.assign(m_frameContext.GetFrameCount(), {});

        const VkExtent2D previousExtent = m_swapchainObject.GetSurfaceExtent();
        ShutdownSwapchain();

        // The new swapchain publishes its extent (SwapchainObject::Create), which is how a client
        // sizing its viewport from eglQuerySurface learns that the window moved.
        CreateSwapchain();
        const VkExtent2D extent = m_swapchainObject.GetSurfaceExtent();
        if (extent.width != previousExtent.width || extent.height != previousExtent.height) {
            MGLOG_I("DirectVulkan: swapchain rebuilt at %ux%u (was %ux%u); the new extent is published", extent.width,
                    extent.height, previousExtent.width, previousExtent.height);
        }
        // Every image of the fresh swapchain holds garbage, so the default framebuffer restarts
        // at index 0: the next write into it re-points the GL-visible index at whatever the next
        // acquire returns (see m_defaultFramebufferImageIndex). Without this the index could name
        // an image of the swapchain just destroyed.
        m_defaultFramebufferImageIndex = 0;
        VK_VERIFY(m_frameContext.InitializeSwapchainSemaphores(m_device,
                                                               static_cast<Uint32>(m_swapchainObject.GetImageCount())),
                  "RecreateSwapchain, InitializeSwapchainSemaphores");
        MOBILEGL_ASSERT(m_renderPassManager != nullptr, "RecreateSwapchain: render pass manager is null");
        Bool ok = m_renderPassManager->Initialize();
        MOBILEGL_ASSERT(ok, "RecreateSwapchain: render pass manager initialization failed");
        if (m_pipelineFactory) {
            m_pipelineFactory->DestroyAll();
        }
        InvalidatePipelineMemo(); // pipelines freed -> the memoized handle would dangle
        g_dynamicStateShadow->graphicsPipelineValid = false;
        DestroyComputePipelines();
        if (m_frameContext.GetFrameCount() > 0) {
            m_frameContext.GetCurrent().isCommandRecording = false;
            m_frameContext.GetCurrent().hasCommandBufferRecorded = false;
            // The pre-pass stream paired with the abandoned recording is
            // dropped with it (its next Begin resets the buffer).
            m_frameContext.GetCurrent().isPreCommandRecording = false;
            m_frameContext.GetCurrent().hasPreCommandBufferRecorded = false;
        }
        const Bool okArena = m_bufferManager.RecreateTransientArenas(m_frameContext.GetFrameCount());
        MOBILEGL_ASSERT(okArena, "RecreateSwapchain: buffer manager transient arena initialization failed");
        if (m_frameContext.GetFrameCount() > 0) {
            if (m_textureManager) {
                m_textureManager->BeginFrame(m_frameContext.GetCurrentFrameIndex());
            }
            m_bufferManager.BeginFrame(m_frameContext.GetCurrentFrameIndex());
        }
        return true;
    }

    const PhysicalDevice& VulkanRenderer::GetPhysicalDevice() const {
        return m_physicalDevice;
    }

    Bool VulkanRenderer::SwapchainIsOutOfDate() {
        if (m_surface == VK_NULL_HANDLE || m_swapchainObject.GetHandle() == VK_NULL_HANDLE) {
            return false;
        }
        VkSurfaceCapabilitiesKHR surfaceCaps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice.handle, m_surface, &surfaceCaps) != VK_SUCCESS) {
            return false;
        }
        // A driver-defined currentExtent (UINT32_MAX) means the surface takes its size from the
        // swapchain, so there is nothing to compare against - the app's requested size wins and
        // only an explicit RequestSwapchainResize can change it.
        if (surfaceCaps.currentExtent.width == UINT32_MAX || surfaceCaps.currentExtent.height == UINT32_MAX) {
            return false;
        }
        // Compare in SURFACE space against the extent the live swapchain was created from. Using
        // the swapchain's own (quarter-turn swapped) extent here would report a difference on
        // every rotated frame and rebuild forever.
        const VkExtent2D builtFrom = m_swapchainObject.GetSurfaceExtent();
        const Bool extentChanged =
            surfaceCaps.currentExtent.width != builtFrom.width || surfaceCaps.currentExtent.height != builtFrom.height;
        const Bool transformChanged = surfaceCaps.currentTransform != m_swapchainObject.GetPreTransform();
        if (!extentChanged && !transformChanged) {
            return false;
        }
        MGLOG_D("Swapchain out of date: surface %ux%u transform %u -> %ux%u transform %u", builtFrom.width,
                builtFrom.height, static_cast<Uint32>(m_swapchainObject.GetPreTransform()),
                surfaceCaps.currentExtent.width, surfaceCaps.currentExtent.height,
                static_cast<Uint32>(surfaceCaps.currentTransform));
        return true;
    }

    void VulkanRenderer::RequestSwapchainResize(Uint32 width, Uint32 height) {
        width = std::max<Uint32>(width, 1);
        height = std::max<Uint32>(height, 1);
        if (m_config.SurfaceWidth == width && m_config.SurfaceHeight == height) {
            return;
        }
        m_config.SurfaceWidth = width;
        m_config.SurfaceHeight = height;
        m_swapchainResizeRequested = true;
    }

    void VulkanRenderer::SetSwapInterval(Int interval) {
        if (m_presentsToAppWindow) {
            m_config.SwapInterval = interval;
        }
    }

    void VulkanRenderer::ExchangeActiveTarget(SurfaceTarget& target) {
        std::swap(m_activeTargetSerial, target.serial);
        std::swap(m_window, target.window);
        std::swap(m_platformDisplay, target.platformDisplay);
        std::swap(m_platformLibrary, target.platformLibrary);
        std::swap(m_platformCloseDisplay, target.platformCloseDisplay);
        std::swap(m_fallbackImageReader, target.fallbackImageReader);
        std::swap(m_config.SurfaceWidth, target.surfaceWidth);
        std::swap(m_config.SurfaceHeight, target.surfaceHeight);
        std::swap(m_config.SwapInterval, target.swapInterval);
        std::swap(m_presentsToAppWindow, target.presentsToAppWindow);
        std::swap(m_swapchainSwapInterval, target.swapchainSwapInterval);
        std::swap(m_swapchainResizeRequested, target.swapchainResizeRequested);
        std::swap(m_presentSuspended, target.presentSuspended);
        std::swap(m_surface, target.surface);
        std::swap(m_swapchainObject, target.swapchain);
        m_frameContext.ExchangeSwapchainSemaphores(target.renderFinishedSemaphores);
        std::swap(m_imageIndexAcquired, target.imageIndexAcquired);
        std::swap(m_defaultFramebufferImageIndex, target.defaultFramebufferImageIndex);
    }

    void VulkanRenderer::ParkActiveTarget() {
        if (m_surface == VK_NULL_HANDLE) return;
        // The active surface's recorded work goes to the queue now, while its images are still
        // the default framebuffer's.
        FlushPendingCommands();
        auto& frame = m_frameContext.GetCurrent();
        // Its acquired image keeps its acquire for when the surface comes back, but the signal
        // lives on THIS slot's semaphore, which the next surface acquires with. Wait it out with
        // an empty submission; the image is then simply held, and its next submit waits nothing.
        if (m_swapchainObject.GetHandle() != VK_NULL_HANDLE && !m_presentSuspended &&
            !frame.imageAvailableSemaphoreConsumed) {
            const VkFence fence = AcquirePooledSubmitFence();
            if (fence == VK_NULL_HANDLE ||
                !SubmitPendingCommandBuffer(frame, fence, /*pooledFence=*/true, /*waitAcquire=*/true)) {
                MGLOG_E("DirectVulkan: a parked surface's pending acquire could not be waited out");
            }
        }
        SurfaceTarget parked;
        ExchangeActiveTarget(parked);
        m_parkedTargets[m_activeTargetKey] = std::move(parked);
        m_activeTargetKey = 0;
    }

    void VulkanRenderer::OnActiveTargetChanged() {
        if (m_swapchainObject.GetHandle() != VK_NULL_HANDLE) {
            m_swapchainObject.PublishDefaultFramebufferInfo();
            if (m_programFactory) m_programFactory->SetDefaultFramebufferHeight(m_swapchainObject.GetExtent().height);
        }
        if (m_renderPassManager) m_renderPassManager->SetDefaultFramebufferTarget(m_activeTargetSerial);
        InvalidatePipelineMemo();
        ResetDynamicStateShadow();
    }

    Bool VulkanRenderer::ActivateSurfaceTarget(Uint64 key, NativeWindowType window,
                                               const VulkanRendererConfig& surfaceConfig) {
        if (key == m_activeTargetKey && m_surface != VK_NULL_HANDLE) return true;
        ParkActiveTarget();
        auto parked = m_parkedTargets.find(key);
        if (parked != m_parkedTargets.end()) {
            ExchangeActiveTarget(parked->second);
            m_parkedTargets.erase(parked);
            // Its acquire was waited when it was parked (ParkActiveTarget).
            m_frameContext.GetCurrent().imageAvailableSemaphoreConsumed = true;
        } else {
            m_activeTargetSerial = m_nextTargetSerial++;
            m_window = window;
            m_presentsToAppWindow = window != NativeWindowType{};
            m_config.SurfaceWidth = surfaceConfig.SurfaceWidth;
            m_config.SurfaceHeight = surfaceConfig.SurfaceHeight;
            m_config.SwapInterval = m_presentsToAppWindow ? surfaceConfig.SwapInterval : Nullopt;
            m_swapchainSwapInterval.reset();
            m_swapchainResizeRequested = false;
            m_presentSuspended = false;
            if (!CreateSurface()) {
                // Nothing of the half-built target stays active: its image reader and library go
                // with it, and the renderer has no target until the caller activates another.
                SurfaceTarget failed;
                ExchangeActiveTarget(failed);
                DestroyParkedTarget(failed);
                m_activeTargetKey = 0;
                MGLOG_E("DirectVulkan: surface target for key %llu could not be built (%s)",
                        static_cast<unsigned long long>(key), window != NativeWindowType{} ? "window" : "pbuffer");
                return false;
            }
            CreateSwapchain();
            m_defaultFramebufferImageIndex = 0;
            VK_VERIFY(m_frameContext.InitializeSwapchainSemaphores(
                          m_device, static_cast<Uint32>(m_swapchainObject.GetImageCount())),
                      "ActivateSurfaceTarget, InitializeSwapchainSemaphores");
            if (m_swapchainObject.GetHandle() != VK_NULL_HANDLE) {
                VkResult result = m_frameContext.AcquireNextImageInCurrentSlot(m_device, m_swapchainObject.GetHandle(),
                                                                               m_imageIndexAcquired);
                if (result == VK_SUBOPTIMAL_KHR) result = VK_SUCCESS;
                VK_VERIFY(result, "ActivateSurfaceTarget, AcquireNextImageInCurrentSlot");
                m_defaultFramebufferImageIndex = m_imageIndexAcquired;
            } else {
                m_presentSuspended = true;
            }
            MGLOG_I("DirectVulkan: surface target %llu built (%ux%u, %s)",
                    static_cast<unsigned long long>(m_activeTargetSerial), m_swapchainObject.GetExtent().width,
                    m_swapchainObject.GetExtent().height, m_presentsToAppWindow ? "window" : "pbuffer");
        }
        m_activeTargetKey = key;
        OnActiveTargetChanged();
        return true;
    }

    void VulkanRenderer::DestroyParkedTarget(SurfaceTarget& target) {
        target.swapchain.Shutdown(m_device);
        for (VkSemaphore semaphore : target.renderFinishedSemaphores) {
            if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_device, semaphore, nullptr);
        }
        target.renderFinishedSemaphores.clear();
        if (target.surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_instance, target.surface, nullptr);
            target.surface = VK_NULL_HANDLE;
        }
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
        if (target.fallbackImageReader != nullptr && target.platformLibrary != nullptr) {
            using AImageReaderDeleteFn = void (*)(void*);
            auto* imageReaderDelete =
                reinterpret_cast<AImageReaderDeleteFn>(dlsym(target.platformLibrary, "AImageReader_delete"));
            if (imageReaderDelete) imageReaderDelete(target.fallbackImageReader);
            target.fallbackImageReader = nullptr;
            dlclose(target.platformLibrary);
            target.platformLibrary = nullptr;
        }
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
        if (target.platformDisplay != nullptr) {
            using XCloseDisplayFn = int (*)(Display*);
            auto* closeDisplay = reinterpret_cast<XCloseDisplayFn>(target.platformCloseDisplay);
            if (closeDisplay) closeDisplay(static_cast<Display*>(target.platformDisplay));
            target.platformDisplay = nullptr;
        }
        if (target.platformLibrary != nullptr) {
            dlclose(target.platformLibrary);
            target.platformLibrary = nullptr;
        }
#endif
    }

    void VulkanRenderer::DestroySurfaceTarget(Uint64 key) {
        if (key == m_activeTargetKey && m_surface != VK_NULL_HANDLE) ParkActiveTarget();
        auto parked = m_parkedTargets.find(key);
        if (parked == m_parkedTargets.end()) return;
        // Whatever is recorded may name the target's images (a blit out of it, say): submit it,
        // then let the GPU finish with everything before the views go.
        FlushPendingCommands();
        VK_VERIFY(vkDeviceWaitIdle(m_device));
        OnSubmitsCompletedUpTo(m_submitCounter);
        DestroyWireDrawPass();
        CollectWireObjects(m_submitCounter, true);
        ClearAllWireDrawPassCaches();
        InvalidatePipelineMemo();
        ResetDynamicStateShadow();
        DestroyParkedTarget(parked->second);
        m_parkedTargets.erase(parked);
    }

    Bool VulkanRenderer::FollowActiveWindowResize() {
        if (m_surface == VK_NULL_HANDLE || !m_presentsToAppWindow) return false;
        const auto& frame = m_frameContext.GetCurrent();
        if (frame.isCommandRecording || frame.hasCommandBufferRecorded || frame.isPreCommandRecording ||
            frame.hasPreCommandBufferRecorded) {
            m_swapchainResizeRequested = true;
            MGLOG_I("DirectVulkan: the window resized mid-frame; its swapchain is rebuilt at this frame's present");
            return true;
        }
        // The surface already reports the swapchain's extent: nothing to rebuild.
        if (m_swapchainObject.GetHandle() != VK_NULL_HANDLE && !SwapchainIsOutOfDate()) return true;
        // The target's own rebuild - the path a surface's re-activation takes: its pending acquire waited
        // out, a new VkSurfaceKHR and swapchain at the surface's current extent and pre-transform, a new
        // target serial for the render passes, and the extent published.
        const Uint64 key = m_activeTargetKey;
        const NativeWindowType window = m_window;
        VulkanRendererConfig config;
        config.SurfaceWidth = m_config.SurfaceWidth;
        config.SurfaceHeight = m_config.SurfaceHeight;
        config.SwapInterval = m_config.SwapInterval;
        DestroySurfaceTarget(key);
        return ActivateSurfaceTarget(key, window, config);
    }

    VkExtent2D VulkanRenderer::SurfaceTargetExtent(Uint64 key) const {
        if (key == m_activeTargetKey && m_surface != VK_NULL_HANDLE) {
            return m_swapchainObject.GetHandle() != VK_NULL_HANDLE ? m_swapchainObject.GetSurfaceExtent()
                                                                   : VkExtent2D{0, 0};
        }
        const auto parked = m_parkedTargets.find(key);
        if (parked == m_parkedTargets.end() || parked->second.swapchain.GetHandle() == VK_NULL_HANDLE) {
            return VkExtent2D{0, 0};
        }
        return parked->second.swapchain.GetSurfaceExtent();
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
