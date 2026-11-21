# FusionGL 超分 + 插帧内核 — 正式版 v1 源码

> **作者与分工声明**:本目录下所有修改由 **DSH 会话 AI(DeepSeek Harness,Gemini/GLM 系列模型)于 2026-11-18 完成**。
> 原始插帧(FSR3.1.6 frame generation)实现、双内核分派层、构建思路由**前一位 AI**完成(见 Kelvde/FusionGL 仓库)。
> 本目录是在其工作基础上的**增量修改**,不包含其原有代码的重写。

## 我(本次 AI)做了什么

### 1. 保存世界变慢修复(插帧暂停状态机)
- **问题**:暂停基准采样到失真值(如 2ms 的空闲帧),恢复插帧后被误判"插帧拖慢游戏 9 倍",暂停时长一路翻倍 90→180→300s,叠加长帧续期导致"愚者"整合包保存世界 29 分钟。
- **修复**(`VulkanRendererPresent.cpp` 暂停状态机):
  - 基准采样过滤 `<8ms` 失真帧
  - 翻倍条件加严:基准 ≥16ms 且真慢 >1.5 倍才翻倍
  - 加 **600s 硬上限**:续期不得超过"首次暂停+600s"
  - **JVM 启动期的"长帧→暂停→续期"保护链一字未改**(启动通常 30-120s,远小于 600s)

### 2. FusionSR 超分(自研命名,免 motion vector)
- **问题**:FSR3 时间超分强制要 motion vector,渲染器(GL 翻译层)无法提供,风险巨大。
- **方案**:换赛道做空间+时间重建,不需要任何 motion/depth 输入:
  - `fusion_sr_upsample.comp`:Catmull-Rom 双三次放大(16-tap 展开)+ 时间帧积累(亮度差加权,ping-pong 历史)
  - `fusion_sr_sharpen.comp`:RCAS 风格自适应锐化(邻域钳制防过冲)
  - `shader_compile.cpp`:用 host 端 glslang 库(arm64)把 GLSL 编译成 SPIRV 的独立工具
  - `FfxProbe.cpp` 内 `FfxUpscaleRecord()`:懒初始化 pipeline/descriptor/历史图,失败任一步自动永久停用(绝不影响插帧/渲染)

### 3. 真超分架构(修复鼠标错位/画面越界)
- **问题根源**:
  a) 上一版"缩 surface buffer"(ANativeWindow_setBuffersGeometry)导致 **显示域(87.5%)≠鼠标输入域(100%)** —— 指针错位、画面越界(Pojav 生态共病);
  b) 上一位 AI 遗留的死代码 FSR3.1.5 upscaler context 占 120MB 显存且干扰判断。
- **修复**:
  - **删除链路 B**(setBuffersGeometry 整段移除)—— buffer 恒全屏,鼠标域=显示域
  - **删除死代码 upscaler P0 探测块**
  - 超分接入:MC 在 87.5% viewport 区域渲染(几何安全)→ 超分从缩放尺寸 1:1 重建到全屏 buffer
- `VulkanRendererDevice.cpp` / `VulkanRendererPresent.cpp` 对应修改

### 4. 正式版打包(top.fusiongl.plugin)
- 等长替换包名(`top.fusiongl.tester`→`top.fusiongl.plugin`,19 字符等长,UTF-8+UTF-16LE 双编码)
- **兼容性报告**(`[CONS]` 日志):启动时记录设备名/API 版本/驱动/扩展数/颜色格式 —— 低版本设备排查"进不去"直接看这段
- 日志静默化:超分/插帧异常仅一行提示;崩溃捕获保留(写 `/sdcard/FusionGL/crash_report.txt`)

### 5. 构建流程固化(`build_release.sh`)
- 6 步全校验:源码同步 → ninja 编译 → 内核指纹(尺寸+超分符号存在性)→ 打包+**读回校验 so md5** → 签名+签名后二次校验 → DONE
- 任意一步失败即中止,杜绝"空壳包"

## 我踩过的坑(给后来者)

