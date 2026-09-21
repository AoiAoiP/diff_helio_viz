# Heliostat Studio — 实时可视化前端

> 语言：**中文** · [English](README.md)

![Heliostat Studio 演示](docs/figs/demo.gif)

一个为 **GPU 可微光追研究管线**手写的 **Win32 + Vulkan 1.4** 实时可视化前端。
没有 GLFW、没有 SDL、没有 ImGui、没有 stb、没有 glm、没有引擎：平台层、交换链、
位图字体、HUD 控件，以及跑在帧循环里的光追 kernel，全部在这个仓库里。

它把 12.84 × 9.45 m 定日镜的**光斑**实时渲染到 157 × 50 像素的接收器圆柱面上
（对数映射热力图 + 5 级 bloom + ACES），可以**拖动面型形变**从平板一直到优化器的结果
（S95 光斑面积 226.67 → 46.26 m²），并用 timestamp query 显示**逐 pass GPU 毫秒**，
同时对光追 kernel 提供三个可现场切换的 A/B 开关。画面里还有**右下角光斑图（带 S95 等值线）**、
**NSWE 罗盘与地面方位标**、**德令哈（37.37°N / 97.37°E）夏至日真实日轨**，以及**四台定日镜的
镜场——只有被选中的那一台才真正执行光追**。

<details>
<summary>上面那张 GIF 演示了什么（27 秒，时轴固定在代码里，可复现）</summary>

1. 太阳划过，光斑跟随（0.36 ms/帧的光追）；
2. 接收器特写：157×50 热力图 + bloom；
3. 收敛滑杆 `t`：平板 → 优化面型（S95 226.7 → 46.26 m²）；
4. A/B 开关：逐光线全局原子 关（0.36 ms）/ 开（48.8 ms）；
5. 调试视图：法线 / 斜率误差 / 面板高度；
6. 四镜场：依次选中 N/E/S/W，只有选中镜执行光追（S95 46 / 55 / 176 / 53 m²）；
7. 光斑图内嵌开关（`F`）——光斑始终居中在右下角；
8. 德令哈夏至日全天日轨（日出 → 日落，HUD 同时给出真太阳时与北京时间）。

</details>

**最有意思的一点**：研究管线需要约 83 ms/帧的同一份 4.04 M 光线负载，在这里的光追
阶段只要 **0.36 ms**（11.3 Gray/s），整帧全质量下 GPU 总耗时 **0.57–0.63 ms**；而且
viewer 的 kernel 与上游参考 flux 图**逐像素位相同**（按上游文档的方位角滚动对齐后
max abs diff = 0.0；sum 480 462.0 W、S95 226.67 m²）。

> 本包复用的研究内核（`src/`、`shaders/`、`data/`、`data_proxy/`）自带一份 README，
> 保留在 `docs/README_core_package.md`，数值基线见 `data/baseline/BASELINE.md`。

---

## 三件值得看的东西

| | |
|---|---|
| **1. 实时光斑** | 太阳方位/高度滑杆每帧驱动整条链：`computeBoltSurface → fluxLite → finalizeFlux → S95`。接收器直接显示 flux 纹理（**每个接收器像素一个四边形**，用与物理一致的**角点约定**采样），所以画面上看到的就是追踪出来的那张图。这里有一个必须说清楚的行为：真实定日镜会随太阳**重新指向**，所以光斑始终锁在 aim point 上——视觉上变化的是镜面姿态与高光、入射光束，以及光斑随入射角的形状/峰值（实测：方位角 ±20° 之间峰值 757 → 724 W/px，S95 221.4 → 221.7 m²）。想让光斑在接收器上明显扫过，用操作说明里的**束靶滑杆**（沿接收器圆周移动 aim point，物理上就是瞄准点本身在动）。 |
| **2. 面板形变** | `t` 滑杆在“零螺栓”与“200 次迭代优化结果”之间插值；面板通过 **SSBO 顶点拉取**直接从 compute 写出的 `yGrid`/`nGrid` 上取点绘制（没有 vertex buffer，没有 index buffer），HUD 上的 S95 数字实时变化。 |
| **3. 性能面板** | `vkCmdWriteTimestamp` 的逐 pass GPU 毫秒、Mray/s，以及三个作用于**实时 kernel** 的 A/B 开关：逐光线预裁剪、spp、上游的逐光线全局原子。切换原子开关会让光追阶段在**同一份工作量**上从 **0.36 ms 变成 48.8 ms（约 136×）**——并且诊断计数器回读可以证明每帧恰好 `4 044 800` 次原子。 |

## 八个必须说清楚的问题

**Q1. bloom 是什么？为什么调高之后会出现意料之外的亮斑？**

bloom 是“**亮通道 → 多级降采样模糊 → 上采样叠加**”的后期：先把超过阈值的亮度抠出来（亮通道），
用 5 级 13-tap 降采样 / 9-tap tent 升采样摊开成大范围光晕，再按强度加回原图。它**不改变任何物理数值**，
只影响观感（太阳盘、镜面高光、接收器核心的辉光）。

亮斑的来源就是这条链的**阈值**：旧的亮通道阈值硬编码为 `1.0`，而本场景的天空渐变本身就接近 1.0，
于是**整片天空都通过了亮通道**，被 5 级模糊摊成大片光晕；再把强度调高，这些光晕就变成“凭空出现的亮斑”。
实测（`docs/figs/bloom_threshold.png`，由 `tools/make_bloom_fig.py` 逐像素对比“关 bloom 的同一帧”）：

