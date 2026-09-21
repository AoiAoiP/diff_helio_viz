# PLAN.md — Heliostat Studio 实时可视化方案

> 配套文件：`PROMPT.md`（开工提示词）、`NOTES_provenance.md`（来源与取舍）、
> `data/baseline/BASELINE.md`（数值与性能基线）、上游详细设计
> `bezier_opt/docs/viz_demo_implementation_plan.md`（更细的坑位清单）。

---

## 1. 定位

把已有的 GPU 可微光追研究管线（headless、只有数字输出）变成**可交互的画面**：

> 拖太阳 → 光斑实时移动；拖螺栓/收敛滑杆 → 面板形变、S95 数字实时变化；
> 切开关 → 逐 pass GPU 毫秒现场变化。

对游戏渲染岗的表达价值：

| 考点 | 本项目证据 |
|---|---|
| 图形 API 底层 | 手写 Win32 + `VK_KHR_win32_surface` + 交换链 + 帧同步（无 GLFW/引擎） |
| Compute ↔ Graphics 混合管线 | compute 每帧算 32×32 面板网格 → VS 用 SSBO 拉顶点 → 图形 pass |
| Shader（Slang/HLSL） | 复用同一份物理着色器；新增 VS/FS/compute 变体与编译期特化 |
| 性能剖析 | 实测定位并消除**主机侧全同步 + 逐光线全局原子**，端到端 76×（见 BASELINE.md） |
| 现代特性 | HDR + bloom + ACES、reversed-Z、SSBO 顶点拉取、timestamp query、（可选）`VK_KHR_ray_query` |

---

## 2. 已经有的东西（不要重写）

`heliostat_core` 已经把"难的部分"做完并验证过：

```
clearFlux → computeBoltSurface → renderForward* → finalizeFlux → computeS95FindLevel
```

- `src/vk.cpp` — Vulkan compute 封装（instance/device/buffer/texture/pipeline/one-shot submit + timestamp 计时）
- `src/engine.cpp` — 上面这条链的完整驱动，含 UBO 逐位打包、重力 bin 选择、有效像素剔除、S95 状态回读
- `src/data.cpp` — 资产加载（TPS 影响函数 / 20-bin 重力 / 螺栓文件三种格式 / NPY 读写 / CPU S95 参照实现）
- 绑定号与上游一致，所以上游 SPIR-V 可原样复用

**你的工作是加图形层，不是重写计算层。**

---

## 3. 范围守卫（明确不做）

1. ❌ 不引第三方库（GLFW/ImGui/stb/glm/fmt 全部手写替代）。
2. ❌ 不做级联阴影 / IBL / SSAO / TAA；只做 bloom + ACES + LUT。
3. ❌ 不做多镜场（先单镜 North 300 m）。
4. ❌ 不在 viewer 里跑反传（Adam 回路留在上游）；"优化过程"用螺栓预设 + 收敛滑杆呈现。
5. ❌ 不在 MVP 阶段上硬件光追（D6 可选）。
6. ❌ 不改 `shaders/forward.slang`（它是 parity 参照组），实时通道另开 `flux_lite.slang`。

---

## 4. 架构

```
每帧一次 submit（2 帧飞行）
├─ compute  computeBoltSurface.spv   (1,1,1)×1024     ← 复用（已验证）
├─ compute  fluxLite.spv             (tileCount, activePixels)×256  ← 新写（spp 可调）
├─ compute  finalizeFluxLite         (10,4)           ← 新写（tileCount 运行期）
├─ compute  computeS95FindLevel.spv  256×1            ← 复用（已验证），每 N 帧
├─ graphics scene                    sky/tower/receiver(emissive flux)/plate(SSBO pull)/bolts
├─ graphics bloom ×9                 pre → down×4 → up×4
├─ graphics composite                ACES + sRGB + 热力图 LUT
└─ graphics hud                      bitmap font + 数值 + 开关状态
```

### 关键决策（每条都能在面试展开）

