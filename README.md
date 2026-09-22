# Heliostat Studio

针对定日镜光斑可微优化管线的实时可视化程序。Win32 窗口 + Vulkan 1.4，手写实现，
不依赖 GLFW / SDL / ImGui / stb / glm / 任何引擎：平台层、交换链、位图字体、HUD 控件、
以及跑在帧循环里的光追 kernel 都在本仓库内。

![演示](docs/figs/demo.gif)

程序把 12.84 × 9.45 m 定日镜的光斑实时渲染到 157 × 50 像素的接收器圆柱面上
（对数热力图 + 5 级 bloom + ACES tonemap），可拖动面板形变从理想平板一直调到优化器输出，
并用 timestamp query 显示逐 pass GPU 毫秒，同时对光追 kernel 提供三个可切换的 A/B 开关。

同一份 4.04 M 光线负载：

| 口径 | 数值 |
|---|---|
| 光追段（1024 spp） | 0.357 ms（11.3 Gray/s） |
| 整帧 GPU（1024 spp，全部功能开启） | 0.57–0.63 ms |
| 与上游基准 flux 图的偏差 | CLI 相对误差 1.951e-07，viewer 逐像素 max abs diff 0.0 |

主要画面元素：接收器光斑与光束、右下角光斑图（带 S95 等值线，可开关）、NSWE 罗盘与地面
方位标、四台定日镜的镜场（只有选中镜执行光追）、德令哈夏至日真实日轨。

参数含义、按键表与每个 pass 的口径都在下面列出；`--help` 会打印同一份键位。常见问题的解释在程序内面板与本文的性能章节里。

---

## 构建

依赖：**Vulkan SDK 1.4+**（头文件、loader、随附 `slangc`）与 Visual Studio 2022。构建期不需要网络。

```powershell
# PATH 上的 msys2 cmake 在受限 shell 里可能失败，建议直接用原生 cmake
& "C:\Program Files\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
& "C:\Program Files\CMake\bin\cmake.exe" --build build --config Release
```

产物：

```
build\Release\heliostat_core.exe   无头前向引擎（与基准对齐验证过）
build\Release\heliostat_viz.exe    实时 viewer
build\Release\shaders\*.spv        slangc 编译产物，POST_BUILD 阶段拷到 exe 旁
```

## 运行

```powershell
cd build\Release

# 直接运行；资产与 SPIR-V 会自动解析，任何工作目录都可
.\heliostat_viz.exe

# 脚本化 27 秒演示（相机、滑杆、镜场、日轨按固定时间轴走一遍）
.\heliostat_viz.exe --demo 27 --validate
# 录 README 顶部的 GIF：演示录成 BMP 序列，再编码为 362 帧 / 640×360 / 14 fps（4.9 MB）
.\heliostat_viz.exe --demo 27 --demo-exit --hud-scale 1.4 --record ..\..\out\gif --record-stride 4
python ..\..\tools\make_gif.py --frames ..\..\out\gif --out ..\..\docs\figs\demo.gif --fps 14 --width 640

# 德令哈夏至日正午（时角钉在 0，动画暂停）
.\heliostat_viz.exe --sunpath 1 --sun-hour 0

# 逐帧状态行与控件几何（外部脚本据此断言）
.\heliostat_viz.exe --state-log --ui-layout --log ..\..\out\logs\run.log

# 与基准 flux 图对比
.\heliostat_viz.exe --frames 40 --spp 1024 --sun 1 --t 0 --dump-flux ..\..\out\flux.npy

# 性能扫描 → CSV + 作图
powershell -File ..\..\tools\perf_sweep.ps1
python ..\..\tools\plot_perf.py

.\heliostat_viz.exe --help
```

常用命令行选项：`--width/--height`、`--spp`、`--t`（收敛）、`--az/--el`（太阳）、`--aim`
（束靶）、`--sunpath/--sun-hour`（日轨）、`--mirror`（镜位）、`--deform`、`--preset`（机位）、
`--fps`、`--present`、`--hud-scale`、`--no-ui`、`--validate`、`--bench`。

---

## 目录结构与架构