| 亮通道阈值（强度固定 1.5） | 被额外点亮的像素 | 占全屏 | 强光晕像素 | 最大增量 |
|---|---|---|---|---|
| 1.0（旧硬编码默认） | **168 579** | **18.29%** | 9 668 | 0.325 |
| **2.5（现在的默认）** | **1 952** | **0.21%** | 498 | 0.307 |
| 6.0（≈关闭） | 0 | 0% | 0 | 0.008 |

修复：① 亮通道阈值默认改为 **2.5**，此时只有真正 HDR 的发射体（太阳盘 ≈16×、镜面高光 ≈8×、
接收器核心 ≈3.6×）会发光，光晕像素减少 **85×**；② 增加 `bloomClamp = 6.0` 萤火虫钳制，
单个超高像素不会再种出一圈亮斑；③ 面板加了 **`bloom thr` 滑杆**（0.5–8.0）与 CLI `--bloom-thr`，
可以现场演示“阈值 1.0 的满屏光晕 → 2.5 的干净画面”。**“奇怪的亮斑”不是渲染错误，而是阈值太低。**
注意：本节的 `--bloom` 是**强度**，`--bloom-thr` 才是阈值；面板上也是两个独立滑杆
（`bloom` 与 `bloom thr`），因为“光晕多亮”和“什么东西允许发光”是两件事。

**Q2. `convergence t` 是什么？**

`t` 是**螺栓行程的插值系数**，把面板从“零螺栓（平板）”连续插值到“200 次迭代的端到端优化结果”
（`data/bolts/North_300m_optimized.txt`）：`boltHeight = t × optimizedHeight`。
`t = 0` 是理想平板，`t = 1` 是完全收敛的优化面型，中间值是“优化到一半”的真实中间态（不是视觉特效，
它真的会用这套螺栓重新算 TPS + 重力形变，再重新跑整条光追）。实测光斑面积 S95：`t=0` **226.67 m²** →
`t=1` **46.26 m²**，即 **4.8× 收缩**。操作里 `F5` 在“收敛 / LSQ 椭圆拟合 / 零螺栓”三档之间切换，
`[` `]` 与滑杆可以连续调；LSQ 档的 S95 是 47.86 m²，比端到端优化略差、但比零螺栓好得多。

**Q3. 画面上显示的面型是优化前的还是优化后的？**

默认 `t = 1`，即**优化后**（200 次迭代、36 个太阳方向）的螺栓面型；要对比优化前，按 `F5` 切到
“零螺栓”或把 `t` 滑杆拉到 0。另外两点区分：

- 面型是**物理算出来的**，不是美术模型：每帧都由 `computeBoltSurface` 在 GPU 上重算 TPS + 重力形变，
  镜面网格通过 SSBO 顶点拉取直接读 compute 写出的 `yGrid`/`nGrid`（没有 vertex/index buffer）。
- `F6` 的 1×/50×/200× 是**纯显示放大**，只影响法线重建的观感，不参与光追；屏幕上的 S95 数字永远来自
  真实面型的追踪结果（例如 South 镜位在同一面型下退化成 176.25 m²，这是物理结果而不是形变缩放造成的）。

**Q4. present 模式有什么区别？三种面型预设分别是什么？**

`F10` 循环三种 present 模式（也可以用 `--present fifo|mailbox|immediate` 指定）。它们是
**交换链把画好的图像交给显示器的三种策略**，只影响“什么时候能提交下一帧”，不影响画面内容：

| 模式 | 行为 | 实测墙钟（本机 59 Hz 面板，关帧率上限） |
|---|---|---|
| `FIFO`（= 垂直同步） | 队列 1 张图，必须等显示器取走才能提交下一张 → 无撕裂，帧率被锁在刷新率 | ≈ 59 FPS |
| `MAILBOX`（默认） | 队列 3 张图，新帧**替换**待显示的旧帧 → 无撕裂，但**不锁死** app 的提交节奏 | 115 FPS（显示器仍只显示 59） |
| `IMMEDIATE` | 直接送，不等 vblank → 可能**撕裂**，上限只由 GPU 决定 | 841 FPS |

所以 `MAILBOX` 下“115 FPS”是**提交速率**而不是“显示器看到了 115 帧”——这正是需要
`--fps`/`F11` 软件帧率上限的原因：想要**确定性的 60 帧节奏**，任何 present 模式都给不了。

`F5` 循环三种面型预设（都是真实算出来的螺栓行程向量）：

| 预设 | 是什么 | 实测 S95 |
|---|---|---|
| **零螺栓** | 理想平板（螺栓全部归零），作为“未优化”基准 | 226.67 m² |
| **LSQ 椭圆拟合** | 单参数解析拟合的近似面型（`North_300m_lsq_init.txt`），优化前的工业做法 | 47.86 m² |
| **端到端优化** | 36 个太阳方向 × 200 次迭代的可微优化结果（默认档，`t` 滑杆的上端点） | **46.26 m²** |

**Q5. 原子 A/B 是什么？为什么关掉之后提高 spp 帧率仍然很高？**

`3` 开关的是**上游研究内核里的逐光线全局原子操作**（`forward.slang` 无条件执行）：
每个接收器像素的每条光线做一次 `InterlockedAdd(diagBuf[5], 1)`，并且对“光线有效位图”
（binding 29）做**分散的** `InterlockedOr(rayValidity[rayIndex >> 5], 1u << (rayIndex & 31))`。
实时 kernel 把它做成开关，就是为了把这份开销单独量出来：