- **D1 力学着色器零改动复用**：`shaders/bolt_forward.slang` 直接编进 viewer 的 SPIR-V 目录，绑定号（0–4、6、7、17、19–21、23、24、30、51）与上游一致 → 力学模型零重复。
- **D2 面板不建顶点缓冲**：VS 用 `gl_VertexIndex` 反算 (u,v)，从 `yGrid`/`nGrid`（binding 6/7）拉取（vertex pulling），三角形索引程序化生成，无 index buffer。
- **D3 单描述符集 union 布局**：绑定号沿用上游编号，未用的绑 dummy 缓冲（见 `engine.cpp` 的 `createDescriptorLayout`）→ 切换管线/变体不重写描述符。
- **D4 光路反向**：接收器像素 → 镜面 → 太阳（与上游同构），保证 flux 与主线可对比；画面上的光束箭头按物理方向（太阳→镜面→接收器）反向绘制。
- **D5 A/B 不碰上游**：把"带上游诊断原子"的 `renderForward.spv` 与"去原子"的 `flux_lite` 同时编进来，按键切换即可复现 **5.2×**。

---

## 5. 目录（在 `viz/` 下新建）

```
viz/
  src/
    viz_main.cpp        WinMain/帧循环/开关状态机
    platform_win32.*    窗口、消息、输入
    vk_context.*        surface/swapchain/帧同步（2 帧飞行）
    vk_resources.*      缓冲/纹理/描述符/图形管线
    vk_scene.*          场景几何、光照、热力图
    vk_post.*           bloom + ACES + LUT
    gpu_timer.*         timestamp 池 + 延迟回读
    hud.*               5×7 位图字体 + 数值面板
    camera.*            轨道相机
  shaders/
    flux_lite.slang     ★ 精简光追（spp 可调、去逐光线原子）
    scene.slang         vsScene / fsScene / fsSceneDebug
    post.slang          bloom / tonemap / LUT
    hud.slang           文本与面板
docs/
  perf_log.md           原始性能数据（每次测量一行）
```

CMake：`CMakeLists.txt` 末尾已经留好被注释的 `heliostat_viz` target 块，取消注释并按需补文件即可。链接同样的 `src/vk.cpp src/config.cpp src/data.cpp src/engine.cpp`。

---

## 6. 数据契约（必须逐位对齐）

### 6.1 UBO（与 `engine.cpp::setSun` 一致）

| 绑定 | 名称 | 字节 | 字段 |
|---|---|---|---|
| 0 | ReceiverParams | 40 | pos(3), radius, height, **dims(u32×2) at float 6-7**, onePixelHeight(8), onePixelWidth(9) |
| 1 | HeliostatParams | 44 | sizeW,sizeL, glassDepth, refrIdx, slopeErr, area, reflectivity, 0, 0, cullDirX(9), cullDirZ(10) |
| 2 | SunParams | 52 | dir(3), dni, shapeParams(4-7), shapeIntegral(8), type(9), iterSeed(10), **cullCos(11)**, 0 |
| 3 | heliostatPosition | 12 | |
| 4 | aimPoint | 12 | |

### 6.2 绑定号

`0-4` UBO · `6` yGrid · `7` nGrid · `8` flux(RWTexture2D R32F) · `10` fluxPartial · `17` boltHeights ·
`19/20/21` influence φ, φu, φv · `23/24` yu/yv grid · `30` gravityMerged(20×3×1024) ·
`51` sunBatchFlat(8 floats/sun: dir3, pad, gravLo(u32), gravHi(u32), gravT, pad) · `52` s95State(4 float) ·
`55` activePixelList。其余（5/9/11/12/13/14/15/16/18/22/25/26/27/29/31/53）绑 dummy。

### 6.3 单位与约定（易错，抄写时不要"顺手修正"）

- `gravityMerged` 的 du/dv 平面是**物理斜率** `dw/dx, dw/dz`；shader 里 `yu/yv = 斜率 × (W 或 L)`。
- 顶点采样是 **cell-edged**：`hx = -W/2 + i·W/(GS-1)`（与 `forward.slang` 一致）；`data_proxy` 的 bin 是 pixel-centered，二者差半步——保持与上游一致，别改。
- 法线：`tu=(W,yu,0)`, `tv=(0,yv,L)`, `n = -normalize(cross(tu,tv))`（板局部系），再用 macro basis 转到世界。
- macro basis：`macroN = normalize(normalize(sunDir)+normalize(aim-hp))`；`u = normalize(cross((0,1,0), macroN))`；`v = cross(u, macroN)`。
- 接收器像素：`angle = px·2π/157`；`P = (R sinθ, Ry + py·(H/50) − H/2, −R cosθ)`；`N = normalize(P.xz)`。
- 世界 1 单位 = 1 米，near 1 / far 2000，建议 **reversed-Z**。

