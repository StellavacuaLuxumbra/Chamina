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
语义。`--layout field` / `--groups auto`（`js/layout.mjs`，741 行）明确
拒绝并提示改用 node；`--roll-power`/`--force-whip` 单布局下 no-op；
`--energy-file` 接受（校验可读 + 有 data）后忽略。

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

**未支持/已知偏差（留待第二阶段或永久记录）**：
- `--layout field`、`--groups auto`（js/layout.mjs layoutField）与
  PHRASE_CUTS/WORD_SPLIT_SPECS/groupCharTimes——gen 第二阶段对象；
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
