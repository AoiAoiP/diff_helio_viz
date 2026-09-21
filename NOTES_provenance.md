# NOTES_provenance.md — 来源、取舍与再同步须知

本包从研究仓库 `bezier_opt/`（2026-08 状态）中抽取，目标是**只保留可视化前端需要的东西**，
同时保证数值与上游可比。所有提取都在 2026-08 由一次会话完成并当场验证。

---

## 1. 一句话结论

| 维度 | 上游研究仓库 | 本包 |
|---|---|---|
| C++ 源码 | `pipeline.cpp` 2408 行 / 118 KB + `vulkan_app` + `config` + `input` + `main` | **1741 行**（含全部头文件），拆成 vk / data / engine / config / main |
| 配置键 | 40+（Adam、正则套件、B-spline、anchor/bend/soft-wall、MSE、POD…） | **21 个**，只留前向/可视化读取的 |
| 第三方依赖 | fmt 2.94 MB + glm 20.56 MB（+ 运行时 slang.dll） | **0**（`std::format`/裸数组；Slang 只当编译器，无运行时 DLL） |
| 数据 | `data_proxy` + 几十个实验目录 | 只留 TPS 影响函数 + 20-bin 重力 + 1 个场景 + 2 个螺栓预设 + 1 个基准 NPY |
| 着色器 | 25 个入口 / 11 个 `.slang` | **6 个 `.slang` / 8 个入口**（+2 个反传参考件不编译） |
| 数值一致性 | — | flux sum 相对误差 **2e-7**，S95 **位相同** |

---

## 2. 逐文件来源

### 2.1 原样复制（未改一个字节）

| 包内路径 | 上游来源 | 说明 |
|---|---|---|
| `shaders/common.slang` | `shaders/common.slang` | UBO 结构、坐标变换、Box-Muller、接收器采样 |
| `shaders/sunshape.slang` | `shaders/sunshape.slang` | Buie / Pillbox / Gaussian |
| `shaders/bolt_common.slang` | `shaders/bolt_common.slang` | TPS 影响函数求值 + 20-bin 重力插值 + `boltSurfaceAtGrid` |
| `shaders/bolt_forward.slang` | `shaders/bolt_forward.slang` | `computeBoltSurface`（每帧面板形变） |
| `shaders/forward.slang` | `shaders/forward.slang` | 光路核心（玻璃双折射 / A1 预裁剪 / 能量归一化）+ clear/finalize |
| `shaders/s95_gpu.slang` | `shaders/s95_gpu.slang` | GPU 协作二分查 S95 阈值 |
| `shaders/reference/bolt_backward.slang` | `shaders/bolt_backward.slang` | **逆向渲染（bwd_diff）参考件，本包不编译** |
| `shaders/reference/bolt_optimizer.slang` | `shaders/bolt_optimizer.slang` | 同上（Adam 参数更新） |
| `data/ellipse_north.txt` | `data/ellipse_north.txt` | North 300 m 场景 |
| `data/sundir/sundir_36.txt` | `data/36_sundir_fast.txt` | 36 方向快速集 |
| `data/sundir/sundir_3val.txt` | `data/3_validation_sundirs.txt` | 3 个验证方向 |
| `data_proxy/*` | `data_proxy/*` | 3 个影响函数平面 + 20 个 3-plane 重力 bin + `gravity_angles.json` + `gravity_y.bin` |
| `data/history/North_300m_history.csv` | `results_fw_tanh_a1e3/North_300m_history.csv` | 200 迭代 loss/S95 曲线（HUD 画收敛曲线用） |
| `data/baseline/North_300m_sun0_flux.npy` | `demo_scratch/out/North_300m_sun0_flux.npy` | 上游 `--dump-flux` 产物，一致性基准 |

### 2.2 改写/精简

| 包内路径 | 上游来源 | 处理 |
|---|---|---|
| `src/vk.{h,cpp}` | `src/vulkan_app.{h,cpp}` | 保留 instance/device/queue/buffer/texture/pipeline 逻辑；去掉与优化器耦合的接口；**新增**时间戳计时（`submitOneShot(..., outGpuMs)`）；错误打印改用 `std::printf`（去 fmt） |
| `src/config.{h,cpp}` | `src/config.{h,cpp}` | 键从 40+ 精简到 21 个；Buie 常量推导逻辑原样保留；去 fmt |
| `src/data.{h,cpp}` | `src/input.cpp` + `pipeline.cpp` 的加载段 | 太阳方向/椭圆解析取自 `input.cpp:11-47`；重力 3-plane 检测与影响函数尺寸硬校验取自 `pipeline.cpp:498-603`；`packGravityParams`/`computeCosTheta`/`computeAimPoint`/`computePixelArea` 逐行对齐；**新增** NPY 读写、CPU S95 参照实现、螺栓文件三格式兼容解析 |
| `src/engine.{h,cpp}` | `src/pipeline.cpp` 的 bolt 前向路径 | 抽取 dispatch 链、UBO 逐位打包、活跃像素列表、S95 状态回读；**去掉**反传/Adam/正则/B-spline/POD/MSE 全部内容；单太阳（上游的 kSunBatchSize=6 批次维度不需要） |
| `src/main.cpp` | `src/main.cpp` 的 `--dump-flux` 分支 | 改为独立 CLI：`--config/--bolts/--sun/--dump-flux/--parity/--bench/--bench-readback/--print-cells`；**新增** parity 比较与墙钟计时 |
| `CMakeLists.txt` | `CMakeLists.txt` + `src/CMakeLists.txt` | 去掉 fmt/glm 的 FetchContent 与 Slang 链接；着色器只编 8 个入口；留好 viewer target 的注释块 |
| `data/bolts/North_300m_lsq_init.txt` | `data/init_lsq/North_300m_bolt_init.txt` | 两列 `idx value` → 单列 + 来源注释头 |
| `data/bolts/North_300m_optimized.txt` | `results_fw_tanh_a1e3/North_300m_STROKE_bolts.txt` | 单列 stroke + 来源与运行参数注释头 |