---

## 7. 同步规则（性能关键）

1. **每帧一次 `vkQueueSubmit`**，2 帧飞行（2 cmd buffer + 2 fence/时间线信号量 + acquire/present 信号量）。
2. **帧循环里禁止 `vkQueueWaitIdle` / `vkDeviceWaitIdle`**（上游 83 ms/帧的根因）。
3. UBO 与 boltHeights **持久映射**，直接 `memcpy`；只读大数据启动时一次性 device-local。
4. **零每帧分配**：buffer/texture/descriptor/query pool 全部启动时建好；resize 只重建交换链相关。
5. barrier 用精确的 buffer/image barrier（compute 写 → VS 读：`SHADER_WRITE`→`SHADER_READ` + `VERTEX_SHADER`；flux 纹理 compute 写 → FS 读同理）。
6. **回读一律延迟**：timestamp 延迟 2 帧；S95 每 10–30 帧读 4 字节；flux 回读只用于截图/导出。

---

## 8. 着色器

### 8.1 `flux_lite.slang`（核心新写件，派生自 `shaders/forward.slang`）

```slang
struct FluxPC { uint spp; uint tileCount; uint rayCull; uint diagAtomics; };
// spp 默认 64（交互档）/ 256（质量档）/ 1024（与上游对照档）
// 相较 forward.slang 的差异：
//   1. totalSpp / tileCount 走 push constant（上游是编译期 kTileCount=4）
//   2. 去掉 rayValidity 与 diagBuf 的逐光线原子（diagAtomics=1 时保留，用于 A/B 演示）
//   3. 保留：A1 预裁剪、P4 半面剔除、双折射玻璃、Buie 太阳、能量归一化 1/(2π·shapeInt·spp)
```

> ⚠️ 上游 `clearFlux` / `finalizeFlux` 的 `kTileCount` 是编译期常量，**改 spp 后不能复用** → viewer 自备 `clearFluxLite` / `finalizeFluxLite`，`fluxPartial` 按最大 spp 预留（如 4096 → 16 tiles）。

### 8.2 `scene.slang`

- `vsScene`：程序化索引 → 从 `yGrid`/`nGrid` SSBO 拉顶点 → macro basis 转世界；输出 worldPos / worldNrm / uv / 局部高度。
- `fsScene`：面板（玻璃 Fresnel + 镜面高光 + 环境渐变）、接收器（flux × log 映射 × viridis LUT + 自发光）、塔、地面网格、天空渐变；平行光用 sunDir。
- `fsSceneDebug`：法线 / 斜率误差热力图（F7）。

### 8.3 `post.slang` / `hud.slang`

- bloom：亮度阈值 → 5 级降采样(13-tap) → tent 上采样加性 → ACES fitted → sRGB。
- HUD：96 字形 5×7 位图表 + 实例化 quad；先期可用 `SetWindowText` 把统计写进窗口标题（零成本）。

---

## 9. 交互

| 键/操作 | 功能 |
|---|---|
| LMB 拖拽 / 滚轮 / WASD | 轨道相机 / 缩放 / 自由飞行 |
| `1` `2` `3` | A/B 开关：A1 裁剪 / spp / 逐光线原子 |
| `F1–F4` | 机位预设（录 GIF 用） |
| `F5` | 面型预设循环：零螺栓 → LSQ 初值 → 优化最优 → 收敛滑杆 |
| `F6` | 形变放大 1×/50×/200× |
| `F7` | 法线热力图调试视图 |
| `F8` | 光束显示开关 |
| `F9` | 曝光 ± |
| `F10` | vsync 切换（FIFO/MAILBOX） |
| `Space` / `Esc` | 定格截图 / 退出 |

**收敛滑杆（核心创意，零额外数据）**：`h = lerp(0, h_optimized, t)`，`t∈[0,1]`。
数据已在包里：`data/bolts/North_300m_optimized.txt`（另有 `North_300m_lsq_init.txt` 作第三档预设）。