| 配置（1024 spp，4.04 M 光线） | flux 段 GPU | 说明 |
|---|---|---|
| 实时 kernel，原子 **关** | **0.358 ms** | 出厂配置 |
| 实时 kernel，原子 **开** | **48.79 ms（≈136×）** | 计数器回读证明每帧恰好 `4 044 800` 次原子 |
| 只保留同地址计数器（去掉分散位图写） | +3.5% | 代价几乎全在那张**分散位图**的内存流量上，不在计数器本身 |
| 上游参照链 `forward.slang`（原子恒开） | 78.0 ms | 上游在“每太阳方向主机同步”的循环里测得 5.2×，因为那时原子延迟不在关键路径上 |

**为什么关掉原子后提高 spp 帧率还很高**：因为光追段的时间**随 spp 线性**，而在关闭原子时它的
绝对值很小——64 spp 只要 0.086 ms、1024 spp 也只要 0.358 ms，而 60 FPS 的帧预算是 16.7 ms。
即使把 spp 从 16 提到 1024（光线数 ×64），增加的时间也只有 0.28 ms 左右，占预算的 1.7%；
不提速的瓶颈根本不在 GPU（默认 60 上限下是帧率上限，不限速时是 present 队列约 7.9 ms）。
反过来，打开原子后同一份工作量要 25–49 ms，**已经超过 16.7 ms 的预算**，此时再提高 spp 就是
在一个已经超支的项上继续乘系数，于是帧率崩到 20–40 FPS。

**Q6. “微调收敛”到底在调什么？**

`[` `]`（以及收敛滑杆）调的是同一个量 `t`：**螺栓行程向量的插值系数**
`boltHeight = t × optimizedHeight`，按键步进 0.7/s、滑杆是 0.01 精度。它不是“画面淡入淡出”，
每一帧都真的会重跑一遍物理链：

1. 螺栓高度 → `computeBoltSurface` 用 TPS 影响函数叠加 + 20-bin 重力模型算出**新的镜面网格**
   `yGrid`/`nGrid`（compute shader，每帧）；
2. 镜面 mesh 通过 SSBO 顶点拉取直接读这份网格（没有 vertex/index buffer），所以形变立刻可见，
   法线也是从网格重新算的；
3. 同一份法线进入光追：每条光线在镜面上的反射方向变了 → 落点变了 → **光斑形状/峰值/S95 全部重算**。

所以“微调”看到的现象是：光斑从 226.67 m² 连续收缩到 46.26 m²、峰值从 665 W/px 升到 4808 W/px，
面板里的 S95 数字每帧跟着变。中间值是**真实的中间面型**（例如 `t = 0.5` 是“螺栓只拧了一半”
的物理状态），不是两张图的混合。

**Q7. 每个按键到底做什么？为什么有的按键“没反应”？**

面板上每个按键都会留一行琥珀色确认（约 3.5 秒）并写进日志，所以“生效了但看不出来”和“按键坏了”
可以区分开。逐条说明（其中 `8` 和 `[` `]` 曾经是**真 bug**，已修）：

| 按键 | 作用 | 备注 |
|---|---|---|
| `M` | 热力图量程 **自动 ↔ 固定** | 自动档按回读峰值 ×1.25 平滑跟随；固定档把上限钉在当前值。两种情况下画面可能几乎一样（`t=1` 时峰值稳定），所以看面板的 `heat AUTO/FIXED<=5940` 与 toast |
| `8` | **镜场光追 开/关** | 关的时候**整条 compute 链不记录**：光追真的不跑，per-pass 表里 `flux` 变成 **0.000 ms**（整帧 GPU 从 ~0.34 → **0.240 ms**）、接收器转冷态、S95/峰值归零、镜场不再绘制。`forward.slang` 等内核本身没动 |
| `[` `]` | **微调收敛 t**（单击 ±0.02，按住 0.7/s） | 曾经完全无效：`[`/`]` 不是 VK 码（Windows 上是 `VK_OEM_4`/`VK_OEM_6`），旧代码拿 ASCII 比较永远不成立 |
| `F4` | 机位：**光束侧视** | 从选中镜方位 +60°、190 m 外侧看入射光束与反射光束 |
| `F7` | 镜面视图四档 | 面板直接写名字：`shaded` 着色 / `normals` 法线 / `slope error` 斜率误差（对宏观法线，满量程 30 mrad）/ `height` 高度（±60 mm） |
| `F8` | **光束三档**：`off` / `normal` / `strong (2×)` | 光束是加性很淡的效果（`alpha ≈ 0.16×脉冲`），面板现在显示 `beams normal (F8)`，所以能确认档位 |
| `F9` | 曝光 ×1.25（`Ctrl+9` 回退） | 面板 `exposure` 滑杆同步 |
| `U` | **UI 比例**：AUTO → 1 → 1.5 → 2 → 2.5 → 3 | 面板与状态行显示当前比例；AUTO 按窗口尺寸取整（1920×1080 → 2×） |
| `F11` | 帧率上限 60 / 120 / 不限 | HUD 首行显示 `cap 60/120/off` |
| `F1`–`F4` | 机位预设（全景 / 飞到选中镜 / 接收器光斑 / 光束侧视） | 面板显示 `camera: <当前机位>` |
| `1` `2` `3` | A/B：A1 预裁剪 / spp 档位 / 逐光线全局原子 | 面板显示 `cull / atomics`，`2` 会 toast 新的 spp |
| `F5` `F6` | 面型预设（收敛 / LSQ / 零螺栓）· 形变放大 1×/50×/200× | `F6` 是**纯显示放大**，不参与光追 |
| `P` `K` `H` `L` `F` `C` | 暂停 · 束靶回中 · 隐藏面板 · 日轨动画 · 光斑图内嵌 · 罗盘 + 方位标 | 全部有 toast / 面板状态 |

