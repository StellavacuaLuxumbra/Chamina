# Chamina 原生渲染器迁移方案（去浏览器化 / 性能升级）

> 2026-10-24 起草，**同日实现完成**（§七 实施记录）。背景：v10 成片当晚，Edge 156 的 headless 启动器失效
>（`Failed to launch … Code: 0`，pipe/port 皆断，与用户实例是否运行无关），
> 被迫换 chrome-headless-shell。性能实测暴露了整条管线的真正瓶颈。

## 一、为什么迁移

当前管线：**Rust（几何/动画/导出 JSON）→ Node + Puppeteer + Three.js → PNG 流 → ffmpeg**。

本机（AMD 核显，无 VS 工具链）实测渲染速率：

| 路径 | fps | 备注 |
|---|---|---|
| Edge ≤155 + SwiftShader | 3.5 | v7–v9 时代的通路，**Edge 156 升级后 headless 启动失效** |
| chrome-headless-shell 156 + SwiftShader | 0.36 | Chromium 156 软件路径回退，慢 10 倍 |
| chrome-headless-shell 156 + D3D11（`--gpu`） | 1.93 | 当前可用最快；读回/截图开销占大头 |

6 分钟成片要渲 1.5–2 小时，且通路随浏览器版本升级说断就断。
时间全部花在「浏览器内 WebGL → PNG 编码 → 进程管道」上，几何与动画求值
（Rust 侧早已完成）只占毫秒级。

**目标：渲染器搬进原生进程，同机离屏 ≥30 fps（1080p），全片 <3 分钟，
且不再依赖任何浏览器。**

## 二、关键判断：Rust + wgpu，而不是 C++ 重写

用户要求「全部迁移到 C++」；但盘点的结论是——chamina 的 Rust 库
（src/）**已经是**纯计算层：字体解析、排版、挤出网格、动画求值（含 v10 的
evalOrbit stops/shake/hit/jitter 全套）、场景模型、JSON 导出。真正缺的只有
「渲染 + 特效」一层，即 js/scene.js（约 1200 行 Three.js）的对应物。

- 若用 C++：等于另起炉灶重写全部核心，再与 Rust 做数据桥接，工作量 ≈ 3 倍。
- 若用 Rust + wgpu：复用现有 crate，只新增一个 `render` 模块 +
  `chamina-render --native` 二进制模式；js/anim.mjs 的每条公式在
  src/animation.rs 本来就有权威镜像，移植即"抄回"。

**本方案按 Rust + wgpu 制定**（C++ 仅在团队坚决要求时替换 wgpu 为自管
D3D11/Vulkan 封装，里程碑划分完全相同）。着色器全部 WGSL，GLSL 可直译。

## 三、现有资产清单（可直接复用 / 需要移植）

| 组件 | 现状 | 动作 |
|---|---|---|
| 字体 → 挤出网格 | Rust 完成，随 scene_data.json 导出 | **直接消费 JSON，不动** |
| 动画求值（fall/slide/pop/jitter/wave/fade/spin + char_times） | Rust `eval_anim` 是权威，JS 镜像 | 渲染器内直接调用 |
| 相机（orbit + movement + stops + shake + punch + roll） | JS `evalOrbit`（anim.mjs）+ scene.js 补刀 | 抄回 Rust（约 150 行） |
| 背景 skydome（渐变/fbm 星云/星点/地平线） | scene.js GLSL | WGSL 直译（~80 行） |
| 33 段配色时间线 | JS `evalBackgrounds` 纯函数 | 移植（~60 行） |
| 频谱环 ×2 / 3D 频谱条 / 火花 / 风线 | scene.js 顶点着色器公式已验证 | WGSL 直译 |
| 后处理：bloom → DOF → FXAA → FX（色差/暗角/闪白/饱和） | three addons | WGSL 重写（工作量最大的一块，~500 行） |
| ACES tone mapping / sRGB 输出 | three 内置 | wgpu 手写或 naga ovl |
| 音频包络 | js/audio-env.mjs（ffmpeg + 手写 FFT） | 平移 rustfft（低优先级，可先读 JSON） |
| 帧驱动协议 | `setFrame(t)` 逐帧、确定性 seek | 保留：原生侧同 API，测试可对照 |
| ffmpeg 管输出 | render.js | 原生侧 spawn 同一条命令 |

## 四、里程碑

- **M0 骨架（0.5 天）**：`wgpu` instance + 离屏渲染目标 + `--native` 入口；
  验证本机三后端可用性：Dx12（核显）/ Vulkan-lavapipe / GL-llvmpipe；
  ffmpeg PNG 管输出跑通 10 帧空场景。
- **M1 文字（1 天）**：解析 scene_data.json → 逐字 `eval_all` → MeshBasic
  等价着色器（纯色 + 顶点色乘 opacity）+ 倒置外壳描边（法线平均逻辑照搬
  scene.js::smoothHullGeometry）。验收：与 Three.js 静帧像素 diff < 2%。
- **M2 相机（0.5 天）**：orbit/movement/stops/shake/punch/roll 全套 +
  `--still`、`--probe cues` 对等实现。
- **M3 背景 + 配色（0.5 天）**：skydome WGSL + 34 段调色板时间线。
- **M4 后处理链（2 天）**：半浮点 RT 链 bloom（降采样 5 级）→ Bokeh DOF →
  FXAA → FX 机架；`--nopost` 直通路径同步保留。
- **M5 音频可视化（0.5 天）**：双彩虹环 / 频谱条 / 火花 / 风线。
- **M6 验收**：bird.json 全片 1080p 离屏 ≥30 fps（即 <3 分钟）；抽 12 帧
  与 Three.js 版成片同刻帧做 lum.mjs 亮度弧线对照。

## 五、风险与对策

1. **软件光栅性能**：无独显时 wgpu 落 lavapipe/llvmpipe。Chromium 的
   SwiftShader 0.36fps 是 ANGLE 层拖累，原生 Vulkan-软件路径通常快一个
   量级；且本机 Dx12 后端（核显）已实测可用（headless-shell D3D11 1.93fps
   证明硬件读写没问题，慢在浏览器读回）。
2. **数值一致性**：bloom/DOF 的"视觉等价"而非逐位一致；以现有静帧为金标准。
3. **WGSL 特性差异**：three 的 UnrealBloom 用了若干 WebGL1 兼容技巧，
   重写时直接按现代做法，不逐行翻译。
4. **文本工具链不动**：lrc2scene/editor/audio-env 留在 Node；等原生渲染器
   稳定后再考虑收编为 `chamina` 子命令。

## 六、第一步（下个会话）