> **端点选择是实测决定的（2026-08 本包实测，单太阳方向 North 300 m）**：
>
> | 螺栓状态 | S95 面积 | 峰值 W/px |
> |---|---|---|
> | 零螺栓（无面型） | **226.67 m²** | 665 |
> | LSQ 椭圆拟合初值 | 47.86 m² | 4824 |
> | 端到端优化结果 | **47.54 m²** | 4780 |
>
> 即：**零螺栓 → 优化面型是 4.8× 的光斑收缩（视觉上极其明显，选它当滑杆端点）**；
> 而 LSQ → 优化只有 0.7%（与研究结论一致：300 m 处理想椭圆面已接近能量守恒最优解，
> 差异在 ≥900 m 才显著）。所以 `F5` 预设三档都保留（零 / LSQ / 优化），
> 但收敛滑杆的 `t=0` 用**零螺栓**——顺带这也是一句很好的面试话术：
> "300 m 处解析拟合与端到端优化的光斑差 0.7%，端到端优化的价值在远距离才体现。"

HUD 两行示例：
```
60.1 FPS | 16.6 ms | flux 2.41 ms (12.7 Mray/s) | scene 1.9 | post 1.2
spp 64 | cull ON | atomics OFF | S95 51.31 m2 | stroke max 35.7 mm | t=1.00
```

---

## 10. 性能预算（基于已实测的核心数据）

实测基线（RTX 4060 Laptop，4.04 M 光线/帧）：

| 项 | 实测 | 说明 |
|---|---|---|
| 纯 GPU（1024 spp） | **0.50 ms** | timestamp query |
| 端到端（含 submit） | 0.64 ms | 墙钟 |
| 端到端（含 flux 回读） | 1.09 ms | 与上游每太阳循环等价 |
| 吞吐 | **8.1 Gray/s** | 4.04 M / 0.50 ms |
| 上游同一负载 | ≈ 83 ms | 主机侧 6–7 次全同步 + 逐光线原子 |

因此 **flux 光追在 64–256 spp 下是"几乎免费"的**（0.03–0.13 ms 量级），
帧预算的主导项将是图形阶段。目标（1280×720，vsync 关）：

| Pass | 目标 |
|---|---|
| computeBoltSurface | ≤ 0.15 ms |
| fluxLite @64 spp | ≤ 0.1 ms |
| scene | ≤ 2.5 ms |
| bloom（9 pass，半分辨率） | ≤ 1.5 ms |
| composite + hud | ≤ 0.6 ms |
| **帧总计** | **≤ 16.6 ms（60 FPS）** |

**要点**：帧时间统计要同时给 GPU timestamp 与墙钟两个口径（本包的 `--bench` 就是这么做的），
并在性能图里注明口径——这是区分"会写渲染"和"会调渲染"的细节。

---

## 11. 里程碑（每个都有可运行验收）

