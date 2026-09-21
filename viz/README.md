# viz/ — 实时可视化前端（待实现）

这个目录是空的，是**本项目的实际工作区**。开工前请先读：

1. `../PROMPT.md` — 开工提示词（把 `---` 之间那段贴进新会话即可）
2. `../PLAN.md` — 方案（架构 / pass / 着色器接口 / 同步规则 / 里程碑 / 验收）
3. `../data/baseline/BASELINE.md` — 必须保持通过的数值基线

## 要创建的文件

```
viz/
  src/
    viz_main.cpp        WinMain / 帧循环 / 开关状态机
    platform_win32.h/.cpp
    vk_context.h/.cpp   surface + swapchain + 2 帧飞行
    vk_resources.h/.cpp 图形管线 / 描述符 / 深度（reversed-Z）
    vk_scene.h/.cpp     天空 / 塔 / 接收器(emissive flux) / 面板(SSBO pull) / 螺栓
    vk_post.h/.cpp      bloom(5 级) + ACES + sRGB + 热力图 LUT
    gpu_timer.h/.cpp    timestamp 池 + 延迟 2 帧回读
    hud.h/.cpp          5×7 位图字体 + 数值面板
    camera.h/.cpp       轨道相机 + 机位预设
  shaders/
    flux_lite.slang     clearFluxLite / fluxLite / finalizeFluxLite（spp 走 push constant）
    scene.slang         vsScene / fsScene / fsSceneDebug
    post.slang          vsPost / fsBloomPre / fsDown / fsUp / fsComposite
    hud.slang           vsHud / fsHud
```

## 复用的既有代码（不要重写）

| 需要什么 | 用哪个 |
|---|---|
| Vulkan compute 封装、时间戳计时 | `../src/vk.h`（`VkCore`） |
| 前向链条（形变→光追→S95）、UBO 打包、有效像素列表 | `../src/engine.h`（`ForwardEngine`） |
| 资产加载、NPY、CPU S95 参照 | `../src/data.h` |
| 面板/塔/接收器的几何参数 | `../configs/*.json` + `../data/ellipse_north.txt` |
| flux 纹理作为 emissive 贴图 | `ForwardEngine` 内部的 `m_flux`（若需要暴露，加一个 getter） |

## 第一步（D0）

先只做：窗口 + 交换链 + 清屏 + 相机 + 帧率写进窗口标题。
它验证了整条工具链（Slang 编图形着色器 → SPIR-V → 图形管线 → present），
后面全是加法。CMake 的 target 块已在 `../CMakeLists.txt` 末尾注释好。

## 提醒

- 帧循环里**不要**出现 `vkQueueWaitIdle`（`VkCore::submitOneShot` 是给启动期/CLI 用的，别直接拿来当帧循环）。
- Slang 的 SPIR-V 入口名是 `"main"`。
- 改动任何物理相关代码后，跑一遍 `heliostat_core --parity` 再继续。