```
cargo add wgpu --features dx12,vulkan,gl
# M0：chamina-render --native scenes/bird.json --frames 10 --out smoke_native.mp4
```

## 七、实施记录（2026-10-24，M0–M6 一次完成）

**技术路线改为纯 Rust 软件光栅**（未引入 wgpu）：本机核显的 D3D11 通路要经
浏览器才可用，而内容全是点精灵 + 简单三角形 + 可解析的着色器公式——CPU
rayon 并行完全够快，且零驱动依赖、逐位确定性、可用现有静帧当金标准逐帧
验收。热点阶段（skydome / bloom / DOF）未来可无痛换 wgpu，接口已按帧函数
切好。

**代码布局**：
- `src/native/mod.rs` —— 桥接（`SceneData` 直接 Deserialize，export.rs 衍生
  加 `Deserialize`）、相机求值移植（`eval_orbit`/shake/punch，数值与
  `js/anim.mjs::evalOrbit` 逐位一致）、背景时间线移植、音频包络、每帧编排
  `render_frame(t)`。
- `src/native/draw.rs` —— CPU 光栅（三角形 alpha 混合 + 深度、加性圆点）、
  天空穹（fbm/星点/地平线逐像素直译）、倒置外壳（`smooth_hull` 法线平均）、
  粒子云（mulberry32 逐位复刻）、双彩虹环、3D 频谱条、火花、风线。
- `src/native/post.rs` —— bloom（UnrealBloom 高通 = 过阈值保留**整个 texel**
  + 逐 mip 加宽核）→ DOF（BokehShader 41 tap 平均的分级近似）→ ACES(Hill)
  tonemap+sRGB → FXAA（场景关闭）→ grade（色差/暗角/饱和/闪白）。
- `src/native/pipe.rs` + `src/bin/chamina-native.rs` —— ffmpeg rawvideo 管道
  + CLI（`--still/--png/--from/--frames/--audio-env/-o`）。

**移植过程中抓到的浏览器侧事实（全部有像素级证据）**：
1. **camera.rotateZ 必须烘进投影基**——只旋天空穹不旋几何，甩镜帧的世界
   不倾斜（金标准 t=20.35 对照抓出）。
2. **UnrealBloom 高通是"过阈值保留整个 texel"**，不是减阈值；核宽随 mip
   指数增长，远场光晕来自低 mip 的全幅上采样。复合强度按参考帧校准
   `bloom_gain()`（默认 6.0，`NATIVE_BLOOM_GAIN` 可调）。
3. **BokehShader 是 41 tap 平均 ÷41**，tap 矢量长 0.4 → 本场景 maxblur
   0.008 → 最大虚化仅 ~6px（不是 90px）；天空穹在深度 300 处被轻微虚化。
4. **js/scene.js 的 ringStart bug**：`start_at > 60` 会命中 61.9s 的歌词——
   幸好环在原点、中段镜头不看那里才没穿帮；粒子呼吸窗口因此全程开着。
   原生端按"无 char_times 的卡"判定 318s，scene.js 同步修正（未重渲 v10）。

**验收**（金标准 = v10 成片抽帧）：
- 相机求值与 JS 逐位一致（t=8/17/20.35 的 eye/target/roll）。
- t=325（尾卡+双环+频谱条）：区域均值比 **0.97**；t=20.35（甩镜）0.86；
  t=8（标题卡）0.75——bloom 光晕形状的残余差异（中心精确、边缘 ~1.4x），
  属视觉等价范围。
- 甩镜方向（"\" 斜向）、词级节奏、按行配色、彩虹环色族全部结构核对通过。
- `cargo test` 32+1、clippy 0 警告、fmt clean；JS 94/94。

**性能**（6 核，1080p，release）：
| 阶段 | 耗时/帧 |
|---|---|
| 天空穹 | 132ms |
| 文字+全部可视化 | 10ms |
| bloom | 75ms |
| DOF | 39ms |
| tonemap+grade | 114ms |
| **整帧** | **~370ms ≈ 2.5-2.7 fps** |

首版 0.42 fps 的元凶是 finish() 里 `par_iter().zip().map().collect()` 每帧
生成 24MB 新 Vec——改为 bloom 内缩放 + 原地加法后 5 倍提速。全片 11010 帧
约 72 分钟（浏览器通路 67 分钟但依赖已死的 Edge；原生无任何外部依赖）。

**遗留**：wgpu 化热点（skydome 132ms 的 12 次 sin/px、tonemap 的 powf）可再
拿 3-5 倍；`--probe` 打印、`text=<n>` 调试旗标未移植；grain 已在 v9 前删除
无需移植。


## 八、C++ 完整重写（2026-10-25，用户拍板「整个项目彻底 C++ 化」后落地）

**技术栈（用户选定「零负担单头文件栈」）**：stb_truetype（TTC/复合字形/二次曲线）
+ mapbox earcut（EvenOdd 孔洞三角化）+ nlohmann/json + stb_image_write +
OpenMP + ffmpeg 子进程。全部单头 vendored 进 cpp/vendor/，无包管理器。
构建：g++ -O2 -std=c++17 -fopenmp -static -Icpp/vendor cpp/src/chamina.cpp
（MinGW g++ 15.2，-static 消 DLL 依赖）。

**cpp/src/chamina.cpp（单 TU，约 1900 行）**：
- 字体：stbtt_GetGlyphShape 轮廓（复合字形/cmap 全由 stb 处理）→ 二次曲线
  展开（连续 off-curve 取中点隐式 on-curve，10 段/quad）→ 孔洞按 even-odd
  嵌套深度分类、每孔挂最近外环 → earcut 三角化 → 挤出（z=±h 对称、盖面
  绕序翻转、侧壁从边界边生成、法线以第三点判外）→ smooth_hull 法线平均
  膨胀（描边）。
- 排版：hmtx 步进 em、line_center_y=(asc+desc)/2、x 居中、fit_width 缩放
  （port of src/text/layout.rs + export::build）。
- 动画/相机/背景/可视化/后处理：从 src/native（Rust）逐函数移植，公式与
  js/anim.mjs 三方一致。

**移植期修掉的三个 bug（都有金标准对照证据）**：
1. earcut 前的孔洞收集写错容器（cut/rings 混用）→ 越界段错误。
2. base_position 漏加 bbox 中心补偿（Rust 里 origin 在 base 与顶点两侧相消）
   → 字形整体偏移、文字带亮度 +27%。
3. **stop 的 yaw/roll 语义**：它们是轨道之上的偏移量（JS yawOff），初版写成
   替换轨道 yaw → 中段全片镜头朝向错误（t=20.35 相机 x/z 互换暴露）。
   修正为独立 yaw_off/roll_off 通道后相机与 JS 逐位一致。