> **进度（2026-08 完成，实测数字见 `docs/perf_log.md` / `docs/perf_viewer.csv`）**
>
> | 里程碑 | 状态 | 验收证据 |
> |---|---|---|
> | D0 窗口与骨架 | ✅ | `tools/acceptance_window.ps1 -Validate`：5 次 resize + 最小化/还原/最大化 + 按键 + WM_CLOSE，exit 0，验证层 0 error；MAILBOX 115 FPS / IMMEDIATE 1166 FPS（D0 场景） |
> | D1 场景 pass | ✅ | `docs/figs/still_1_overview.png`：天空/地面/塔/接收器(157×50 四边形)/面板(SSBO 拉取)/35 螺栓节点/光束，reversed-Z + 4 机位；scene pass GPU 0.043 ms |
> | D2 形变与交互 | ✅ | 收敛滑杆 `t` 实时改变形变与 S95（226.67 → 46.27 m²，`--t 0` vs `--t 1`）；`F5` 三档预设、`F6` 1×/50×/200×、`F7` 法线/斜率/高度调试视图 |
> | D3 光斑与光束 | ✅ | `flux_lite` 1024 spp 与上游基准**逐像素 max abs diff 0.0**；接收器 emissive 热力图 + 光束 + 5 级 bloom + ACES；光追段 0.358 ms（11.3 Gray/s） |
> | D4 性能面板与打包 | ✅ | 逐 pass GPU 毫秒 + Mray/s + 三个 A/B 开关（cull / spp / atomics）；GIF `docs/figs/demo.gif`；8 张静帧；4 张性能/对比图；`docs/perf_viewer.csv` |
> | D5/D6 | ⬜ | 本轮未做（硬件光追版 / 优化轨迹回放） |
> | D7 画面功能扩展 | ✅ | 光斑图内嵌（`F`，0 额外 GPU 成本）· 3 纬度太阳轨迹（`9`/`0`/`L`）· NSWE 罗盘 + 地面方位标（`C`）· 四镜场（`4`/`5`/`6`/`7`，**只有选中镜跑光追**，S95 N/E/S/W = 46.26/55.39/176.25/52.67 m²）· bloom 阈值修复（旧默认 1.0 会把 18.29% 的画面点亮 → 默认 2.5 后 0.21%，`docs/figs/bloom_threshold.png`）· `bloom thr` 滑杆与 `--bloom-thr`/`--bloom`/`--sunpath`/`--mirror`/`--no-flux-map`。回归：parity CLI 1.951e-07 / viewer 逐像素 0.0，验证层 0 error，交互 PASS 8/8，1024 spp GPU 0.485 ms（954 FPS） |
> | D8 | ⬜ | 本轮未做（Vulkan 硬件光追对照 / 优化轨迹回放） |
> | D9 打磨轮 2（HUD / 光斑图 / 面板 / 机位 / 帧率） | ✅ | ① HUD 全部改为**整数像素对齐**绘制 + 整数倍字形块（消除半像素发虚与字形粗细不均），罗盘去掉斜线笔画与重叠半透明形状（脏图案）→ 只剩像素对齐矩形 + 位图文字两种原语；② 光斑图按**圆柱圆形质心卷绕**（North 的 u=0.003 原先被接缝切成两半，峰值偏移 177 px → 现在 4 镜位质心偏移 ≤ 2.5 px）；③ 面板 652×357 → **472×312**（占屏 25% → 16%，pass 行 7 → 5），**操作指南移到屏幕左下**；④ `F2/F3/F4` 改为跟随选中镜（N/E/S/W 的 cam yaw = 196/106/16/−74°），修掉"按 4-7 换镜后 F2 仍是 N300"；⑤ 新增**软件帧率上限**（默认 **60 FPS**，`--fps 120|0`、`F11` 循环、HUD 显示 `cap`）：实测 59.9 / 119.8 / 126.8 FPS。回归：viewer 逐像素 parity 0.0、交互 PASS 8/8、窗口 PASS、验证层 0 报错 |
> | D10 打磨轮 3（螺栓标记） | ✅ | 用户报告"有些螺栓点不在镜面上 / S300m 的螺栓跑到镜外"。定位为**两个显示层缺陷**：① 螺栓云用**引擎基准**（含束靶偏移）而镜面用**每镜基准** → 束靶 ±60° 时 South 两套基准差 **7.91°**、4 个角点全部离开镜面 **0.83 m**（North 仅 0.93°，因为 South 法线几乎竖直 nY=0.99）；② `F2` 近景一直在镜面**背面**（离镜面平面 −10 m），不透明镜面把正面的螺栓/高光全挡住。另修：标记由"以节点为中心的八面体"（下半截埋进镜面）改为**底面贴在镜面上**的小方块、高度改用**双线性**采样（与镜面同一插值），并去掉该路径的动态数组下标。验收：`tools/bolt_probe.py` 差分+连通域实测四镜位可见标记 **35/35/34/35**；新增 `[state]` 诊断 `boltsOut/worst/frame/nrm` 全为 0/0.00 m/≤0.03°；parity（CLI 1.951e-07、逐像素 0.0）、交互 PASS、窗口 PASS 全部保持 |
> | D11 打磨轮 4（德令哈日轨 + 镜位按键联动） | ✅ | ① 太阳轨迹改为**场地真实日轨**：德令哈 **37.37°N / 97.37°E**，赤纬按日期取（夏至 **+23.44°** / 分点 0 / 冬至 −23.44°），时角范围随日期（±109.34° / ±90° / ±70.66°），HUD 同时显示**真太阳时**与**北京时间**（+1h31m）。实测夏至正午 az **0.00°**、el **76.07°**、昼长 **14.578 h**，日出/日落端 el 2.78°；新增 `--sun-hour` 钉住时角以便复现。② `4/5/6/7` 由"只选镜"改为**选镜 + 镜头直接飞到该镜面**，`F2` 改为**飞到当前选中镜**；新增 `tools/acceptance_keys.ps1` 实测 **PASS 9/9**（cam 23.91/−27.91/186.02/75.24°，`--mirror 2` 只选不动，随后 F2 飞到 South）。parity（1.951e-07 / 逐像素 0.0）、交互 PASS、窗口 PASS、验证层 0 报错全部保持 |
> | D12 打磨轮 5（按键可发现性 + 面板自适应 + UI 放大） | ✅ | 用户逐条追问 `M`/`8`/`[`/`]`/`F4`/`F7`/`F8` 的作用，其中**两个是真 bug**：① `[`/`]` 用了 ASCII 0x5B/0x5D 而不是 `VK_OEM_4`/`VK_OEM_6` → **永远无效**（修后单击 ±0.02、按住 0.7/s，实测 t 1.000→0.478→1.000）；② `8` 实际绑的是"关闭日轨"而帮助文字写"field off" → 现在 `8` **真正关闭镜场光追**（整条 compute 链不记录：`flux 0.000 ms`、整帧 GPU 0.34→**0.240 ms**、接收器冷态、S95 归零）。另外：新增**按键确认行 toast**（每个开关在面板显示 3.5 s 并写日志）、面板改为**按实际内容测量底板**并在装不下时**自动缩小 UI 比例**（720p 强制 3× → 自动落到 1.84×）、**自动 UI 比例**（1920×1080 → **2×**，`U` 循环、`--hud-scale` 可钉死）、面板状态由数字改为名字（`view normals (F7)` / `beams normal (F8)` / `heat FIXED` / `camera: <机位>`）。验收：`tools/acceptance_keys.ps1` 扩到 **13 项 PASS 13/13**；parity、验证层、交互、窗口全部保持 |
>
> 与计划的一处重要偏差（如实记录）：A/B 的"逐光线全局原子"在**本包的单次提交、2 帧飞行**结构下
> 实测为 **136×**（0.358 → 48.789 ms），而不是 BASELINE.md 里上游的 5.2×。原因是上游那次测量
> 处在"每太阳方向 6–7 次主机同步"的循环里，原子延迟被暴露在关键路径上；详见
> `docs/perf_log.md` 的 A/B 小节（含 `diagBuf[5] = 808 960 000` 的计数器实证）。