1. **打包脚本无校验 → 三个包(v3/v4/v5)内核全是旧的**(嵌入的 so 是 v3.0 自带的原版内核,不是新编译的)。用户测了三轮全白测。教训:**打包后必须读回 APK 内的 so 做 md5 比对**。
2. **打包基底用错**:拿 v0.1(最老骨架)当基底,导致用户的配置页/汉化/实验板标识"消失"。教训:**永远用设备上现存安装版做基底**(`pm path` 拉取)。
3. **变量名全局替换误伤**:把 `w[0].sType=` 替换时连带改了 `sw[0].sType=`(子串匹配)。教训:替换后 grep 检查所有数组名。
4. **python 字符串切块删代码吞掉后续函数**(死代码删除时 endmark 定位错误,把后面函数开头也删了)。教训:**大块删除用行号+diff 验证,别用字符串匹配**。
5. **glslang 校验规则**:inductive-loop 必须是标准形式(`i++`/`i+=const`),展开循环代替 for 循环遍历 tap。
6. **strings 校验关键词**:C++ mangled 名(`_ZN8MobileGL...`)搜不到可读名 `FfxUpscaleRecord(`,要么搜 mangled 片段要么搜日志字符串。
7. **APK 安装对齐**:Android 11+ 要求 resources.arsc 未压缩且**数据起点** 4 字节对齐(不是 header offset),签名后需重新验证。
8. **签名差异**:换签名密钥后无法覆盖安装,需先卸载;或者接受共存(不同包名)。
9. **上游已知 bug(修复前必读)**:UniversalScalers-Vulkan PR#1(protected 内存导致全屏色噪)与 PR#2(UMA 设备纯 DEVICE_LOCAL 分配失败)——手机 Mali 全是 UMA,内存类型选择必须排除 PROTECTED 并加 HOST_VISIBLE device-local 回退(已在 `FfxUpscaleRecord` 的 `mt()` 实现)。
10. **ADB 权限弹窗**:每条 android_shell 命令都可能弹确认,命令要**尽量合并成一条**,否则用户来不及点允许。

## 目录结构

```
src/
  FfxProbe.cpp/.h            插帧(P0/P1/P2)+ 超分(US namespace)主实现
  VulkanRendererPresent.cpp  present 链路:暂停状态机 + 超分串联 + 插帧
  VulkanRendererDevice.cpp   surface/设备初始化(链路B已移除)
  VulkanRendererPipeline.cpp 上游 API 代差适配(blendConstants/polygonOffset)
  VulkanRendererInternal.h   (与前一位 AI 版本一致,无修改)
  fusion_sr_*.comp / *_spv.h      超分 shader 源码与编译产物
  shader_compile.cpp         GLSL→SPIRV 工具(链接 host glslang)
  build_release.sh           固化构建+打包+三重校验
  repack_release.py          正式版包名等长替换+重打包
```

## 构建要求

- Android NDK r27c(aarch64)、CMake 3.22+、Ninja
- MobileGL 上游源码树(`mgl/src`,26.11-dev)+ fsr-vulkan 子项目
- JDK17 + Android SDK build-tools 34(apksigner)
- 上游 API 适配点:`ApplyPolygonOffsetState` 2 参版、`vkCmdSetBlendConstants`(详见 Pipeline.cpp 内注释)

## 与上游的关系

- 插帧/分派层:继承自前一位 AI 的工作(Kelvde/FusionGL)
- 超分:本次新增,参考 AMD FidelityFX-SDK 文档与 Slipstream(LLCleanFSR3VK)思路,命名独立为 FusionSR(空间放大+时间积累+锐化属业界通法,未参考任何闭源实现)
- 所有修改均为原创或基于 MIT/LGPL 许可的开源组件

## 测试状态(截至 2026-11-18 18:10)

- ✅ 编译通过、三重校验通过、正式版 APK 已产出
- ⚠️ **真超分架构的实机验证未完成**(鼠标错位是否消除、超分是否真跑通)——用户尚未完成 v7/正式版实机测试
- ⚠️ "愚者"整合包保存修复效果未复测
- 测试方法:装正式版 → 配置页 87.5%+帧生成2倍 → 进游戏看鼠标/画面几何 → 读 `/sdcard/FusionGL/ffx_probe.log` 看 `[FFX-UP]`/`[CONS]` 段