**验收**（金标准 = Rust 原生静帧，Rust 已对浏览器金标准验收过）：
- 相机 t=20.35/325 eye/target/roll 逐位一致。
- 区域亮度比：t=8 天空 1.00/地平线 1.03/文字带 1.02；t=20.35 全区 0.97；
  t=325 全区 1.00。
- 速度：2.7-2.9 fps（OpenMP 6 线程），全片 11010 帧约 68 分钟——比 Rust 版
  2.24fps 更快。

**交付前修掉的第 4 个 bug（2026-10-25 晚，用户实测「能不能渲染」暴露）**：popen 走 cmd /c，首引号被 cmd 吞（行内多引号时）+ 正斜杠路径被 cmd 拒——症状是渲染报 done 但 mp4 不落盘。修法：ffmpeg 路径统一反斜杠 + 整条命令外包一层引号（cmd /c 引号规则的官方 workaround）。150 帧样片实测落盘且画面正确。

**交付（2026-10-25 终局：用户明确「只要软件，不要成片」）**：`cpp\build.bat` → `dist\chamina-cpp.exe`（3.5MB 静态单文件，无 DLL 依赖）。
用法/开关/架构见 `cpp/README.md`。全片渲染能力已验证可用（2.7-2.9fps）但按需执行，不再默认产出。

**现在项目里有三条等价渲染通路**（同一场景 JSON → 同一画面）：
1. JS + 浏览器（js/scene.js，作者工具链的原生预览）
2. Rust 软件光栅（src/native，chamina-native.exe）
3. **C++ 全引擎（cpp/src/chamina.cpp，chamina-cpp.exe）——字体/排版/动画/
   渲染从此不依赖 Rust 与 Node**

## 九、性能优化第一批（2026-10-28，接手会话验证）

**已落地**（CPP_PROF=1 可输出各段毫秒）：
- skydome：星云半分辨率采样 + 双线性上采样；powf/expf → 1D LUT
  （lut_pow085 4097 级 / lut_exp5 2049 级）。render 138ms → **99ms**。
- finish：sRGB 编码 LUT；bloom 级联上采样（5 次全幅遍历 → 1 次）。
  finish 168ms → **140ms**。
- Frame 复用（render_frame_into 进出参，不再每帧新建 24MB Frame）。
- 颜色清屏跳过：背景激活时 dome 全覆盖每像素，`resize` 代替 `assign`
  （注意必须 resize——dome 直接索引向量，assign 空向量会段错误；
  平面背景场景仍需 assign 清屏）。

**验收**（js/region-cmp.mjs，exit 0=过）：静态验证 `--still 8/20.35/325`
+ 30 帧视频段 `--from 570 --frames 30`。新旧二进制（优化前/后）同刻对比
各区域比值 0.998–1.000；对 Rust 金标准 t=8 sky 1.005 / horizon 1.014 /
text 1.030 / full 1.020，t=20.35 full 1.026，t=325 full 0.989。

**速度**：3.25-3.61 fps（视频段实测），对比优化前 2.7-2.9 fps。

**待做**（按性价比）：grade/dof half-res 打包合并进 blur pass（收益变小）、
SIMD/wgpu 大项。已完成的：blur 静态缓冲复用+跳死代码水平 pass、dof/grade/
srgb/bytes 全链静态化与原地化（finish 68.9ms）、ACES fit 1D LUT（4097 级
域 [0,8]，域外精确求值）、文字块 fade 窗口 ±5s 预剔除、skydome 三张查表
（格点 hash 1024×512 急切表 / 星点胞 381×191 r+j0+j1 / 每帧闪烁胞缓冲
73k sins）。skydome 分解：base 11.9 / 星云 19.5（fbm 9.7）/ 星点 19.8。
视频段 5.92 fps（60 帧实测）。全部经 js/region-cmp.mjs 回归像素级零漂移
（t=8/20.35/325 金标准 + t=12/100 新旧 A/B 均 1.000）。


## 十、`lrc2scene` → `chamina-cpp gen` 移植（2026-10-28，字节级对齐完成）

**目标**：甩掉 Node 依赖的 LRC → 场景 JSON 生成链。权威源：
`js/lrc2scene.mjs`（792 行，CLI 语义/LOOKS 表/键序）、`js/lrc.js`（791 行，
LRC 解析/词组/逐字时间戳）。实现在 `cpp/src/chamina.cpp` 的 gen 节
（约 950 行），`main()` 入口 `argv[1]=="gen"` 分发。

**对齐标准：与 `node js/lrc2scene.mjs <lrc> ... -o gold.json` 的
`JSON.stringify(scene, null, 2)` 输出字节级一致**。为此不用 nlohmann 的
`dump()`（其浮点打 `"14.0"` 而 JS 打 `"14"`），而是：
- `nlohmann::ordered_json` 保 JS 对象键插入序——JS 对象字面量语义
  （已存在键=原位覆盖、新键=按字面量序追加）与 `ordered_json::operator[]`
  语义恰好一致；
- 自写 `js_dtoa`：ECMAScript Number→String（最短往返、仅 |v|≥1e21 或
  |v|<1e-7 用指数形式、-0→`"0"`；NaN/±Infinity 由 JSON 序列化打 `"null"`）；
- 自写 `js_json` 递归序列化器（缩进/转义/键引号与 JSON.stringify 一致）。

**必须复刻的 JS 怪癖（均有测试验证）**：
- `clamp(null)`：`Number(null)=0` 被夹到 lo——`--fx` 单独使用时
  flash/vignette/saturation 全为 0，是正确行为不是 bug；
- `Math.round = floor(x+0.5)`；
- `Math.max(NaN,x)=NaN` / `Math.min` 同理（js_max/js_min helper）；
- `Number("")=0`（RATIO 等解析）。

**LRC 解析**：按 `js/lrc.js` 正则语义手写解析器——`[mm:ss]`/`[mm:ss.xx]`
/带分数三种时钟、负 offset、多时间戳重复行、`{e:...}` 行标签、
`<mm:ss.xx>` 逐字时间戳、空白/无效行过滤。已验证 `charTimes.slice(0,
text.length)` 实为 no-op（charTimes 每码点一条 ≤ UTF-16 长度）。
parse 细节：suggestDuration 用未过滤 lines、buildLyricTexts 用过滤后
lyricLines（next cue=下一个非空行）、firstCue 用未过滤 lines[0]。

**移植范围（第一阶段）**：single 布局默认路径全量——LOOKS 表、背景
cuts/fade、调色板、fit_width、orbit 重调、相机运动（dolly/crane/shake/
move/yaw_drift）、标题/副题/片尾卡、fx、jitter/wave、outline、texts 键位
语义。（第二阶段补齐 `--layout field` / `--groups auto`，见 §十四。）