### D0 窗口与骨架（2–3 h）
交付：`viz/` target 可编译；1280×720 窗口；ESC 退出；标题显示 FPS；resize/minimize 不崩。
验收：稳定 60 FPS，拉伸正常，退出无验证层报错。

### D1 场景 pass（1 d）
交付：天空 + 地面 + 塔 + 接收器圆柱 + 面板（未形变 flat/wireframe）+ 平行光 + 轨道相机 + reversed-Z + 4 机位。
验收：绕场景 60 FPS；面板与塔的比例、300 m 距离感正确。

### D2 形变与交互（1 d）
交付：复用 `computeBoltSurface.spv` 的形变 pass + VS SSBO 拉取 + 螺栓小球着色 + 面型预设（零/LSQ/优化）+ 收敛滑杆（t=0 取零螺栓，见 §9）+ 放大系数 + 法线热力图。
验收：拖 `t` 从 0→1 时，S95 从 226.67 降到 47.54 m²（单太阳方向基准），形变可见、帧率不掉；bolt 全 0 时面板为平面。
**实测**：viewer 用 36 方向/200 迭代的优化预设，S95 226.67 → **46.26 m²**（比单方向基准的 47.54 略好，见 `docs/perf_log.md`）。

### D3 光斑与光束（1 d，素材日）
交付：`flux_lite`（含 lite clear/finalize）+ 接收器 emissive 热力图 + 光束 pass + HDR/bloom/ACES + S95 显示 + 太阳滑块驱动全链路。
验收：拖太阳时光斑与光束实时跟随（≥30 FPS）；parity（对照档 1024 spp）仍通过。

### D4 性能面板与打包（0.5–1 d）
交付：逐 pass ms + Mray/s + 三个 A/B 开关 + 位图字体 HUD + 性能 CSV → matplotlib 曲线 + README（首屏 GIF）+ 简历文案 + 3 分钟演示脚本。
验收：演示脚本一次跑通不卡壳。

### D5/D6 可选
- D5：`VK_KHR_ray_query` 硬件光追版（BLAS = 面板网格，TLAS = 面板 + 塔）与 compute 版做 ms/画质对照。
- D6：优化轨迹回放（需在上游加一个 `dump_bolt_trace` 配置项，默认关）。