```
├─ CMakeLists.txt              两个目标：heliostat_core（无头引擎）、heliostat_viz（viewer）
├─ src/                        研究内核：Vulkan 封装、前向引擎、配置与数据加载
├─ shaders/                    研究内核的 Slang 着色器（含 parity 参照组 forward.slang）
├─ data/                       螺栓预设、太阳方向、基准 flux 图
├─ configs/                    场景配置（接收器、定日镜、光学参数）
├─ data_proxy/                 上游派生数据（影响函数峰值、椭圆拟合）
├─ viz/src/                    viewer：平台层、交换链、图形 pass、HUD、计时器、相机
├─ viz/shaders/                viewer 着色器：flux_lite / scene / post / hud
├─ tools/                      测量与验收脚本（Python + PowerShell）
└─ docs/figs/                   演示 GIF 与性能/对比图（本目录下的其它笔记不随仓库发布）
```

每帧一次 `vkQueueSubmit`，2 帧飞行，逐 image fence，动态渲染（无 render pass），reversed-Z 无限远投影：

```
one vkQueueSubmit per frame, 2 frames in flight, no per-frame allocation
│
├─ compute  clearFluxLite         (10,4)         运行期 tile 数
├─ compute  computeBoltSurface    (1,1,1)        复用上游 SPIR-V（TPS + 20-bin 重力）
├─ compute  fluxLite              (tiles, 3950)  spp 16..1024，cull/atomics A/B
├─ compute  finalizeFluxLite      (10,4)
├─ compute  computeS95FindLevel   256×1          复用上游（协作二分）
│     + 每 15 帧一次采样回读 → S95 面积、flux 总量、诊断计数器
├─ graphics scene       天空 / 地面 / 塔 / 接收器（157×50 自发光四边形）
│                       / 镜面（顶点拉取 yGrid+nGrid）/ 35 个螺栓标记 / 光束
├─ graphics bloom ×9    亮通道 + 4× 13-tap 降采样 + 4× tent 升采样（5 级，半分辨率）
├─ graphics composite   ACES + 曝光 + 暗角 → sRGB 交换链
└─ graphics hud         5×7 位图字体 + 面板 + 滑杆，单个顶点缓冲，无 ImGui
```

帧循环中强制遵守的约束：

- 每帧一次 submit，2 帧飞行，逐 image fence；
- 帧循环路径上没有 `vkQueueWaitIdle` / `vkDeviceWaitIdle`（唯一的 device idle 在退出时）；
- 每帧零分配：buffer、descriptor、query pool、pipeline 全部预建，逐帧数据写入持久映射的 UBO 切片；
- 所有回读都是延迟的：GPU 时间戳在 2 帧后收集，flux 图每 15 帧一次，截图延后 2 帧。

上游复用关系：`shaders/common.slang`、`sunshape.slang`、`bolt_common.slang`、`forward.slang`
（parity 参照组）、`s95_gpu.slang` 原样使用；`src/vk.cpp` 的 compute 封装、`src/engine.cpp`
的 dispatch 链、push constant、绑定号与 UBO 逐位布局保持不变，viewer 只在其上增加图形层。

---

## viz 程序内的参数与单位

面板与命令行里的参数含义如下。角度以场景世界轴为准：+X 为东，+Z 为镜面朝向
（北半球镜场即南），+Y 向上。

**太阳相关**

| 参数 | 单位 | 范围 / 默认 | 含义 |
|---|---|---|---|
| `sun azimuth` | deg | −60 .. +60 | 太阳方位角，**0° = 正南**，+90° = 东 |
| `sun elevation` | deg | 5 .. 85 | 太阳高度角，0° = 地平线 |
| 纬度 | deg（北纬） | 37.37（德令哈，固定） | 日轨计算用 |
| 经度 | deg（东经） | 97.37（德令哈，固定） | 只影响时间显示：北京时间 = 真太阳时 + 1h31m |
| `dec` 赤纬 | deg | +23.44 / 0 / −23.44 | 夏至 / 春·秋分 / 冬至 |
| `H` 时角 | deg | 夏至 ±109.34（日出→日落） | 0° = 真太阳时正午，下午为正；动画在日出到日落之间循环 |
| `LST` | h:mm | 04:59 .. 19:01（夏至） | 真太阳时 |
| `BJT` | h:mm | 06:29 .. 20:32（夏至） | 北京时间 |
| `noon el` / `day` | deg / h | 76.07 / 14.578（夏至） | 正午高度角 = 90° − \|纬度 − 赤纬\|，以及昼长 |
| `DNI` | W/m² | 1000（配置固定） | 法向直射辐照度，光追的能量基准 |