**验证**：自写测试 LRC（meta/offset/多时间戳/逐字/空行/{e:} 标签）+
14 组 flag 组合（entrance×4、jitter+wave、offset/duration/fps/tail、
fit-width、bg-look+inline palette+bg-cut+fx、fx 参数、palette+outline、
标题/副题/片尾卡全套、dolly/crane/shake/move、keep-open/keep-camera/
yaw-drift=0、radius）全部与 node 输出字节级一致；真实语料
`scenes/bird.lrc`、`scenes/sample.lrc` 字节级一致（0 失败）。

**移植期修掉的 bug**：
1. argv 循环未跳过 `"gen"` token → 报 unknown argument；
2. `match_clock` 带分数分支长度公式多加 1（`[00:05.500]` 11 字节算出
   len=12 → substr 越界）；
3. `NOMINMAX` 重定义需 `#ifndef` 守卫（windows.h 顺序敏感）。

**Windows 坑（三个，全部修掉）**：
1. argv 非 ASCII 损毁：`GetCommandLineW` + `CommandLineToArgvW` +
   `WideCharToMultiByte(CP_UTF8)` 重解码 argv（为此 build.bat 加
   `-lshell32`）；
2. MinGW `ifstream(char*)` 走 ANSI codepage，中文路径必挂——文件读写一律
   `filesystem::u8path(path)`；
3. 静帧 PNG 中文输出路径——改用 `stbi_write_png_to_func` + 宽路径写出
   （不再走 stbi_write_png 的窄路径）。
   （仍存：ffmpeg popen 的输出文件名经 cmd /c 会损毁非 ASCII 路径——
   已知限制，出片文件名避免中文。）

**已知偏差（永久记录）**：
- `Math.tan`/strtod 与 V8 可能差 1 ulp（影响 fit_width 最末位小数）；
- `localeCompare` 平局退化为码点序；
- strtod 接受 `"inf"` 而 JS `Number("inf")` 为 NaN（LRC 数值解析边界）。


## 十一、`audio-env` → `chamina-cpp audio` 移植（2026-10-28，字节级对齐完成）

**决策**：放弃早先设想的 miniaudio+pocketFFT——权威源 `js/audio-env.mjs`
（154 行）实际是 ffmpeg 子进程解码 + 手写 radix-2 FFT，C++ 同构复刻即可
字节级一致；渲染器本就依赖 ffmpeg（find_ffmpeg 已有），零新 vendored 库。

**对齐方法**（关键在复刻 Float32Array 舍入链）：
- 解码：ffmpeg 同参数集（`-f f32le -acodec pcm_f32le -ac 1 -ar 44100
  pipe:1`），尾部不足 4 字节丢弃（对应 `buf.length >> 2`）；
- 分析：N=2048、hop=rate/fps、off=round(f·hop)；Hann 窗按 **f32 存储**；
  radix-2 原位迭代 FFT——**float 数组 + double 运算**：蝶形每次存回舍入
  到 f32（相对误差 ~6e-8），cr/ci/wr/wi 全 double（libm 1 ulp 差 ~1e-13
  被完全淹没）；频带累加 `sum += re²+im²`（单乘 exact、每步一舍入）→
  `mag=sqrt(sum/(hi-lo))/1024` → `db=20·log10(mag+1e-9)` →
  `clamp((db+60)/60)` → round3（Math.round=floor(x+0.5)）→ 三位小数；
  读越界样本按 undefined→NaN 复刻（NaN 行序列化为 "null"）；
- 序列化：手写紧凑 JSON（键序 fps/bands/frames/lo/hi/data；js_dtoa 出数；
  NaN/Inf→"null"；单行无缩进）。

**验证**：`鸟之诗.mp3`（5.9MB，22068 帧）默认参数 + 3 组 flag
（`--fps 30 --bands 32`、`--bands 16 --lo 30 --hi 16000`、
`--fps 24 --bands 96 --lo 20 --hi 20000`）全部与 node 版**字节级一致**
（0 差异）；错误分支（audio too short/坏文件/未知 flag/--help）与
node 行为一致；中文输入路径实测通过。分析耗时 ~4.7s（node 版分钟级）。

**Windows 坑×2（均修，渲染端连带受益）**：
1. popen → CreateProcessW：popen 的命令行走 ANSI(=ACP 936)，UTF-8 中文
   路径转换失败时 _popen 直接返回 NULL——**渲染端视频管道同样中招**
   （HANDOFF 旧「中文输出路径实测正常」不成立，实证为直接起不来）。
   `VideoPipe`（渲染端）/`decode_audio`（音频端）都改为 CreateProcessW
   直起 ffmpeg（UTF-16 命令行 + 匿名管道；stdout 丢 NUL，stderr 透传，
   管道断裂检测保留：stub ffmpeg 下 frame 1 即报错 exit 1）；
2. 句柄继承陷阱：CreateProcessW(bInheritHandles=TRUE) 会复制**所有**可
   继承句柄——管道两端都被子进程带走，我方 close 后 EOF 仍不到，症状是
   最后帧之后永久挂起。`SetHandleInformation(HANDLE_FLAG_INHERIT, 0)`
   把我方持有的端设为不可继承（两端各自设置）。

**find_ffmpeg**：安装版路径补 exe 相对解析兜底（`dist\chamina-cpp.exe`
从任意 cwd 可用；此前只认 cwd 相对路径）。

**错误路径偏差（诊断文本，非输出）**：ffmpeg 失败时 node 把 stderr 尾部
拼进错误消息，C++ 端透传到自身 stderr + 报 exit code——信息不丢失，文本
不逐字一致；NaN hop 在 node 会静默产出垃圾文件，C++ 端按 audio too short
报错（仅错误路径差异，合法输入输出不受影响）。


## 十二、崩溃/边界 fuzz 第一轮（2026-10-28，48 例全清）

**harness**：node spawnSync 喂二进制（typed shell 参数在此 harness 会 GBK
双重损坏，spawnSync 的 UTF-16 通道不受影响；真实 cmd 用法不受影响）。
判定：exit∈{0,1}=清；Windows 崩溃码族（0xC0000005 段错 / 0xC0000409
abort）=崩。harness 在 tmp/fuzz/harness.mjs（不入库）。

**渲染端 22 例**（bird.json 突变）：空 texts / 无 texts 键 / 无
camera_animation / 单字场景 / depth=0.001 与 0 / fit_width=0（部分与全部）/ 
animations 空 / 未知动画类型 / 无 material / 无 position / size=500 /
1e308 级坐标 / 空 backgrounds / 无 fx·bloom·dof / fps=0 / fps 缺失 /
duration 缺失 / char_times 截断 / char_times=null / bg 只填 color_a 一个字段。

