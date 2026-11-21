第三方组件与许可清单
====================

本项目为研究性质的组合产物，按「组件」列出来源、许可与本项目所做的处理。
许可全文（若适用）放在 licenses/ 目录。

| 组件 | 上游 | 许可 | 本项目处理 |
|---|---|---|---|
| MobileGL（dev 分支） | MobileGL-Dev/MobileGL | GNU LGPL v3.0 | 源码编译为 `libMobileGL.so`；替换 glslang 为项目指定 fork、补齐 SPIR-V 枚举、适配 3 处 glslang API；未改动 Magma/DirectVulkan 后端逻辑 |
| NG-GL4ES（main） | BZLZHH/NG-GL4ES | MIT（含 gl4es 原始版权） | 源码编译为 `libng_gl4es_g720.so`；重链接时把 SONAME 改为 `libng_gl4es_g720.so` 以避免与启动器自带同名库冲突 |
| glslang | KhronosGroup / MobileGL-Dev fork | 多种（BSD-3-Clause、Khronos、Apache-2.0 等，见其 LICENSE.txt） | 作为 MobileGL 的编译期依赖 |
| SPIRV-Cross | KhronosGroup | Apache-2.0 | 编译期依赖 |
| SPIRV-Tools / SPIRV-Headers | KhronosGroup | Apache-2.0 / Khronos 系列 | 编译期依赖（glslang ENABLE_OPT） |
| SPIRV-Reflect | KhronosGroup | Apache-2.0 | 编译期依赖 |
| Vulkan-Headers / Vulkan-Utility-Libraries | KhronosGroup | Apache-2.0 / Khronos 系列 | 头文件依赖 |
| VulkanMemoryAllocator (VMA) | AMD / GPUOpen | MIT | 编译期依赖 |
| xxHash | Cyan4973 | BSD-2-Clause | 编译期依赖 |
| asio | chriskohlhoff | Boost Software License 1.0 | 头文件依赖 |
| flat_hash_map (ska) | Malte Skarupke / MobileGL-Dev | Boost Software License 1.0 | 头文件依赖 |
| Zalith Launcher 2 | MovTery 及贡献者 | GNU GPL v3.0 or later | **仅阅读其公开源码以理解插件接口规范**，未复制其代码进本项目；本项目不包含 ZL2 源码 |
| Android NDK / SDK build-tools | Google | 各自 SDK 许可 | 仅作为构建工具，未随本仓库分发 |

说明
----

- Apache-2.0 组件：若你分发包含这些组件的二进制，需要保留许可文本、
  NOTICE 内容，并对修改作出标记；本仓库的 licenses/ 已含 Apache-2.0 全文一份。
- MIT / BSD / BSL-1.0 组件：需保留版权声明与许可文本，本仓库已含
  NG-GL4ES 的 MIT 全文。
- LGPL-3.0 组件：见根目录 LICENSE 的义务说明。
- 官方 MobileGL 插件 APK 的 dex / 资源文件**未包含在本仓库**，其授权状态
  未由本项目核实，故不随本仓库分发，也不建议上传。

本项目对 MobileGL 源码的具体改动（相对上游 dev 分支快照）：

1. `include/glslang/SPIRV/spirv.hpp11`：按 SPIRV 官方 grammar 自动补齐
   SPIRV-Tools 引用但旧头文件缺失的枚举。
2. `MobileGL/MG_Util/ShaderTranspiler/glslang/TMglGlslIoResolver.cpp`：
   恢复项目 fork 风格的 3 参数 glslang API 调用。
3. CMake 配置：关闭测试 / benchmark / Tracy / 拆分层，
   `ANDROID_ABI=arm64-v8a`、`ANDROID_PLATFORM=android-26`。
4. 未修改 Magma / DirectVulkan 后端实现，未针对 Mali 做架构级定制。