**接收器与光斑**

| 参数 | 单位 | 范围 / 默认 | 含义 |
|---|---|---|---|
| `beam target` 束靶 | deg | −60 .. +60（≈ ±157 px） | 沿接收器圆周移动瞄准点。定日镜随之重新指向，光斑始终锁在瞄准点上 |
| S95 面积 | m² | 46.26（优化）/ 226.67（平板） | 阈值以上像素数 × 单像素面积 |
| S95 level | W/px | 随面型变化 | GPU 协作二分求出的阈值 |
| flux 峰值 | W/px | 665（平板）.. 4808（优化） | 接收器最亮像素 |
| 单像素面积 | m² | 0.1601（固定） | 接收器 157×50 像素中的一个 |
| `heat <=` ceiling | W/px | 自动（峰值 ×1.25）/ 固定 | 热力图对数映射上限 |
| 接收器几何 | m | R = 10，H = 20 | 圆柱半径与高度 |

**面型与形变**

| 参数 | 单位 | 范围 / 默认 | 含义 |
|---|---|---|---|
| `convergence t` | 无量纲 | 0 .. 1（默认 1） | 螺栓行程插值系数：0 = 理想平板，1 = 200 次迭代优化结果 |
| 螺栓行程 | mm | 0 .. 36.2 | 35 个作动器的实际行程 |
| `deform` 放大 | × | 1 / 50 / 200 | **仅显示用**，不参与光追 |
| 镜面网格 | — | 32 × 32 | compute 写出的 `yGrid` / `nGrid` 分辨率 |
| 面板尺寸 | m | 12.84 × 9.45 | 镜面宽 × 长 |

**渲染与后期**

| 参数 | 单位 | 范围 / 默认 | 含义 |
|---|---|---|---|
| `exposure` | × | 0.25 .. 3.0 | 合成前的线性曝光 |
| `bloom` 强度 | 无量纲 | 0 .. 2.0（默认 0.55） | 光晕叠加量 |
| `bloom thr` 阈值 | × | 0.5 .. 8.0（默认 2.5） | 亮通道门槛，相对白点 1.0；低于 2 时天空也会发光 |
| `bloomClamp` | × | 6.0（固定） | 亮通道的萤火虫上限 |
| `view` | — | shaded / normals / slope error / height | 镜面着色模式；斜率误差满量程 30 mrad，高度 ±60 mm |
| `beams` | — | off / normal / strong 2× | 光束叠加强度 |
| `spp` | 样点/像素 | 16 .. 1024 | 每像素追踪的镜面网格点数；光线数 = spp × 3950 |
| `ui x` | × | AUTO / 1 / 1.5 / 2 / 2.5 / 3 | UI 与位图字体比例（整数倍像素块，不模糊） |
| `cap` | fps | 60（默认）/ 120 / off | 软件帧率上限 |

**场景常量**

| 项 | 值 |
|---|---|
| 塔高 / 镜塔水平距离 | 接收器中心 y ≈ 180 m，镜场半径 300 m |
| 地面高度 | y = −7 m |
| 光学参数 | CSR 0.01、斜率误差 1.0 mrad、反射率 0.88、Buie κ = −7.1748 γ = −1.6971 |

---

## 操作说明

**鼠标**：左键拖动转轨道、直接拖面板滑杆 · 右键/中键平移 · 滚轮缩放 ·
WASD/QE 自由飞行（Shift 加速）

每个按键都会在面板显示一行提示（约 3.5 秒）并写入日志。