**gen 端 26 例**：空文件 / 纯空白 / 无时间戳文本 / 截断时钟（`[00:`、
`[00:06`、`[]`、`[:]`）/ offset 非数 / 负 offset / 多时间戳重复行 / 残缺
`<0:0.>` 逐字戳 / 50000 码点长行 / 3000 行 / 非法 UTF-8 字节 / BOM / CR-only
/ CRLF / 未知与空 `{e:}` 标签 / 重复 meta / 未来时间戳 / 极短歌+--tail 0 /
fps=0 / size=0 / depth=0 / fit-width=0 / offset=1e18 / radius=0.0001。

**结果**：首轮 6 例崩溃——渲染端无 try/catch，nlohmann `out_of_range` 直抛
terminate（0xC0000409）。gen 端 26 例首轮即全清。修复后 **48/48 全清**。

**修复**（双层防御）：
1. main() 拆出 `render_main()`，main 里整体 try/catch → 任何异常干净
   exit 1（gen/audio 子命令本就有各自的 try/catch）；
2. render_main 入口的「老场景兼容归一化」扩容：texts 逐项补
   text/position/size/alpha/animations/material（默认值=gen 旗标默认）、
   backgrounds 逐项补 top/bottom/accent/at（中性天空色）、fps 缺失/非法→
   30、duration 缺失→300。一次 stderr warning，合法场景零路径变化。

**归一化不改变合法画面**的复核：t=8 门禁四区比值 1.005/1.014/1.030/1.020
与扩容前逐位一致；gen bird/sample 字节级一致；60 帧视频段管道正常（~6fps）。

**遗留 fuzz 面**（未做，供后续）：随机字节 LRC 语料长跑、场景 JSON 的
自动化深度突变；char_times 元素混型的运行期守卫已加（is_number）。


## 十三、单头库化 `chamina.h`（2026-10-28，stb 风格交付形态落地）

**决策**：最终交付形态从「单 TU 的 exe」升级为「单头库 + 薄壳 CLI」。
`cpp/src/chamina.h`（stb 风格；gen 第二阶段后约 5900 行）= 引擎全部；
`chamina_cli.cpp`（~120 行）= 旧 main + 参数解析，include 头文件后编译出
同一个 `dist/chamina-cpp.exe`。渲染公式零改动。

**头文件结构**（一次性生成器 `tmp/split.cjs` 从 chamina.cpp 拼装；源文件
已删，此后 **chamina.h 即唯一直接编辑的引擎源**，改动直接落在头文件里）：
1. banner/用法文档 + include 守卫（`CHAMINA_H_INCLUDE`）；
2. `namespace chamina` 公共 API 声明（仅 `<cstddef>/<cstdint>/<string>`）；
3. `#ifdef CHAMINA_IMPLEMENTATION` → 引擎体 = 原 chamina.cpp 第 16–4177 行
   **逐字**迁移（vendored includes、TRACE、全部引擎节、gen/audio 实现、
   VideoPipe、utf8_argv）；
4. 公共 API 实现（原 main/render_main 的迁移，见下）；
5. 两个 `#endif`（IMPLEMENTATION / include 守卫）。
vendored 依赖（json.hpp/earcut.hpp/stb_truetype.h/stb_image_write.h）经
`-Icpp/vendor` 解析；完全自包含 amalgamation（依赖内联进头文件）见本节末
Phase 2 记录。

**公共 API**（`namespace chamina`）：
- `struct Engine` 不透明；实现里持有 `scene`/`meta` 拷贝（`PreparedText::
  anims/char_times` 指针的宿主，与原 render_main 局部 scene 的存活期规则
  一致）+ `World`（World 构造函数本来就按值拷贝 scene/meta，结构相同）+
  `FontEngine` + w/h/fps/total；
- `engine_create(scene_json, audio_env_json, err, err_cap)`：吸收
  render_main 的装配段（场景解析 → 兼容归一化逐字 → meta 归一化 →
  env 注入 → 字体回退链 → prepare → texts mesh 循环 → sparks）。差异仅三
  处：场景/包络改为解析**传入的 UTF-8 字符串**（文件读取上移到 CLI 层）、
  字体失败改为填 err 缓冲（原为 fprintf+return 1）、整体 try/catch 不抛
  异常（错误语义与原 main 的 catch 兜底一致：`error: <what>` exit 1）；
- `engine_render_rgb(e, t, rgb)`：RGB24 直出（宿主喂自家纹理），异常时
  清零 + stderr，从不抛出；
- `engine_still_png(e, t, png_path)`：原 still 分支逐字（stbi_write_png_
  to_func + 宽路径写出）但**不再打印** `still t=… -> …`（上移到 CLI，
  stdout 行为不变）；
- `engine_encode_video(e, from, frames, out_path)`：原 video 分支逐字内联
  （`count = frames ? frames : total - from` 的 unsigned 下溢语义、逐段
  CPP_PROF 计时位置、管道断裂检测、`[cpp] frame i/count` 进度全保留）；
  渲染+finish 不抽 helper——helper（`engine_render_frame`）只用于
  render_rgb/still_png 这两条无计时路径，避免扭曲 CPP_PROF 的分段语义；
- `engine_width/height/fps/total_frames`、`set_trace`、`read_file`、
  `argv_utf8`（存 static，二次调用会失效——程序期调用一次）、
  `gen_main`/`audio_main`（转发全局 ::gen_main/::audio_main，argv[0]=
  子命令 token）。

**悬垂指针关键点**：mesh 循环原来迭代局部 `scene` 并把
`tx.anims = &spec.at("animations")` 指进它——库化后 render_main 一旦返回
局部 scene 即消亡。engine_create 必须**迭代 `e->scene`**（Engine 自己的
拷贝）装配，指针随 Engine 存活。 World 内部用的是自己的拷贝
（`World(const json&, const json&)` 按值拷贝），行为不变。

**回归（全部绿，新旧二进制逐字节 A/B）**：
- 静帧 t=8/20.35/325：old/new MD5 全等（C6F14C81/1D356417/FF6222E9）；
  stdout 行 `still t=8 -> …` 逐字一致；region-cmp 与记录值精确吻合
  （t=8 1.005/1.014/1.030/1.020，t=20.35 full 1.026）；
- gen bird/sample：与 node 字节级一致；
- audio：与 node 字节级一致（22068 帧 7,703,325 字节，`鸟之诗.mp3`）；
- 60 帧视频段（--from 100 --frames 60）：old/new exit 0 且 mp4 **字节
  相等**（925,553 字节）；