### 2.3 明确丢弃（以及为什么）

| 丢弃内容 | 理由 |
|---|---|
| `pipeline.cpp` 的 Adam / 反传 / 正则套件（anchor/bend/soft-stroke/L1）/ B-spline / POD-ROM / MSE loss | 可视化前端不需要；逆向渲染的"优化"环节留在上游，viewer 只消费其结果（螺栓预设、history.csv） |
| `vulkan_app` 的每调用 staging 分配 + `vkQueueWaitIdle` 模式 | 上游 83 ms/帧的根因；本包只在启动期上传资产时使用一次性 submit，帧循环路径（`engine::render`）已是一次 submit |
| `forward.slang` 里的逐光线诊断原子（`InterlockedAdd(diagBuf[5],1u)`） | 实测占 5.2×；本包**没有改上游文件**，而是在对照中体现（见 `BASELINE.md`）。若要修上游，改成波内归约/组级计数即可 |
| fmt / glm 依赖 | glm 在上游 `src/` 里 **0 次使用**；fmt 仅 4 处（错误串 + 2 条打印）→ 用 `std::format`/`std::printf` 替代 |
| 上游 `shaders/backward.slang`、`loss.slang`、`optimizer.slang` | Bezier 模式与旧 MSE 路径，与本包无关 |
| 数十个 `results_*` 实验目录、`configs/archive/`、Python 分析脚本 | 研究过程产物，不属于可视化工程 |
| `slang.dll` / `slang-compiler.dll` 运行时依赖 | 本包 C++ 从不调用 Slang API，只用 `slangc` 编译 → 可执行文件无额外 DLL |

---

## 3. 有意保留的"怪癖"（不要"顺手修正"）

这些看起来像 bug，但修正后会破坏与上游的数值一致性：

1. **顶点采样是 cell-edged 而 proxy 数据是 pixel-centered**：`forward.slang` 用 `hx = -W/2 + i·W/(GS-1)`，而 `data_proxy/*.bin` 是按 `u=(i+0.5)/GS` 生成的。二者差半步，但这是上游生产口径，改动会让 parity 失效。
2. **`Σφ_b ≡ 1` 使"全体螺栓加常数"等价于刚性平移**：所以 `h_pipe` 与 `h_stroke` 两列数值不同但光学结果相同——螺栓文件解析取最后一列（stroke）是安全的。
3. **重力 bin 的 du/dv 是物理斜率**，shader 里要乘 W/L 转成 `d/du`、`d/dv`。
4. **上游 `--dump-flux` 会把光斑在方位角方向循环滚动**（居中）。所以基准 NPY 的**像素位置**与本包不可逐像素直接比，只能比 roll 不变量（sum / peak / S95）。
5. **`clearFlux` / `finalizeFlux` 的 tile 数是编译期常量**（grid 32 → 4）。本包固定 1024 spp 所以没问题；viewer 一旦要改 spp，必须新写 lite 版三件套。
6. **Slang 生成的 SPIR-V 入口名统一是 `main`**，不是 Slang 函数名（上游所有管线都传 `"main"` 就是这个原因）。

---

## 4. 验证证据

- `data/baseline/BASELINE.md` — 数值对照表（flux sum 相对误差 1.95e-07；S95 位相同）+ 性能对照表（上游 83 ms ↔ 本包 1.09 ms）。
- 复现命令：
  ```powershell
  & "C:\Program Files\CMake\bin\cmake.exe" --build build --config Release
  cd build\Release
  heliostat_core.exe --config ..\..\configs\probe_single_sun.json --sun 1 `
                     --parity ..\..\data\baseline\North_300m_sun0_flux.npy --bench 200
  ```

---

## 5. 如果再与上游同步

1. 上游改动过 `common.slang` / `bolt_common.slang` / `forward.slang` / `s95_gpu.slang` → 直接覆盖本包同名文件，然后**必须重跑 parity**（UBO 布局或绑定号若有变动，同步改 `engine.cpp`）。
2. 上游重新生成 `data_proxy`（例如换了布局、margin、材料）→ 同步 `.bin` 并核对字节数：影响函数每个 **143360 B**、重力每个 bin **12288 B**（3-plane）；旧 4096 B 单平面会被识别为 legacy 并打印警告。
3. 上游若新增一个前向相关的着色器入口 → 加到 `CMakeLists.txt` 的 `SHADER_ENTRIES`，并在 `engine.cpp::createPipelines` 里建管线（入口名一律 `"main"`）。
4. 上游若修了逐光线诊断原子 → 本包无需改动，但 `BASELINE.md` 的性能对照数字需要重测并更新。