**Q8. 螺栓标记为什么有的看不见、S300m 的还跑到镜外面？**

两个独立的原因，**都是显示层**（物理链路一直是对的——parity 前后都是 CLI 1.951e-07、
逐像素 0.0）：

1. **螺栓云和镜面用了两套基准**。镜面用“每镜记录”（`helioPosOf/helioToWorldDir`），
   而螺栓用的是引擎的宏观基准（`scene.macroN/U/V`）。引擎在有**束靶偏移**时会按偏移后的
   瞄准点重算法线（物理上定日镜确实重新指向了），查看器画镜面时却忽略了这个偏移 →
   两套基准分叉。实测：束靶 ±60° 时 South 镜两套基准差 **7.91°**、**四个角上的螺栓全部
   离开镜面（最多 0.83 m）**；North 只差 0.93°，因为 South 的宏观法线几乎竖直（nY = 0.99），
   它的基准对瞄准点极其敏感。修法：螺栓改用与镜面**完全相同**的每镜基准，并且让
   `aimPointFor()` 也带上束靶偏移（顺带修掉“束靶≠0 时画出来的镜面与被追踪的镜面不一致”）。
   现在所有镜位、所有束靶角都是 **0 个越界角点 / 0.00 m / ≤0.03°**。
2. **`F2` 近景一直是从镜面背面看的**（相机落在法线负侧约 10 m），不透明镜面就把长在正面的
   东西全挡住了——螺栓、镀膜高光、镜面反射都看不到。现在近景沿**法线正侧**取景
   （`yaw = azimuth(n)+22°`、`pitch = elevation(n)+14°`）。

另外两处也一并修了：螺栓原来是**以节点为中心**的八面体、下半截埋在不透明镜面里（只露半个）；
螺栓高度原来取最近网格节点而非镜面的双线性插值高度（F6 放大到 50×/200× 时会浮起/沉下）。
现在 `tools/bolt_probe.py`（把标记尺寸设为 0 与 0.30 m 各渲一帧、逐像素相减隔离标记像素、
再做连通域计数）实测四个镜位分别可见 **35 / 35 / 34 / 35** 个标记（South 的两个在透视下并成
一个连通域）。**总览机位（F1）看不见标记是几何结果**：相机与镜面几乎共面（227 m 距离上离
镜面平面只有 9.8 m ≈ 2.5° 掠射），0.44 m 的小方块投影不足一个像素；按 `F2` 近景或加大标记
（`--bolt-scale 0.5`）就能看到。

---

## 构建

依赖只有：**Vulkan SDK 1.4+**（头文件、loader 和随附的 `slangc`）与 Visual Studio 2022。
没有别的东西，构建期不需要网络。

```powershell
# 注意：优先使用原生 cmake；PATH 上的 msys2/msys cmake 在受限 shell 里可能失败。
& "C:\Program Files\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
& "C:\Program Files\CMake\bin\cmake.exe" --build build --config Release
```

一条命令产出两个可执行文件，SPIR-V 会在 POST_BUILD 阶段拷到 exe 旁边：

```
build\Release\heliostat_core.exe   无头前向引擎（已与上游对齐验证）
build\Release\heliostat_viz.exe    实时 viewer
build\Release\shaders\*.spv        slangc 编译产物，POST_BUILD 拷到这里
```

## 运行

```powershell
cd build\Release

# viewer（资产与 SPIR-V 会自动解析，任何工作目录都能跑）
.\heliostat_viz.exe

# 脚本化 27 秒演示：相机、滑杆、镜场与日轨按固定时间轴自动走一遍
# （序列结束后默认交还交互；加 --demo-exit 才会退出进程，录 GIF 用的是它）
.\heliostat_viz.exe --demo 27 --validate
.\heliostat_viz.exe --demo 27 --demo-exit --hud-scale 1.7 --record ..\..\out\gif --record-stride 4

# 可脚本化的状态行与控件几何（验收脚本用的就是这两个）
.\heliostat_viz.exe --state-log --ui-layout --log ..\..\out\logs\run.log

# 德令哈日轨：夏至日正午（时角钉在 0，动画暂停，可复现）
.\heliostat_viz.exe --sunpath 1 --sun-hour 0

# 数值自检：viewer 的 kernel 对比上游基准 flux 图
.\heliostat_viz.exe --frames 40 --spp 1024 --sun 1 --t 0 --dump-flux ..\..\out\flux.npy
.\heliostat_viz.exe --frames 40 --spp 1024 --reference-chain --dump-flux ..\..\out\flux_ref.npy

# 性能扫描 -> CSV + 作图
powershell -File ..\..\tools\perf_sweep.ps1
python ..\..\tools\plot_perf.py

.\heliostat_viz.exe --help
```