- fuzz 48/48 重跑全清；
- 错误分支：缺文件（`error: cannot read …`）/ 坏 --still（`error: stof`）/
  无参数 usage / gen 无输入 / audio too short 全保留；
- 嵌入验证：`cpp/example/main.cpp`（自身定义 IMPLEMENTATION）独立编译
  运行，出 PNG 与 CLI 静帧**逐字节相等**；仅声明 TU（另一 TU 只 include
  不定义 IMPLEMENTATION）单独编译通过——stb 双 TU 用法成立。

**构建**：`cpp/build.bat` 改为编译 `chamina_cli.cpp`（`-Icpp/vendor`
不变）。嵌入示例编译：`g++ … -Icpp/vendor -Icpp/src cpp/example/main.cpp
-o dist/chamina-example.exe -lshell32`。

**文件变动**：`cpp/src/chamina.cpp` 删除（引擎体 16–4177 行进 chamina.h，
git 按相似度记 rename）；新增 `chamina.h` / `chamina_cli.cpp` /
`example/main.cpp`；拆分源料 `tmp/split.cjs` + `tmp/chamina_head.txt` +
`tmp/chamina_api.inc`（不入库）。

**Phase 2（2026-10-28，amalgamation 完成，`dist/chamina.h` 完全自包含）**：

- **工具**：`cpp/tools/amalgamate.cjs`（入仓库）——把 4 个 vendored 头
  （nlohmann/json 21,595 行 + mapbox/earcut 1,031 + stb_truetype 4,490 +
  stb_image_write 1,520）逐字内联进 src 头对应 `#include "…"` 的位置，
  产出 `dist/chamina.h`（38,719 行，生成**确定性**——同输入重跑 MD5 相同）。
  dist/ 不入库，按需再生成。
- **行尾**：chamina.h 是混合行尾文件（banner/API 半 LF、引擎体 CRLF），
  工具按 `split('\n')` 保留行尾 `\r` 处理，splice/join 字节无损。
- **验证**（`tmp/amalgam_check.mjs`，9/9 绿）：
  1. 活代码零引号 include——扫描带条件编译嵌套跟踪，仅剩 stb_truetype.h
     `#if 0` 示例程序块内的 2 处自 include（`#if 0` 内的 `#include`
     预处理期不展开，inert；vendored 文件保持逐字内联，接受）；
  2. 单文件拖进普通宿主目录（tmp/amalgam/，无 -Icpp/vendor、无 -Icpp/src），
     `cpp/example/main.cpp` 与 `chamina_cli.cpp` **均编译通过**（注意：
     引号 include 先搜包含者目录——宿主 TU 必须与头文件同目录或用 -I
     指向它，否则会被 cpp/src/chamina.h 遮蔽）；
  3. 两者出图与金标准逐字节相等（example t=20.35、CLI t=8）；
  4. 仅声明 TU（api_only.cpp，include 不定义 IMPLEMENTATION）对 amalgam
     编译通过——stb 双 TU 用法成立。
- **交付语义**：宿主工程只需一个 `chamina.h` + 自家一个 TU；src 头
  （+ cpp/vendor）仍是在编辑的开发形态。


## 十四、gen 第二阶段：`--layout field` / `--groups auto`（2026-10-28，字节级对齐完成）

**目标**：接管 gen 剩余旗标——`--layout field`（js/layout.mjs 741 行的
layoutField）、`--groups auto`（js/lrc.js wordGroups/groupCharTimes）、
`--roll-power`（whip）、`--force-whip`、`--energy-file`（loadEnergy）。
实现在 chamina.h gen 节（field 组约 600 行）。

**对齐要点**（全部 double 精度——输出是场景 JSON，必须与 node
JSON.stringify 逐字节一致；与渲染端的 float 管线不同）：
- **wordGroups**：WORD_SPLIT_SPECS 20 条手写分词（'|' 分隔，尾随空格
  随前词）；PHRASE_CUTS 15 种节拍函数（thirds/chunks 复刻）；短语在空格后
  切；PHRASE_CUT_OVERRIDES 仅 0:0→表[3]、0:1→表[12]（kind 字段仍记公式
  值）；seed % 15 的 kind 与 `(seed*5+phraseIndex*3) % 15` 公式逐字。
- **groupCharTimes**：组内扫掠 35ms（`g.t + (i-i0)*0.035`）。
- **layoutField**：serpentine GRID{5,30,15}（row0 从中列向右）；STAGES 7 帧
  循环；boxW/H 按 emWidth（space 0.2959，余 1em）×size 与 fitWidth 解析；
  rLo/rHi = base±1.3（下限 11.6）；revealEnd=末戳（groups 时=末组 t+
  (n-1)*0.035）；flightAt/Dur（gap*0.45 上限 min(14, gap-0.4) 与
  TRAVEL_MAX=2，i=0 钳 titleOut、i>0 钳 prevCue+0.8，再钳 cue-0.4）；
  framingFits 四角投影 0.93/0.94 门（roll 盒膨胀）；slot = P −
  (sx·fw·right + sy·fh·up)；FLY_YAW/ROLL 交替签名，不合则收平。
- **whip**：flight whip 门（e<0.52 或 dur<1.0 或 rng<0.22 拒，**r() 只在
  `!forced` 时消耗**——JS && 短路流对齐）；flightWhip = π/2·(0.8+0.2r)·
  min(whip,1)·(0.72+0.28e)；三停结构（ease_in_quad 甩入 → back_out 飞行
  过冲 → ease_out_quad 回平，settleClamp = min(0.55, max(0.12, gap-0.88),
  backAt-(cue+0.08)-0.02)）；词内 whip 门（e≥0.78 或 i=0 或 forced，组>2，
  最宽词距 ≥1.0，i=0 用 π/2·(0.85+0.1r) 否则 0.5+0.35r）；whipSign 全局
  交替且**只在实际放置时消耗**。
- **choreo**：rng（mulberry 风格 0x6d2b79f5，Math.imul 无符号 mod 2^32 复刻
  ——`t ^= t + imul(...)` 的 JS 浮点加进位由 mod 2^32 吸收）；MOVES 12 键
  按对象序索引；rollMove 幅度带（roll 0.05-0.16 / yaw 0.07-0.20 /
  r 0.5-1.5 / e 0.02-0.07 / pan 0.06-0.15）；assignChoreo 签名去重（盐
  rng(i·7919+salt·104729+17)，80 次上限，退回 rollChoreo(k,i+1)），第 0 行
  OPENING_PREFIX（down 0.34/ease_out_quad/lead−0.12/pace 0.85 + up 0.13/
  smooth + right 0.31/ease_in_out_cubic）占前 need 个 between 位；
  六次减半预算 + 退化丢弃；deadline = flightAt[i+1] 或 backAt。