---

## 12. 验证计划

| 编号 | 验证 | 判据 |
|---|---|---|
| V1 | 物理一致性 | `heliostat_core --parity`：flux sum 相对误差 < 1e-6，S95 位相同（改着色器后必跑） |
| V2 | viewer 的 flux == 引擎的 flux | 同参数（对照档 1024 spp）下 viewer 导出 NPY 与 `--dump-flux` 输出比：sum 相对误差 < 1e-5 |
| V3 | S95 数值 | viewer HUD 的 S95（像素计数 × 0.1601 m²）与 `computeS95Area` 一致（≤1%） |
| V4 | 性能回归 | 连续 3600 帧 min/avg/max 帧时间稳定，显存无增长 |
| V5 | 稳定性 | resize / minimize / 全屏切换 / 双 GPU（独显↔核显）不崩 |

---

## 13. 风险与回退

| 风险 | 缓解 / 回退 |
|---|---|
| Slang 图形阶段入口问题 | `[shader("vertex")]` + `-entry vsScene`；仍失败则加 `-stage vertex`；再失败 scene/post 改用 glslangValidator（SDK 自带），物理仍用 Slang |
| 交换链在核显上行为不同 | 队列族探测 + `VK_ERROR_OUT_OF_DATE_KHR` 全路径；启动参数 `--gpu=discrete|integrated` |
| 实时性不达标 | 三级降级：spp 64→32→16；分辨率 1280×720→960×540；flux 隔帧更新（时间复用） |
| 时间不够 | **保底线 = D0+D1+D2**（窗口+场景+交互形变），配静帧 + 相机环绕 GIF 也能投递 |
| 描述符不匹配 | 先开验证层；把 layout 改成"上游 bolt 布局的精确副本"（`engine.cpp` 已示范） |
| 画面不够"游戏感" | 视觉分级投入：玻璃 PBR + 环境渐变 + bloom + 体积光束 + 电影机位；录屏 60 FPS、关 vsync |

---

## 14. 投递包装

1. 主 GIF ≤15 MB（15 s）：太阳划过 + 光斑跟随 + 收敛滑杆 + 性能开关切换。
2. 静帧 4 张：全景 / 光斑特写（bloom）/ 形变对比 / 法线热力图。
3. 性能曲线 2 张：帧时间 vs spp、A/B 开关柱状图（含 5.2× 那根）。
4. README：首屏 GIF + 3 行卖点 + 构建命令 + 性能表 + 架构图 + "与上游复用关系"。
5. 简历 bullet（数字都已实测）：
   - 基于 **Vulkan 1.4 + Slang** 从零实现 GPU 实时可视化引擎（Win32 平台层 / 交换链 2 帧飞行 / compute-graphics 混合管线 / HDR+bloom+ACES / timestamp 逐 pass 计时）；
   - **性能剖析**：定位逐光线同地址全局原子（400 万次 `InterlockedAdd`/帧，实测 5.2×）与每次渲染 6–7 次 staging 分配 + `vkQueueWaitIdle`，改为持久映射 + 单次 submit，同一 404 万光线负载端到端 **83 ms → 1.09 ms**，纯 GPU **0.50 ms（8.1 Gray/s）**；
   - **可微渲染可视化**：复用研究管线的 Slang 力学/光学着色器（TPS 影响函数 + 20-bin 重力 + 双折射玻璃 + Buie 太阳），GPU compute 每帧更新 32×32 镜面网格，参数拖动 → 形变/光斑/S95 即时反馈。

---

## 15. 附录：本包已验证的实现索引

| 主题 | 位置 |
|---|---|
| dispatch 链 + push constant + barrier | `src/engine.cpp::render` |
| UBO 逐位打包 / 重力 bin 选择 / macro normal | `src/engine.cpp::setSun` |
| descriptor union 布局（绑定号表） | `src/engine.cpp::createDescriptorLayout` |
| 有效像素剔除（P4 半面） | `src/engine.cpp::buildActivePixelList` |
| Vulkan 封装 + 时间戳计时 | `src/vk.cpp::submitOneShot` |
| GPU S95 二分（复用上游） | `shaders/s95_gpu.slang` |
| CPU S95 参照实现（交叉验证用） | `src/data.cpp::computeS95LevelCPU` |
| parity 检查 / 性能 bench | `src/main.cpp` |