**操作方式**：左键拖拽转轨道 / 直接拖面板滑杆 · 右键/中键平移 · 滚轮缩放 ·
WASD/QE 自由飞行（Shift 加速）· 面板滑杆（太阳方位、太阳高度、**束靶**、收敛 `t`、
曝光、bloom、**bloom 阈值**）· **每个按键都会在面板显示一行确认（toast）并写进日志** ·
`1` 裁剪 A/B · `2` spp 档位 · `3` 原子 A/B · `H` 隐藏面板 · `M` 热力图量程 自动/固定 ·
`K` 束靶回中 · `F` 右下角光斑图开关 · `C` 罗盘 + 地面方位标 ·
**`4`/`5`/`6`/`7` 选中 N/E/S/W 镜并把镜头直接飞过去** · **`8` 关闭/开启镜场光追**
（跳过整条 compute 链：光追真的不跑，per-pass 表里 flux 变 0.000、接收器转冷态）·
`9` 德令哈日轨演示（夏至 / 分点 / 冬至）· `0` 关闭日轨 · `L` 轨迹动画 · `←/→` 调时角 ·
**`[` `]` 微调收敛 `t`**（单击 ±0.02，按住 0.7/s）· `F11` 帧率上限 60/120/不限 ·
**`U` UI 比例**（AUTO / 1 / 1.5 / 2 / 2.5 / 3）·
机位：`F1` 全景 · **`F2` 飞到当前选中的镜面** · `F3` 接收器光斑 · `F4` 光束侧视 ·
`F5` 面型预设 · `F6` 形变放大（1×/50×/200×）·
`F7` 镜面视图（`shaded` 着色 / `normals` 法线 / `slope error` 斜率误差 / `height` 高度，面板显示当前档）·
`F8` 光束三档（off / normal / strong 2×）· `F9` 曝光 · `F10` present 模式 ·
`Space` 截图 · `ESC` 退出。

**界面布局**：左上角是**状态面板**（帧率 / 逐 pass GPU 毫秒 / 7 个滑杆 / 当前镜位·面型·视图·光束·UI 比例状态），
**操作指南在屏幕左下角**，罗盘在右上角，光斑图在右下角。默认 **60 FPS 上限**（`--fps 120`
或 `F11` 可切换，`--fps 0` 不限速）。

**UI 与字体大小**：默认**自动**——按窗口尺寸取档（1280×720 → 1×、**1920×1080 → 1.5×**、
2560×1440 → 2×、3840×2160 → 3×），字形始终是**整数倍像素块**（1.5× 时 8×11 px），不会糊。
`U` 键循环切换（AUTO/1/1.5/2/2.5/3）、`--hud-scale <f>` 可钉死、`H` 一键隐藏面板。
面板盒**按实测内容双向拟合**（宽=最宽一行，高=内容高度），装不下时自动缩小比例重排，
所以日轨演示那种"多两行"的情况永远塞得下（实测 1280×720 强制 3× → 自动落到 1.84×）。
面板占屏面积实测：**17.9%**（1280×720）、**18.0%**（1920×1080，1.5×）、18.2%（2560×1440，2×），
开启日轨后 18.7%——`tools/panel_probe.py` 会核对"盒子里确实有内容"（内容像素占比 26–42%）。

### 四个可以现场演示的画面功能

| 功能 | 操作 | 演示内容 |
|---|---|---|
| **右下角光斑图** | `F` 开关（或 `--no-flux-map` 默认关） | 接收器 157×50 的 flux 纹理直接画成内嵌热力图，带 **S95 等值线**与 `S95 / peak` 数值。光斑按**圆柱圆形质心卷绕**，所以**无论哪个镜位、光斑都在面板正中**（North 的光斑质心原本正好骑在 u=0 接缝上、被切成两半）。复用每 15 帧一次的同一份回读，**0 额外 GPU 成本**。 |
| **德令哈夏至日日轨** | `9` 循环（夏至 / 春·秋分 / 冬至），`0` 关闭，`L` 动画，`←/→` 调时角 | 场地取**中国青海德令哈 37.37°N / 97.37°E**（50 MW 熔盐塔式电站），轨迹是该场地的**真实日轨**：赤纬按日期取（夏至 **+23.44°**），时角从日出扫到日落，正午高度角 = 90°−\|φ−δ\|。夏至日正午 **76.07°**、昼长 **14.58 h**，HUD 同时给出**真太阳时**与**北京时间**（德令哈在 120°E 以西 22.63° → 北京时间 = 真太阳时 + 1h31m）。轨迹驱动的是同一条物理链，不是演示模式。 |
| **NSWE 方位** | `C` 开关 | 右上角罗盘（N 蓝 / E 绿 / S 红 / W 琥珀 + 太阳方向菱形）＋ 地面 22–320 m 的方位条带。**本场景世界轴**：+Z = 镜面朝向（北半球镜场 = 南），+X = 东；−Z = 北、−X = 西。太阳方位角同样以**正南为 0°**。 |
| **四镜场 + 选中才光追** | `4`/`5`/`6`/`7` **切换并把镜头飞到** N/E/S/W 镜面，`8` 关闭 | 四台定日镜各自有基础/塔/接收器链路，**只有被选中的那台执行光追**，其余画成重新瞄准后的平板并压暗。实测 S95（1024 spp、`t 1`）：North **46.26**、East **55.39**、South **176.25**、West **52.67 m²**。 |

> 四台镜共用同一份 **North 优化螺栓预设**，因此 E/S/W 的语义是“**同一面型重新瞄准**”，
> 不是各自独立优化；这一点在 HUD 上也会标注（`mirror East (traced)`）。

---

## 性能（实测，RTX 4060 Laptop，1280×720，关 vsync）

口径说明：`GPU ms` 是 `vkCmdWriteTimestamp` 的差值；`wall ms` 是 `steady_clock` 包住整个
迭代（含 present 与系统合成器）；**下面的 FPS 都是 `--fps 0`（关掉默认 60 上限）的口径**，
不设上限的数字用 `VK_PRESENT_MODE_IMMEDIATE_KHR`。默认 60 上限下 `wall ms` 就是 16.7 ms。

