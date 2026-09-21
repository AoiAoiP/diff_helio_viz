# PROMPT.md — 新项目开工提示词

把下面整段（`---` 之间的内容）复制到新项目目录下的新会话里，作为第一条消息。
它是一份"自包含任务书"：不依赖任何历史对话上下文。

---

## 任务：为 Heliostat Studio 核心包实现实时可视化前端（游戏渲染岗作品）

### 0. 你在哪里

工作目录就是这个精简包 `DiffHelio_viz/`。先读这三个文件，它们是你需要的全部上下文：

- `README.md` — 包里有什么、怎么编译运行、已验证的数字
- `PLAN.md` — 项目方案（架构 / pass 设计 / 着色器接口 / 同步规则 / 里程碑 / 验收标准）
- `NOTES_provenance.md` — 每个文件从哪来、改了什么、故意去掉了什么
- `data/baseline/BASELINE.md` — 与上游研究仓库的数值一致性基线（**改动物理代码后必须仍然通过**）

### 1. 目标

给这个 GPU 可微光追研究管线写一个**实时可视化前端**，作为游戏渲染岗位的作品集项目。最终要有三样东西：

1. **一个能跑的 exe**：Win32 + Vulkan 1.4 手写（不引 GLFW/ImGui/任何引擎），60 FPS 交互；
2. **三个画面**：实时光斑（接收器上的 flux 热力图 + bloom）、面板形变（拖螺栓/收敛滑杆）、性能面板（逐 pass GPU 毫秒 + Mray/s + A/B 开关）；
3. **可复现的数据**：性能曲线（帧时间 vs spp、A/B 开关柱状图）+ 一段 GIF + README。

### 2. 硬约束（违反即失败）

1. **零新依赖**：不引入 GLFW / SDL / ImGui / stb / glm / fmt（前者是环境限制，后两者本包已刻意去掉）。平台层、交换链、位图字体全部手写。
2. **不要破坏已验证的核心**：
   - `src/vk.cpp`、`src/data.cpp`、`src/engine.cpp` 的物理与触达方式（dispatch 链、push constant、绑定号、UBO 逐位布局）不要改语义；
   - 任何改动后必须重新跑 `BASELINE.md` 里的 parity 检查，`flux sum` 相对误差 < 1e-6、S95 位相同；
   - 如果需要为实时性加"精简光追"着色器（见 `PLAN.md` §8.4），**新开文件**（如 `shaders/flux_lite.slang`），不要改 `shaders/forward.slang`——后者是 parity 参照组。
3. **`shaders/reference/` 里的反传着色器（bwd_diff）不要编译、不要改**：逆向渲染的 C++ 驱动留在上游研究仓库。
4. **不要 `vkQueueWaitIdle`/`vkDeviceWaitIdle` 出现在帧循环里**：本包上游的 83 ms/帧就是这么来的（见 `BASELINE.md`）。帧循环 = 单次 submit + 2 帧飞行 + 持久映射 UBO + 零每帧分配。
5. **Slang 的 SPIR-V 入口名统一是 `main`**（不是 Slang 函数名），创建管线时 `pName` 传 `"main"`。图形阶段入口同理。
6. **不要写主源码目录 `shaders/*.spv`**：编译产物只进 `build/shaders/`，并 POST_BUILD 拷到 exe 旁边。

### 3. 执行方式

- 按 `PLAN.md` §13 的 D0→D4 顺序推进，**每个里程碑必须有可运行验收**（能跑的命令 + 期望看到的画面 + 期望的帧时间）。
- 每个里程碑结束时自测并报告：帧时间（GPU timestamp 与墙钟两个口径）、是否仍通过 parity 检查、当前与目标的差距。
- 遇到不确定的 Vulkan 行为，**先用 `--validate` 打开 `VK_LAYER_KHRONOS_validation` 跑一遍**，把报错贴出来再修。
- 需要性能数字时用 `heliostat_core --bench` / `--bench-readback` 作为基线对照，不要凭感觉写"更快了"。
- 每完成一个里程碑就更新 `PLAN.md` 里的进度勾选，并把实测数字写进 `docs/perf_log.md`（自建）。

### 4. 起点建议（D0 第一小时）

1. 在 `viz/` 下建 `src/` 与 `shaders/`，按 `PLAN.md` §6 的文件清单建空文件；
2. `platform_win32.cpp`：`RegisterClassEx` + `CreateWindowEx` 1280×720 + 消息循环 + ESC 退出；
3. `vk_context.cpp`：instance（`VK_KHR_surface` + `VK_KHR_win32_surface`）→ 选物理设备时**必须挑出同时支持 graphics+present 的队列族**（本机有 NVIDIA 独显 + Intel Arc 核显两台设备）→ device 开 `VK_KHR_swapchain` + `VK_KHR_timeline_semaphore` → swapchain（`B8G8R8A8_SRGB` + `FIFO`，`minImageCount+1`）；
4. 2 帧飞行：2× cmd buffer + 2× fence + acquire/present 信号量，清屏用动态渲染（`vkCmdBeginRendering`）不需要 render pass 对象；
5. query pool 计时，把 `FPS | GPU ms` 写进窗口标题（先不做位图字体，`SetWindowText` 每 250 ms 一次即可）。

### 5. 完成定义（Definition of Done）

- [ ] `cmake --build build --config Release` 一条命令出 `heliostat_viz.exe`，无警告级错误；
- [ ] 1280×720 窗口稳定 ≥60 FPS（vsync 关），resize / 最小化 / 恢复不崩；
- [ ] 拖太阳方位角滑块，接收器上的光斑与光束实时跟随（≥30 FPS）；
- [ ] 拖螺栓滑块 / 收敛滑杆，面板形变与 S95 数字实时变化；
- [ ] 性能面板显示逐 pass GPU 毫秒 + Mray/s，并能现场切换三个 A/B 开关（A1 逐光线裁剪 / spp / 逐光线全局原子）；
- [ ] parity 检查仍通过（`BASELINE.md`）；
- [ ] `README.md` 首屏一张 GIF + 构建命令 + 性能表；`docs/perf_log.md` 有原始数据；
- [ ] 录一段 15 s 主 GIF，导出 4 张静帧。

### 6. 汇报要求

每次汇报请给出：
1. 本次做了什么（文件级）；
2. **实测数字**（GPU ms / 墙钟 ms / FPS / Mray/s，标注测量口径）；
3. parity 是否仍通过；
4. 下一步与阻塞点（如果有的话，给出最小可复现命令）。

---