| 按键 | 作用 |
|---|---|
| `1` | A/B：A1 逐光线预裁剪 开/关 |
| `2` | spp 档位循环（16/32/64/128/256/512/1024） |
| `3` | A/B：逐光线全局原子 开/关 |
| `4` `5` `6` `7` | 选中 N/E/S/W 镜执行光追，并把镜头飞到该镜面 |
| `8` | 镜场光追 开/关（关闭时 compute 链整条不执行） |
| `9` | 德令哈日轨循环：夏至 → 春/秋分 → 冬至 |
| `0` | 关闭日轨（太阳改由滑杆控制） |
| `[` `]` | 微调收敛 `t`（单击 ±0.02，按住 0.7/s） |
| `←` `→` | 日轨时角微调（Shift 加速） |
| `L` | 日轨动画 暂停/继续 |
| `P` | 暂停（冻结物理） |
| `H` | 显示/隐藏面板 |
| `M` | 热力图量程 自动/固定 |
| `K` | 束靶回中 |
| `F` | 右下角光斑图 开/关 |
| `C` | 罗盘 + 地面方位标 开/关 |
| `U` | UI 比例循环（AUTO/1/1.5/2/2.5/3） |
| `F1` | 机位：全场总览 |
| `F2` | 机位：飞到当前选中的镜面 |
| `F3` | 机位：接收器光斑特写 |
| `F4` | 机位：光束侧视 |
| `F5` | 面型预设：收敛 / LSQ 椭圆拟合 / 零螺栓 |
| `F6` | 形变放大 1× / 50× / 200×（仅显示） |
| `F7` | 镜面视图：shaded / normals / slope error / height |
| `F8` | 光束：off / normal / strong 2× |
| `F9` | 曝光 ×1.25（`Ctrl+9` 回退） |
| `F10` | present 模式循环 |
| `F11` | 帧率上限 60 / 120 / 不限 |
| `Space` | 截图：当前工作目录下 `shot_001.bmp`、`shot_002.bmp`…（同时写出同名 `.txt` ASCII 预览） |
| `ESC` | 退出 |

---

## 性能

口径：`GPU ms` 是 `vkCmdWriteTimestamp` 的差值；`wall ms` 是 `steady_clock` 包住整个迭代
（含 present 与系统合成器）。下表的 FPS 均为 `--fps 0`（关掉默认 60 上限）口径，
不设上限的数字用 `VK_PRESENT_MODE_IMMEDIATE_KHR`。默认 60 上限下 wall 恒为 16.7 ms。
测试机：RTX 4060 Laptop，1280×720，关 vsync。

| spp | 光线/帧 | flux 段（GPU） | GPU 总计 | wall（IMMEDIATE） | 吞吐 |
|---|---|---|---|---|---|
| 16 | 63 k | 0.082 ms | 0.204 ms | 0.88 ms（1137 FPS） | 0.8 Gray/s |
| 64 | 253 k | 0.086 ms | 0.208 ms | 0.91 ms（1097 FPS） | 2.9 Gray/s |
| 256 | 1.01 M | 0.121 ms | 0.242 ms | 1.20 ms（835 FPS） | 8.4 Gray/s |
| 1024 | 4.04 M | 0.357 ms | 0.57–0.63 ms | 1.19–1.41 ms（708–843 FPS） | 11.3 Gray/s |

1024 spp 逐 pass（全部功能开启）：deform 0.021 · flux 0.357 · scene 0.054 ·
bloom（9 个 pass）0.059 · composite 0.011 · 内嵌图与 HUD 0.064 ms，合计 **0.566 ms**。

同一份 4.04 M 光线负载下的 A/B 开关（1024 spp、收敛 t=1、`--fps 0 --present immediate`）：

| 配置 | flux 段（GPU） | wall | 相对实时 kernel |
|---|---|---|---|
| 实时 kernel `flux_lite.slang` | 0.357 ms | 1.08 ms | 1.0× |
| 打开逐光线全局原子（`--atomics`） | 48.87 ms | 48.4 ms | **137×** |
| 参照链 `forward.slang`（原子恒开，原样复用） | 62.31 ms | 62.0 ms | **175×** |