| spp | 光线/帧 | flux 段（GPU） | GPU 总计 | wall（IMMEDIATE） | 吞吐 |
|---|---|---|---|---|---|
| 16 | 63 k | 0.082 ms | 0.204 ms | 0.88 ms（1137 FPS） | 0.8 Gray/s |
| 64 | 253 k | 0.086 ms | 0.208 ms | 0.91 ms（1097 FPS） | 2.9 Gray/s |
| 256 | 1.01 M | 0.121 ms | 0.242 ms | 1.20 ms（835 FPS） | 8.4 Gray/s |
| 1024 | 4.04 M | **0.357 ms** | **0.57–0.63 ms** | 1.19–1.41 ms（708–843 FPS） | **11.3 Gray/s** |

1024 spp 的逐 pass（GPU，ms，**最终构建 + 全部功能默认开启**）：deform 0.019 ·
**flux 0.357** · scene 0.049 · bloom（9 个 pass）0.078 · composite 0.011 · **总计 ≈0.60**。
默认 `MAILBOX` 模式下墙钟被 present 队列卡住（约 8.7 ms → 59 Hz 面板上 115 FPS），
HUD 会把两个口径都显示出来。

**同一份 4.04 M 光线负载下的 A/B 开关**：

| 配置 | flux 段（GPU） | wall | 说明 |
|---|---|---|---|
| 实时 kernel（`flux_lite.slang`） | **0.358 ms** | 1.12 ms | 出厂配置 |
| 关闭 A1 预裁剪 | 0.351 ms | 1.24 ms | 裁剪跳过的是那些“可证明贡献为 0”的光线的折射与太阳形状计算，所以收益很小 |
| **打开逐光线全局原子** | **48.79 ms** | 47.6 ms | 上游的 `InterlockedAdd(diagBuf[5])` + 分散的 `InterlockedOr(rayValidity[…])`，每帧 4 044 800 次原子（读回计数器实证） |
| 参照链（`forward.slang`，上游文件原样，原子恒开） | 78.0 ms | 84.2 ms | parity 参照组，同进程运行做对照 |

**帧率上限**（`--fps` / `F11`，默认 60）：60 → 墙钟 16.687 ms（**59.9 FPS**）；
120 → 8.349 ms（**119.8 FPS**）；`0`（MAILBOX）→ 126.8 FPS；`0` + `IMMEDIATE` → 708–843 FPS。
实现是**软件排程**：单调时钟 + 高精度可等待定时器（`CreateWaitableTimerExW`，kernel32）粗睡 +
自旋收尾，零新依赖、不动全局 `timeBeginPeriod`。

**与无头引擎对照**：

| 路径 | GPU | 每渲染墙钟 |
|---|---|---|
| `--bench 200`（仅渲染，1024 spp） | 0.50 ms | 0.64 ms |
| `--bench-readback 200`（等价于上游每太阳循环） | 0.50 ms | 1.09 ms |
| 上游研究仓库，每个太阳方向 | — | ≈ 83 ms |

## 数值一致性

| 检查 | 结果 |
|---|---|
| `heliostat_core --parity`（上游基准 NPY） | flux sum 相对误差 **1.95e-07**；peak 664.911 相同；S95 level 75.207809 相同；S95 area 226.67 m²（0.00%） |
| viewer `flux_lite`（1024 spp）vs 上游基准 NPY，逐像素 | 按文档的 79 像素方位角滚动对齐后 **max abs diff 0.000000e+00**，sum 480 462.0 W 相同 |
| viewer `flux_lite` vs 同进程同 UBO 同描述符集运行的 `forward.slang` | **逐像素 diff 0.0** —— 实时 kernel 是被验证 kernel 的精确复现 |
| S95（GPU 协作二分）vs CPU 参照实现 | 相对差 0.00e+00（位相同） |

## 架构

```
one vkQueueSubmit per frame, 2 frames in flight, no per-frame allocation
│
├─ compute  clearFluxLite         (10,4)         new: runtime tile count
├─ compute  computeBoltSurface    (1,1,1)        reused upstream SPIR-V (TPS + 20-bin gravity)
├─ compute  fluxLite              (tiles, 3950)  new: spp 16..1024, cull/atomics A/B
├─ compute  finalizeFluxLite      (10,4)         new
├─ compute  computeS95FindLevel   256×1          reused upstream (cooperative bisection)
│     + sample readback every 15 frames -> S95 area, flux sum, verified counters
├─ graphics scene       sky / ground / tower / receiver (157×50 emissive quads)
│                       / plate (vertex pull from yGrid+nGrid) / 35 actuator markers / beams
├─ graphics bloom ×9    bright pass + 4× 13-tap down + 4× tent up (5 levels, half res)
├─ graphics composite   ACES + exposure + vignette -> sRGB swapchain (dynamic rendering)
└─ graphics hud         5×7 bitmap font + panel + sliders, one vertex buffer, no ImGui
```

帧循环里被强制执行的规则（`viz/src/viz_main.cpp`、`viz/src/vk_context.cpp`）：

* **每帧一次 submit**，2 帧飞行，逐 image fence，`MAILBOX`/`IMMEDIATE` present；
* 帧循环路径上**没有 `vkQueueWaitIdle` / `vkDeviceWaitIdle`**（唯一的 device idle 在关闭时）；
* **每帧零分配**：buffer、descriptor、query pool、pipeline 全部预建，逐帧数据写进持久映射的
  UBO 切片；