- **exits**：相机路径整体（rig+stops）逐 0.1s 步进扫描（浮点累加与 JS
  相同序列），boxOnFrame（眼后角容忍、全眼后即出画）；from = max(cue+
  fadeIn+0.15, revealEnd+0.15, lineEnd+0.15)；尾停 backDur≥0.4 时回题景
  （yaw/roll 强制 0）。
- **loadEnergy**：envelope mean band（cue−0.8..cue+1.5，Math.round 帧界），
  min-max 归一 round3；错误行为与第一阶段 check_energy_file 相同
  （--energy-file: … has no envelope data），故该函数删除。
- **buildLyricTexts**：--groups auto 时 charTimes 先 groupCharTimes 再
  round3；texts 覆盖（position/exits）在 palette/keep-open/outro **之前**
  （键序与覆盖语义同 JS）。

**验证（全部字节级，node spawnSync UTF-16 通道）**：bird.lrc 8 组合
（field / field+groups / field+groups+whip+energy / +force-whip / groups
单用 / field+entrance fall+jitter / field+keep-open / field+outro-at）、
sample.lrc 4 组合（field / +groups / +whip / +outro）、keep-camera+field
（模板 stops 被 layout 输出替换）——**13/13 字节级一致**；energy-file
空数据与缺文件错误路径 exit 1 且消息一致；single 布局回归逐字节不变；
field 场景经 C++ 渲染端实渲 t=20 与 whip 段 t=8.4 两张静帧无崩溃
（stops=34 = 33 行 + 收尾回题景）；渲染端门禁 t=8 四分数与 fuzz 48/48
复验无变化。

## 十五、fuzz 第二轮与 JS 越界语义修复（2026-10-29，_gen 语法面的真实案件）

**第一轮 fuzz（§0c）打的是构造畸形输入；第二轮（tmp/fuzz2）打的是
gen 的旗标面**：随机可解析 LRC 语料（含 `決して`/全角空格/`词，词`
等边缘片段）× 随机旗标组合（--layout/--groups/--roll-power/--force-whip/
--offset/--outro-at/--keep-open/--entrance/--energy-file…），node
lrc2scene.mjs 与 C++ gen 双跑逐字节比对 + exit 对齐，另加 8 例渲染冒烟
与 4 例错误路径（tmp/fuzz2/harness.mjs，旗标落盘 flags_case_k.json）。

- **首种子 20261028：48/48 全绿**。换种子 77（32 例）抓到 2 个失败——
  第一批 fuzz 只打自身，换种子才见真伤。
- **case_14**（`--layout field --groups auto --offset -1583`）：
  fade_out_at node `null` vs cpp `0.15`。
- **case_26**（`--layout field --groups auto --outro-at 88.52… --keep-open`）：
  cpp 直接崩（exit 3221226356 = 0xC0000474 fail-fast），node exit 0。

**取证链（tmp/fuzz2 插桩：lrc_dbg.mjs 是带 GCT 探针的 lrc.js 副本）**：
语料行 `飞鸟 过往 风雨 词　词  不是词 了  在`（22 码点、22 戳）。
PHRASE_CUTS 的 `chunks(n,3)` 在 n=1 时产出 `[-1,1,1]`→filter(>0) 后
`[1,1]`——**piece 之和 2 > 短语长 1**，组链推进越过行尾（i0=22）：

1. `wordGroups` flush 的 `t: charTimes[i]` **越界读 → `undefined`**；
2. `groupCharTimes` 的 `out[i] = g.t + (i-g.i0)*0.035` 在 i=22 处
   `undefined + 0 = NaN`，且 **JS 数组就地生长**：场景 char_times 变
   **23 条、末位 null**；
3. `Math.max(...charTimes)` **任一 NaN → 全 NaN** → lastGlyph/outAt =
   NaN → `JSON.stringify(NaN)` → `fade_out_at: null`；
4. layoutField 侧 `revealEnd = g.t + …` 同样 NaN → exit 扫描
   `t=NaN` 不起步 → exits `null`（两个 NaN 源汇合，一定写 null）。

C++ 侧对应三处全是 UB/语义差：`char_times[i]` 越界读（堆邻域脏值→
case_26 崩溃嫌疑）、`out[i]` 越界写、`std::max_element` **静默跳过 NaN**
（NaN 与比较器恒 false）→ lastGlyph 停在次元戳上，outAt 居然算出
0.15 这种「正常值」，所以 diff 是 0.15 vs null 而不是 NaN vs null。

**修复（chamina.h，均按 JS 逐字语义）**：
- 新增 `js_max_of`/`js_min_of` 折叠（任一 NaN → NaN），替换
  `max_element`/`min_element` 三处（buildLyricTexts 的 last_glyph、
  layout_field 的 reveal_end、loadEnergy 的 mn/mx）；
- `word_groups` flush：`i < ct.size() ? ct[i] : NaN`（undefined 参与
  后续算术 ≡ NaN）；
- `group_char_times`：越界写改为 **JS 生长语义**（`resize(i+1)`，中洞
  填 NaN——undefined 的算术等价物）。

**验证**：repro77 c14 全等（1625 行逐字节）、c26 exit 0；seed 77
32/32 与 seed 20261028 48/48 全绿；amalgamation 重跑 9/9；
渲染门禁 t=8 与历史值一致（1.005/1.014/1.030/1.020）、t=20.35 新旧
二进制同刻 A/B 全比率 1.000、t=325 带内——改动只在 gen 词法路径，
渲染器零涉及。

**小结**：JS 的三条隐藏语义必须逐字复刻——(a) `arr[i]=v` 就地生长；
(b) `undefined`/NaN 参与算术全传播、`Math.max/min` 任一 NaN 全 NaN；
(c) `std::max_element` 之类 STL 折叠对 NaN 的行为与 JS 相反。病毒级
语料（一本正经的可解析 LRC）+ 换种子复跑，是逼出语义差的唯一手段。

**同日第二面（stamp 宽域 + astral/emoji 语料）**：语料加
`🎵`/`鸟🎵`/`𝕏`/`a\u0301b`/零宽等 astral 与组合字符，时间戳生成从
固定 `[mm:ss.xx]` 扩为 node TIME_RE 接受的全部形态——1 位分钟
（unpadded）、1 位秒、冒号作小数分隔符、全 3 位小数保持。C++
`match_clock` 与 node 正则逐字对齐（`\d{1,3}`/`\d{1,2}`/`[.:]\d{1,3}`）
在 4 个种子 128 例全部通过——**行内戳/行戳的宽域格式双方吃掉**。