（flux 列取三轮最小值，wall 列取三轮中位。A1 预裁剪开关 `--no-cull` 不单列：实测它和上一行在
0.35–0.48 ms 之间互换位置，也就是**没有可测差别**——本场景里它能挡掉的采样点本来就不多。）

口径：只有原子档和参照链会把 GPU 压满几十秒，笔记本的功耗/温度墙会让同一份代码的 `flux` 读数漂
±20%（同一会话里实时 kernel 读到 0.357 / 0.478 / 0.496 ms，参照链读到 62.31 / 62.49 / 63.72 ms）。
最小值最接近"代码本身要多少时间"，因为时钟只会往下掉、不会超过标称频率。比率按同样口径配对；
换成"每轮自己的实时 kernel 当分母"，比率会被这个抖动直接乘进去（原子档 99–140×、
参照链 126–175×）。**不要跨批次拆开引用**：本文档这一批是 137× 与 175×，另一次重跑是 130× 与 169×，
都在这条带里。复现：`python tools/perf_ab.py`。

帧率上限（`--fps` / `F11`，默认 60）：60 → 16.687 ms（59.9 FPS）；120 → 8.349 ms（119.8 FPS）；
不限速 + `MAILBOX` → 126.8 FPS；不限速 + `IMMEDIATE` → 708–843 FPS。

与无头引擎对照：

| 路径 | GPU | 每渲染墙钟 |
|---|---|---|
| `heliostat_core --bench 200`（1024 spp） | 0.50 ms | 0.64 ms |
| `heliostat_core --bench-readback 200`（等价上游每太阳循环） | 0.50 ms | 1.09 ms |
| 上游研究仓库，每个太阳方向 | — | ≈ 83 ms |

原始数据用 `tools/perf_sweep.ps1` 现场重测（默认 400 帧一档，写到 `docs/perf_viewer.csv`），
A/B 开关用 `tools/perf_ab.py`（四用例交替三轮取最小值，写到 `out/perf_ab.csv`）；
曲线与对比图在 `docs/figs/`（`frametime_vs_spp.png`、`ab_switches.png`、`pass_breakdown.png`）。

---

## 数值一致性

| 检查 | 结果 |
|---|---|
| `heliostat_core --parity`（上游基准 flux 图） | sum 相对误差 1.951e-07；peak 664.911 相同；S95 level 75.207809 相同；S95 面积 226.67 m²（0.00%） |
| viewer `flux_lite` vs 基准 flux 图，逐像素 | 按文档的 79 像素方位角滚动对齐后 max abs diff = 0.0 |
| viewer `flux_lite` vs 同进程运行的 `forward.slang` | 逐像素 diff 0.0 |
| S95 GPU 协作二分 vs CPU 参照实现 | 位相同 |

## 自动验收

| 检查 | 命令 | 结果 |
|---|---|---|
| 物理一致性 | `heliostat_core --parity …` | 1.951e-07，S95 位相同 |
| viewer kernel | `heliostat_viz --dump-flux` + numpy 比对 | 逐像素 0.0 |
| 验证层 | `heliostat_viz --validate --demo 27` | 0 error / 0 warning |
| 控件交互 | `powershell -File tools/acceptance_interaction.ps1 -Validate` | PASS 8/8 |
| 按键 | `powershell -File tools/acceptance_keys.ps1` | PASS 21/21（含 `Space` 在**不带任何截图参数**的启动下确实落盘） |
| 窗口 | `powershell -File tools/acceptance_window.ps1 -Validate` | PASS（8 次交换链重建，0 报错） |
| 面板内容与尺寸 | `python tools/panel_probe.py` | 面板占屏 17.9%（1280×720）/ 18.0%（1920×1080），盒内内容占 26–42% |
| 光斑图居中 | `python tools/inset_probe.py` | 4 镜位质心偏移 ≤ 2.5 px |
| 螺栓标记可见性 | `python tools/bolt_probe.py` | 四镜位 35 / 35 / 34 / 35 个 |
| 演示 GIF 重录 | `python tools/gif_check.py --old out/old_demo.gif` | 8 个阶段逐段一致（各段亮度中位差 ≤ 0.22 / 255） |