* **所有回读都是延迟的**：GPU 时间戳在 2 帧后收、flux 图每 15 帧一次、截图延后 2 帧。

## 与上游研究内核的复用关系

| 原样复用 | 本包新写 |
|---|---|
| `shaders/common.slang`、`sunshape.slang`、`bolt_common.slang`、`forward.slang`（parity 参照组）、`s95_gpu.slang`、`bolt_forward.slang` | `viz/shaders/flux_lite.slang`、`scene.slang`、`post.slang`、`hud.slang` |
| `src/vk.cpp`（compute 封装）、`src/data.cpp`、`src/engine.cpp` 的 dispatch 链、push constant、绑定号、UBO 逐位布局 | `viz/src/{platform_win32, vk_context, vk_gfx, vk_flux, vk_scene, vk_post, hud, gpu_timer, camera}.cpp` |
| flux/S95 物理、TPS + 重力面板模型、有效像素剔除 | 整个图形层、帧循环、HUD、性能管线 |

完整对照表（哪些是上游、哪些是新写，以及每一处的证据）在 `NOTES_provenance.md`。

## 过程中发现并修掉的缺陷

### 研究内核里的两个潜在缺陷（不影响任何物理数值）

1. `forward.slang` 的逐光线 P2 有效性位图（binding 29）索引可达 ~250k uint（≈1 MB），
   而 engine 把它绑到一个 4 KB 的 dummy buffer 上 → **越界写**；现在绑定一个按
   `spp*pixels/32` 精确分配的 scratch buffer。
2. `loadConfig()` 对读不到的路径**静默返回默认值**，而 CSR 推导出的 Buie 常数只在成功读文件时
   才算 → 从 `build\Release` 用相对路径启动时太阳形状参数退化成 (0,0)，flux 总量偏高 **68×**。
   现在显式解析配置路径并在失败时告警。

### 修复轮的 7 条（用户反馈“UI 镜像 / 画面自动切换后自动关闭”之后）

| # | 现象 | 根因 | 修前 → 修后证据 |
|---|---|---|---|
| 1 | HUD 整体上下镜像（文字倒置、面板跑到画面底部） | `vsHud` 沿用了场景通道的 NDC 约定：场景渲染进 HDR 时 NDC 是 y-up，由 composite 翻转上屏；而 HUD 直接画到交换链，必须显式 `ndc.y = y/H*2-1`（Vulkan 帧缓冲原点在左上、y 向下） | 修前绿色标题在 rows 628..683；修后 rows 31..78，ASCII 预览可读出 “HELIOSTAT STUDIO” |
| 2 | **接收器热力图完全没显示**（本项目最核心的画面） | UBO 打包错位一格：`pixelHeight` 写到 float 76，而 shader 读的 `ground.w` 是 float 75 → 接收器只画了 **12 行**（应为 50 行） | 热力图色像素 **2 285 → 53 682**；随滑杆变化：t=0 亮区 240 px 高，t=1 聚焦后 106 px |
| 3 | **收敛滑杆 / F5 面型预设无效** | 预置螺栓文件按 `data_proxy/../bolts` 找，实际在 `data/bolts/` | `--t 1` 的 S95 **226.67 → 46.26 m²** |
| 4 | 面板滑杆点不中 | `WM_LBUTTONDOWN` 不更新指针位置 | 改用消息里的坐标（日志实证） |
| 5 | 自动化脚本读不到日志 | CRT 默认独占打开 | 改用 `_fsopen(..., _SH_DENYWR)` |
| 6 | `--demo` 跑完自动退进程 | 默认行为太意外 | 默认交还交互，`--demo-exit` 才退出 |
| 7 | 放大 UI 时演示字幕跑出画面 | 混用了像素与 HUD 单位 | 统一到 HUD 单位 |

### 打磨轮 2/3/4

* **HUD 清晰度**：全部改为整数像素对齐绘制 + 整数倍字形块；罗盘去掉斜线笔画与重叠半透明形状
  （只剩像素对齐矩形 + 位图文字两种原语），四个方位字母改为 2 倍字号 + 深色底片。
* **面板瘦身**：652×357 → **472×312**（占屏 25% → 16%），操作指南移到屏幕左下角。
* **光斑图居中**：按圆柱**圆形质心**卷绕 u（North 的光斑质心 u = 0.003，正好骑在接缝上；
  旧实现把它切成两半：峰值偏移 177 px、水平展布 72.8 texel）→ 现在四个镜位质心偏移 ≤ 2.5 px、
  展布 ≤ 12 texel。
* **螺栓标记**：见 Q7（两套基准 + 背面近景 + 半埋八面体 + 最近节点采样，共四处）。
* **机位跟随选中镜**：`F2/F3/F4` 不再固定看内置北镜；`F2` 现在飞到**当前选中镜**。
* **德令哈日轨**：太阳轨迹由“三纬度 + 赤纬 0”的示意改为德令哈的真实日轨（见 Q4 后的功能表）。

完整证据（命令、数值、修前修后对比）在 `docs/perf_log.md`。

## 目录结构

```
├─ src/, shaders/, data/, configs/, data_proxy/   已验证的研究内核
├─ viz/src/        viewer：平台层、交换链、图形 pass、HUD、计时器、相机
├─ viz/shaders/    flux_lite.slang、scene.slang、post.slang、hud.slang
├─ tools/          bmpstat.py、gen_font.py、perf_sweep.ps1、plot_perf.py、
│                  make_gif.py、make_stills.ps1、make_bloom_fig.py、bloom_probe.py、
│                  inset_probe.py、hud_probe.py、bolt_probe.py、
│                  acceptance_window.ps1、acceptance_interaction.ps1、acceptance_keys.ps1
├─ docs/perf_log.md、docs/perf_viewer.csv、docs/figs/
└─ PLAN.md、PROMPT.md、NOTES_provenance.md
```