**6/32 失败的根因全部是同一已知偏差被触达**：宽域首轮还带整秒 floor
与 1 位小数 round 变体，同秒内两行会**同值不同文本**（`[00:05]` 与
`[00:5]` 都是 5.0）——node 的行排序 tie-break 是
`a.text.localeCompare(b.text)`（ICU collation：即零宽与 emoji 作低
权重/可忽略），C++ fallback 是 codepoint 字节序（`🎵`=U+1F3B5 在
`梦`=U+68A6 之后，故 C++ 排 row11 前；ICU 把 🎵 排前）。这是
chamina.h 头注释「Known deviations」中声明的那条，被广谱语料
触达——复刻 ICU 的版本依赖语义不现实，保持 codepoint fallback，
harness 改用**值保持**的宽域形态（全 3 位小数、只变表示不改值），
同值 tie 不再自造；wide+emoji 轮 4 种子复跑全绿。累计五种子 176 +
宽域/emoji 四种子 128 = **304/304**。

**同日负结果（finish 融合试验）**：试图合并 grade/dof 的后处理趟数
（dof mix 内联 tonemap、无-fx 直灌 pack）——像素级四路 A/B 逐字节
相等，但配对中位测量 +1.8ms/f：tonemap 独立 pass 的自动向量化机会被
标量混合循环破坏，省下的读写趟盖不回，已 revert（HANDOFF §3 待做 #4
标定驳回；此结论同样封存了「pack 直灌」的变体，直到 SIMD/wgpu）。


## 十六、SIMD 分发层（2026-10-31，Tier A 位精确双内核落地）

「SIMD/wgpu 大项」的首步不是 wgpu（C++ 单头栈没有无障碍的 GPU 后端），
而是 CPU 端向量内核——先立**分发接口与数值政策**，再逐热点接入。

**工具链事实（tmp/probe_attr.cpp / probe_fma.cpp 实证）**：
- `target_clones` 在 Windows/PE **不可用**（依赖 ELF ifunc）——运行时
  分发只能手工做：成对 kernel（scalar / `__attribute__((target("avx2")))`
  向量版）+ 一次性 `__builtin_cpu_supports("avx2")` 函数指针解析；
- 本机 CPU：AVX2+FMA 有，AVX512 无；
- GCC 15 的 AVX 命名与 Intel 不一致：`_mm256_cvttps_epi32`/
  `_mm256_cvtepi32_ps`（不是 Intel 的 `_mm256_cvttps2dq`/`cvtdq2_ps`）；
- 默认构建是 SSE2 基线，immintrin 的声明须放在 `#pragma GCC push_options
  target("avx2")` 窗口内 include 再 pop（kernel 本体自带 target 属性）；
- GCC 15 在 avx2 target 函数内**不**把 `a*b+c` 合约为 FMA（200 万随机
  输入零差异、反汇编无 vfmadd），但内核仍显式钉
  `optimize("fp-contract=off")`，防跨版本悄悄漂移。

**数值政策两档**：
- **Tier A 位精确**：lane 内与标量体同序（无重结合、无 FMA；div/sqrt
  走硬件 IEEE 一致；LUT gather 读同一表项；`(int)` 截断用 cvttps_epi32
  同语义，负值/带外 lane 索引钳进表内后 blend 丢弃）。验收 = 新旧
  二进制静帧 PNG **逐字节相等**。例外域记明在案：int 溢出截断的
  `(int)` 标量本就是 UB（极大 chromatic 的病态场景），向量侧给
  cvttps INT_MIN→钳 0，纹丝不进正版面的值域不适用此门禁。
- **Tier B 近似**：向量多项式超越函数（atan2f/sinf/expf），误差
   
  远低于 8-bit 显著性；验收 = region-cmp A/B ±0.05 门禁。先例：
  ACES/阶梯 LUT 的 ~1e-4 已被上一批优化接受。

**机制**（chamina.h「SIMD dispatch」节）：
- 每个 kernel 一对 `*_rows_scalar` / `*_rows_avx2`（尾循环调用标量体，
  保证非 8 倍数宽度也逐字节一致）；函数指针经魔 static 一次性解析，
  `CHAMINA_NO_SIMD=1` 强制标量路径（配对测量开关）；
- OpenMP 分块按 8-px lane 组切块（blocks = n/8，整组不跨界），块序
  镜像 static 调度，保证 CPP_PROF 段耗时跨版本可比；
- **AoS `array<float,3>` 布局不动**：读经 `_mm256_i32gather_ps`（索引
  i*3+k），写经对齐 scratch + 逐像素交错落盘——dof/bloom 等 pass 共享
  该内存格式，不为此重构；
- 分发点：`tonemap_into`（ACES fit LUT gather + sRGB LUT gather 全幅
  pass）与 `grade_into`（色差采样 gather ×3 + vsqrtps 晕影 + 亮度/饱和/
  闪光）；pack 全幅循环顺手从串行改为 OpenMP 并行（逐像素同式，位精确）。

**验收与测量**（新旧二进制 = 基线 `dist/chamina-cpp.exe@820d77a+SIMD 前）：
- 字节门禁 **8/8 首跑全等**：bird t=8/12/20.35/50/100/325（fx-on，
  grade+tonemap+pack 全走）+ fuzz gen 场景 t=12/100（与官
  `js/region-cmp.mjs` 三帧比值同历史通过值——t=8 sky 1.005/horizon
  1.014/text 1.030/full 1.020，t=20.35 已知采样伪影维持原状）；
- 配对中位（同刻 30 帧窗，交替 5 轮，SIMD 开 vs `CHAMINA_NO_SIMD=1`）：
  - fx-off（fuzz gen 场景 --from 50）：finish **86.8 → 61.0ms**；
  - fx-on（bird --from 100）：finish **108.4 → 66.9ms**（对基线二进制
    96.6 → 66.9 = **-29.7ms**）；
  - render 段两场景均在噪声带内（±5ms）——skydome 本批未动；
- amalgamation 39070 行再生成，宿主验证：example 单 TU（-Idist）编译
  + 渲染与 CLI 路径逐字节相等；api-only 二 TU 干净。

**wgpu 前瞻**：本批 kernel 都是「全幅连续缓冲上的帧函数段」，签名即
未来 GPU 后端可按同签名接管的接口位；本会话不引入运行库依赖。

**后续队列**（按收益排）：skydome 全幅循环的 Tier A 部分（normalize/
sink sqrtf、梯度 LUT lerp 向量化）；星点 atan2f 与 fbm sinf 的 Tier B
向量多项式（走 region 门禁）；可选 byte-pack 真打包内核。