`tools/bmpstat.py` 值得一提：它打印区域统计和整帧的 ASCII 亮度预览——本项目每一次
画面改动都是靠它在终端里检查的。`tools/bloom_probe.py`、`tools/inset_probe.py`、
`tools/hud_probe.py`、`tools/bolt_probe.py` 用“两帧逐像素相减”的同一套思路分别验证
bloom 阈值、光斑图居中、HUD 布局与**螺栓标记可见性**。三个 `acceptance_*.ps1` 从外部
驱动窗口、滑杆与按键，任何一条验证层消息都会让它们失败。

## 验收清单（全部自动，无需人工）

| 检查 | 命令 | 结果 |
|---|---|---|
| 物理 parity | `heliostat_core --parity …` | 相对误差 1.95e-07，S95 位相同 |
| viewer kernel parity | `heliostat_viz --dump-flux` + numpy 比对 | 对基准 NPY 与对 `forward.slang` 都是逐像素 0.0 |
| API 正确性 | `heliostat_viz --validate --demo 27` | **0 error / 0 warning**（stderr 空文件，3138 帧，115.9 FPS） |
| 窗口鲁棒性 | `tools/acceptance_window.ps1 -Validate` | 5 次 resize + 最小化/还原/最大化 + 按键 + WM_CLOSE：PASS，8 次交换链重建，0 报错 |
| **控件交互** | `tools/acceptance_interaction.ps1 -Validate` | **PASS 8/8**：收敛滑杆 t 1→0 且 **S95 45.78 → 222.19 m²**；太阳方位 4.31°→42.00°；束靶 0→+36°；`2` 键 spp 256→512；验证层 0 报错；退出码 0 |
| **镜位按键联动** | `tools/acceptance_keys.ps1` | **PASS 13/13**：按 `4/5/6/7` 切到 N/E/S/W **并**把镜头飞到该镜（cam 23.91/−27.91/186.02/75.24°）；`8` 关/开镜场光追；`M` 热力图改 FIXED；`F7` view→normals；`F8` beams→strong；`U` UI 比例→1.5；`[` 把 t 降到 0.49、`]` 又升回 1.00；且每个开关都在日志里留了确认行 |
| **面板大小 / 内容 / UI 比例** | `tools/panel_probe.py` + `--ui-layout` | 面板占屏 **17.9%**（1280×720）、**18.0%**（1920×1080，自动 1.5×）、18.2%（2560×1440，自动 2×）；盒内内容像素占 26–42%（回归前是 **0%**，面板空白）；强制 3× 时自动落到 **1.84×** 以免裁剪 |
| **`8` 关闭光追** | 按 `8` 后读退出报告 | `deform 0.000 | flux 0.000 | ... | total 0.240 ms`（默认 ~0.34 ms 且 flux 非零），S95/峰值读数归零 |
| 光斑图居中 | `tools/inset_probe.py` | **PASS**：4 镜位质心偏移 ≤ 2.5 px；反例（`--no-inset-centre`）North 峰值偏移 177 px、展布 72.8 texel |
| HUD 布局 | `tools/hud_probe.py`（开/关 UI 差分） | 面板 472×312 左上、操作指南左下、罗盘右上、光斑图右下；画面中央 HUD 占用 0.8% |
| 螺栓标记可见性 | `tools/bolt_probe.py` | **PASS**：N/E/S/W 可见 **35 / 35 / 34 / 35** 个标记 |
| 德令哈夏至日轨迹 | `--sunpath 1 --sun-hour H` | 正午 az **0.00°**、el **76.07°**、真太阳时 12:00、北京时间 13:31、昼长 **14.578 h**；日出/日落端 el 2.78° |
| 帧率上限 | `--fps 60 / 120 / 0` | 实测墙钟 59.9 / 119.8 / 126.8 FPS |
| 帧循环停顿 | 运行结束的计数器打印 | 见 `docs/perf_log.md` |

> 两个和测试有关的小坑，记在这里省得再踩：① viewer 是 **DPI aware** 的进程，测试脚本
> （PowerShell）不是——在 125% 缩放的显示器上，未声明 DPI 感知的测试进程会把客户区看成
> 1024×576 而不是 1280×720，所有合成点击都会偏 25%；② 控件的命中测试读的是**真实指针
> 位置**，所以自动化测试必须 `SetCursorPos` 真的移动光标，只 post 坐标会被随后到来的真实
> `WM_MOUSEMOVE` 覆盖。

## 术语与口径

* **spp**：每个接收器像素追踪的表面网格点数（32×32 网格 → 最大 1024）。`spp` 与上游
  `totalSpp` 是同一个量。
* **GPU ms / wall ms**：前者是设备时间（timestamp query），后者包含提交、present 与
  系统合成器；汇报性能时两者必须同时给。
* **S95 面积**：阈值以上像素数 × 单像素面积（0.1601 m²）。
* **ray**：一次“接收器像素 → 镜面点 → 反射 → 太阳”的完整追踪。
* **真太阳时 / 北京时间**：德令哈在 120°E 标准子午线以西 22.63°，所以北京时间 = 真太阳时 + 1h31m。
* **方位角**：本场景 `az = atan2(x, z)`，**0° = 正南**，+90° = 东。
