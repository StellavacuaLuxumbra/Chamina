# Chamina — 会话进度小结

> 本文由会话结束时实际写入（此前有一次「已写入本文件」的表述并未真的执行过，特此以真实内容为准）。

## 一、目标

用 Rust 库 `chamina`（纯计算层：字体解析 / 排版 / 动画 / 场景，导出 JSON）
+ Node.js / Three.js 无头渲染器，输出可反复渲染的成品 MP4（生日礼物 PV）。
最终一条命令跑通：

```
chamina-render <scene.json> -o gift.mp4
# = Rust 导出 scene_data.json → Node 逐帧截图 → ffmpeg 编码
```

## 二、环境（关键，后续会话别再踩）

- 机器无 VS / Windows SDK，`link.exe` 不可用 → **MSVC 链不出来**，改用 GNU 工具链。
- `RUSTUP_HOME=D:\Rust\rustup`、`CARGO_HOME=D:\Rust\cargo`（已设为用户级环境变量），
  PATH 追加 `D:\Rust\cargo\bin;D:\mingw64\bin`。
- toolchain：`stable-x86_64-pc-windows-gnu`（rustc 1.98.1，含 rustfmt / clippy）。
- 链接器 `D:\mingw64\bin\gcc.exe`；**GNU ld 无法处理非 ASCII 路径**，而仓库在
  `F:\项目\Chamina` → `.cargo/config.toml` 设 `target-dir = "D:/ChaminaTarget"`。
- 常用前缀（cwd = `F:\项目\Chamina`）：

  ```powershell
  $env:RUSTUP_HOME='D:\Rust\rustup'; $env:CARGO_HOME='D:\Rust\cargo'
  $env:Path="D:\Rust\cargo\bin;D:\mingw64\bin;$env:Path"
  ```

- PowerShell `Set-Content -Encoding UTF8` 会写 BOM，`serde_json` 解析失败
  → 用 write/edit 工具，或 `[IO.File]::WriteAllText($p,$c,(New-Object Text.UTF8Encoding($false)))`。
- JS：Node v24.15.0；ffmpeg 不在 PATH → `js/node_modules/@ffmpeg-installer/win32-x64/ffmpeg.exe`；
  浏览器 Edge `C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe`；
  `puppeteer-core@25.12.0`、`three@0.186.1`；ES module 走本地 HTTP server。
- **读图工具（`read` 对 PNG/JPG）在本会话不可信**：会返回陈旧图像。看图只能靠
  - `node js/preview.mjs <png> [cols] [rows]` → ASCII 色彩分类图
  - `node js/lum.mjs <png> [cols] [rows] [gamma]` → ASCII 亮度灰阶图（判断对比度/光晕更直观）
  - ffmpeg 解 raw RGB + node 采样像素统计

## 三、架构与命名

- 仓库重命名 `chumina` → **`chamina`**：package 名、README `# Chamina`、
  `ChaminaError`、bin `chamina-render`（产物 `D:\ChaminaTarget\x86_64-pc-windows-gnu\debug\chamina-render.exe`）。
- 删除全部 Bevy 渲染路径（`src/render/`、`src/offline.rs`），渲染一律交给 JS。
- 桥接格式 JSON；Rust 只负责几何与动画求值，JS 侧 `js/scene.js` 必须**逐字镜像**
  Rust 的 `eval_anim` 公式。

## 四、本阶段完成的工作

### 1. Rust 工具链与工程

- 修通 GNU 构建（见上）；`cargo check / clippy / fmt / build` 全绿。
- `src/animation.rs`、`material.rs`、`scene.rs`、`layout.rs`、`export.rs`、
  `bin/chamina-render.rs` 重写完成。

### 2. 四个早期渲染 bug（已修）

字体单位、`toFlat` NaN、bloom 过曝、三角形绕序。

### 3. 「实心块」问题的定位（结论：几何没问题）

- `js/debug-geom.mjs` 把 front-face 三角形直接投成 SVG→PNG，「生」轮廓完全正常。
- `scene_data.json` 各字世界坐标 bbox 逐字核对，间距/位置全部正确。
- 真正原因是**相机角度 + 挤出深度**导致侧壁糊住笔画间隙；同时相机 yaw 起始值
  让画面多数时间近乎侧看（正面是 `yaw = π/2`）。

### 4. 动画

- **Slide 语义修正**：`offset += delta * (1 - ease(u))` —— 从 `base+delta` 滑入并归位，
  与 Fall/Fade 的「起始偏移→归位」一致；`src/animation.rs` 文档注释与
  `js/scene.js` 的 `evalAnim` 同步。
- 5 个单测：`fall_starts_at_height_and_lands`、`slide_settles_at_base`、
  `fade_ramps_and_fades_out`、`spin_is_identity_before_delay`、`layered_animations_compose`。

### 5. 网格挤出（mesher）—— 本阶段主要工作

按顺序修掉的问题：

1. clippy：`fill.indices.chunks_exact(3)` → `fill.indices.as_chunks::<3>().0`。
2. **侧壁缺最后一段**：`OutlinePen::close()` 是 no-op，轮廓没闭合
   → 改成 `for i in 0..len { poly[i], poly[(i+1)%len] }`。
3. **裂缝根因**：lyon 自行展平的盖面顶点 ≠ 我们 `flatten()` 的侧壁顶点。
   → **只展平一次**：先 `flatten()` 得到 `polys`，`build_path(&polys)` 只喂
   `begin/line_to/close`（无曲线可展平），bbox 改用 `polyline_bbox(&polys)`，
   删除旧的 `outline_bbox(&[Vec<Seg>])`。
4. `dedup_closing()`：去掉连续重复点与首尾重复点（字体确实会输出零长度段，
   否则产生零面积侧壁四边形）。
5. **lyon 沿共线段会输出零面积三角形**（实测 S=33、g=28、a=23 个）：
   - 先剔除它们得到 `keep`；
   - **盖面只用 `keep`**；
   - **侧壁改从盖面自己的边界生成**：统计 `keep` 的无向边，取计数 == 1 的边，
     端点直接用盖面顶点索引 → 闭合性由构造保证；
   - 法线方向用「相邻盖面三角形的第三点」判定朝外，必要时交换 a/b 以保持绕序。

### 6. 测试（`cargo test` 8/8 通过）

- `animation::tests` ×5（动画公式权威定义）。
- `text::mesher::tests::straight_stroke_mesh_is_watertight`：CJK 直笔画
  `生 / 日 / 快 / 乐`。
- `text::mesher::tests::curved_glyph_mesh_is_watertight`：曲线/孔洞
  `g S o a B e n y`。
- `text::mesher::tests::mesh_counts_stay_consistent`。

`assert_watertight` 的判据（重要，不是简单的「每条边恰好 2 个三角形」）：

- **必须**：每条边被**偶数**个面覆盖（奇数 = 有洞 = 真 bug）；
- **必须**：非 2 计数的边 ≤ 5%（防止四边形被重复生成）；
- 允许少量 ×4：轮廓自接触的**钉扎点**（如「生」有 3 处），几何上是闭合的。

> 演进过程：先用「侧壁来自我们的折线」→ CJK 通过但曲线字在共线段处失败；
> 再试「完整盖面 + 未配对有向边」→ 曲线字失败；最终「去退化三角形 + 盖面边界」
> 才同时通过两组。中间还写过 `dump_cap_stats` 临时诊断测试（已删除）。

`cargo clippy --all-targets` 0 警告，`cargo fmt` 已格式化。

### 7. 渲染器与场景

- `js/render.js` 支持 `--url <query>`；still 路径打印 `probe:`（字符 pos/scale/
  quat/opacity、`renderer.info`、`gl.readPixels` 采样、canvas 尺寸）。
- `js/scene.js` 调试开关：`debug / front / nopost / noparticles / nolights / text=<n> / box / nooutline`。
- **新增 `js/lum.mjs`**：亮度灰阶 ASCII 预览（比色彩分类更适合判断曝光与光晕）。
- `scenes/gift.json` 定稿参数：
  - 相机 `orbit radius=14 elevation=0.09 speed=0.1 start_yaw=1.18 target=[0,1,0]`
    → yaw 在 1.18~1.88 之间绕 `π/2` 轻摆，全程接近正面；
  - `depth` 0.4/0.25/0.2 → **0.16/0.10/0.09**（减薄侧壁遮挡）；
  - 三行尺寸/位置：`3.9@y3.4` / `1.7@y0` / `1.3@y-2.3`；
  - line1 `fall{duration:1.0, stagger:0.12, height:1.2, gravity:-2}` + fade`{0.9, 0.12}`；
    line2 `slide{delta:[-6,-2.5,0], duration:1.4, ease:ease_out_quad}` + fade；
    line3 仅 fade；三行 `fade_out_at:6.2 / fade_out:0.8`；
  - bloom `intensity 0.3 / threshold 1.0 / scale 1.2`；
  - `dof.enabled = true, focal_distance = 14`（= 相机半径，见 §10.1）；
  - line1 加 `outline { color:#eafcff, width:0.02, strength:1 }`。
- 三条线颜色经像素采样确认正确：青 `#00e5ff` / 金 `#ffd166` / 粉 `#ff5fa2`。

### 8. 端到端一条命令已跑通

```
chamina-render.exe scenes\gift.json -o gift.mp4
# 420 帧 @60fps 1920x1080，1.94 fps，216s，4.2 MB，7.00s / 4976 kb/s
```

### 9. 刚发现并修掉的 bug（**成片里被拍进去了**）

`js/scene.js` 原来是 `const NOBOX = DEBUG || flag('nobox')`，
即**默认显示**一个 3×3×3 绿色调试方块（只有 debug 模式才隐藏）——
方向反了，`gift.mp4` 第一版里有这个方块。已改为：

```js
const BOX = DEBUG ? false : flag('box');   // 仅显式传 box=1 才显示
// ...
if (BOX) { ... }                            // 原 if (!NOBOX)
```

## 四之二、本会话（构图 / 特效 / 三个真 bug）

### 10.1 景深 DOF 实测与校准

原 `scene.js` 的映射量级错了，**把文字糊掉了**：

```
aperture = 0.025 * (4 / f_stops)   maxblur = 0.01 * focal_radius
→ 文本区 diff 27.5，背景区才 3.6（正负颠倒）
```

BokehShader 是 `factor = focus + viewZ`（viewZ 为负），`dofblur = clamp(factor * aperture, ±maxblur)`。
本场景尺寸是「米级」，按物理 f-number 算出来是亚像素模糊，没有意义 → 改成**艺术化映射**：

```js
const aperture = 0.00017 * (4.0 / Math.max(meta.dof.aperture_f_stops, 0.1));
const maxblur  = 0.008   * Math.max(meta.dof.focal_radius, 0.001);
```

标定结果（t=6.0，与 nopost 对比）：

| 指标 | 结果 |
|---|---|
| 文字锐度（水平梯度均值比） | **0.952**（只损失 4.8%） |
| 背景/边缘 diff | 0.6~1.2（明显软化） |
| 粒子 on/off diff | 全幅 mean 4.3（粒子确实在画面里） |

`focal_distance` 必须跟着相机 `radius` 走（radius 21→14 时同步改）。

### 10.2 【bug】淡入淡出完全无效

`fadeFactor` 算出来是对的（probe 里 `o: 0.125`），但**画面纹丝不动**：
neon 用的是 `MeshBasicMaterial({ color, side })`，**没开 `transparent`**，
three.js 直接忽略 `opacity`。修：

```js
const m = new THREE.MeshBasicMaterial({ color, side: THREE.DoubleSide, transparent: true });
```

验证：t=6.0 → mean 85.4；t=6.9（alpha=0.125）→ **mean 16.6、px>200 289k→1k**。
开场 fade-in 同理生效（t=0.2 mean 5.85 → t=0.7 37.3 → t=1.3 84.5）。

### 10.3 构图：文字块只占画幅 38%

用 `scene_data.json` 的逐字 bbox 精确算过：原参数文字块只填
**38.8% 宽 × 38.3% 高**（相机 radius 21，可见宽 35 单位，标题才 13.5）。

- 相机 `radius 21 → 14`（可见宽 35 → 23.2），`focal_distance` 同步 14；
- 三行尺寸/位置改为 `3.9@3.4 / 1.7@0 / 1.3@-2.3`；
- 实测文字块 → **约 67% 宽 × 65% 高**（像素阈值法量的 lit bbox）。

### 10.4 【bug】相机内移后粒子变满屏光斑

粒子云原来 `z ∈ [-27.5, 17.5]`，radius 14 时相机（z≈13）**直接坐进云里**，
近处粒子 `gl_PointSize = 260/dist` 能到几百像素。两处修：

```js
offsets[i*3+2] = (rnd()-0.5)*45 - 20;          // z ∈ [-42.5, 2.5]，全在文字后方
gl_PointSize = min(aScale * (260.0/max(-mv.z,1e-3)), 46.0);   // 距离钳制
vTw *= smoothstep(2.0, 7.0, -mv.z);            // 贴脸的粒子淡出
```

### 10.5 开场标题在画面外 1.1 秒

`fall{height:2, gravity:-9}` 在 `duration:1.3` 下解出的
`v0 = -(h + 0.5*g*d²)/d = +4.31` → 是**先往上抛再落下**，顶点 +3.03em，
标题要到 t≈1.14 才进画面（前 1.1 秒近全黑，t=0.4 mean 只有 3.93）。
改成 `height:1.2, gravity:-2, duration:1.0`（v0≈0，纯自由落体，
起点 y≈8.08 恰在画面上沿 7.5 附近）→ t=0.7 标题已在画面内。

### 10.6 描边（倒置外壳）

Rust 侧：`Neon.outline: Option<Outline{color, width, strength}>`
（serde `default` + `skip_serializing_if`，向后兼容）+ builder
`.outline(color,width)` / `.outline_from(&other)`，`scene.rs::color()` 改色时保留。
JS 侧 `buildText` 挂一个 `side: BackSide` 的子网格，`uWidth` 单位是 em（随网格缩放）。

**关键坑**：挤出网格的盖面与侧壁是**各写各的法线**（同位置两套顶点），
直接按原始法线外推 → 侧壁整体平移 16px、盖面一点不动，
描边**脱离文字浮在 16px 外**（扫描线实测：rim 在 x654-656，字从 x673 才开始）。
必须先按位置合并、把重合顶点的法线**求平均**再外推：

```js
function smoothHullGeometry(geo) { /* Map<pos, 累加法线> → 归一化写回 */ }
```

修完扫描线量到：rim 与字**连续无间隙**，宽度 ≈ `width × size × 82.7px × ~0.75`。
现设 `width: 0.02` → 约 5px。另加 `--url nooutline=1` 便于 A/B。

### 10.7 亮度构图核对（t=5.0）

| 区域 | nopost | bloom 0.3 |
|---|---|---|
| 顶部 y<90 | 7.3 | 33.8 |
| 左侧 x<90 | 7.6 | 11.5 |
| 底部 y>990 | 7.2 | 5.8 |

即光晕**只跟着标题走**，四角仍是背景色；`sat% = 0.00`（无过曝）。
粒子对亮度的贡献只有 +0.9（可忽略），所以别再往粒子上找「画面发灰」的原因。

### 10.8 本会话成片

```
chamina-render.exe scenes\gift.json -o gift.mp4
# 7.00s / 1920x1080 / 60fps / 4976 kb/s / 4.2 MB / 1.94 fps / 216s
```
亮度弧线（抽帧 mean gray）：0.30s=8.8 → 0.80s=48.7 → 1.50s=84.5 →
5.00s=86.1 → 6.50s=33.1（收尾淡出）。

## 五、歌词 → 成片（本轮）

### 5.1 `start_at` 绝对开始时间
- `src/animation.rs`：`Fall/Slide/Fade` 各加 `start_at: f32`（serde default 0.0）+ builder；`eval_anim` 三处 delay 统一为 `start_at + idx * stagger`。
- `js/scene.js::evalAnim` 镜像（行 119/129/140，`(a.start_at ?? 0) + idx * a.stagger`）。
- 新增单测 `start_at_delays_the_whole_cue`；5 处测试字面量补 `start_at: 0.0`。
- 结果：`cargo clippy --all-targets` 0 警告，`cargo test` 9 passed + 1 doc-test。

### 5.2 歌词自动适配 `fit_width`
- `src/scene.rs::TextSpec` 加 `fit_width: Option<f32>` + builder `.fit_width(w)`。
- `src/export.rs::build`：`let mut size = spec.size;` 按 `max_w/(total_width*size)` 缩放，下游 `base_scale`/`base_position`/导出 `size` 全用解析值。
- 实测最长行 19.59em → size 1.007，world 宽 19.73 ≈ 视口 85%。

### 5.3 LRC 工具链
| 文件 | 作用 |
| --- | --- |
| `js/lrc.js` | `parseLrc / applyOffset / suggestDuration / buildLyricTexts / lyricsToScene`（浏览器与 Node 通用） |
| `js/lrc2scene.mjs` | LRC → scene CLI；默认 fitWidth = 可见宽×0.85、`tuneOrbit` 对称摇摆、bloom 0.06 |
| `scenes/bird.lrc` | 《鸟之诗（中文版 feat. 重音テト）》33 行，时间轴与用户桌面 LRC 逐行一致 |
| `scenes/bird.json` | 33 行 / 367s / 22020 帧 / 1920x1080 |

相机 `tuneOrbit`：`start_yaw = π/2 - drift/2`、`speed = drift/duration`（默认 drift 0.6）——长时间场景不会再转到文字背面。

### 5.4 bloom 可读性量化（`js/bloom-sweep.mjs`）
指标 = 文字行扫描线亮度 std，越高笔画越分明：nopost 109.7 / **bloom 0.06 → 57.8** / 0.1@0.7 → 42.5 / gift 的 0.3 → 15.9（糊）。**歌词场景统一取 0.06。**

### 5.5 本地 Web 编辑器（②GUI）
- `js/editor.js`：loopback HTTP server，静态路由 + `/api/scenes|scene|export|render`（渲染为 SSE 进度流）。
- `js/editor.html`：三栏（LRC 表单 / 预览 iframe + 时间轴 / 场景 JSON），保存后自动 export 并 reload 预览。
- `js/editor-check.mjs`：无头 Edge 断言 glyph 数、探针时间从场景首条 cue 自动推导、收集 pageerror。
- 当前状态：`no page errors`，33 行 502 glyphs 握手通过。

### 5.6 音频
- `chamina-render --audio <file>` / `render.js --audio <file>`：单 pass 混流（`-map 0:v -map 1:a`、`apad`+`-shortest` 保证时长 = 视频）。
- `js/mux-audio.mjs <video> <audio> [-o out] [--offset sec]`：**无需重渲**，视频 `-c:v copy`、音频 AAC 192k。正 offset = 裁掉音频开头（更早），负 = `adelay` 延后。
- 验证：`sine` 测试音轨混流成功；人声起点实测在 **16s**（RMS 0.038 → 0.146），与 LRC 首行 `00:16.575` 吻合。

### 5.7 成片
```
node js/mux-audio.mjs target/chamina/bird_video.mp4 鸟之诗.mp3 -o out/鸟之诗.mp4
```
- `target/chamina/bird_video.mp4`：22020 帧全片渲染，3.5 fps，136 分钟，100.0 MB，无 page error。
- **`out/鸟之诗.mp4`：6:07.00，1920x1080 60fps + AAC 192k 立体声，108.8 MB。**
- 抽帧复核 t=18 / 180 / 350：首行入场、镜头正中、末句 `keep-open` 长驻均正常。

### 5.8 开场标题卡
- 头 16.575s（首句之前）画面只有粒子，读起来像卡住 → 加标题卡。
- `js/lrc.js` 新增 `buildTitleBlock(opts)`（固定时刻淡入淡出，与歌词块同结构）与 `visibleWidth(scene)`（从 `lrc2scene.mjs` 上移共享）。
- `lrc2scene.mjs` 新增 `--title / --subtitle / --title-in / --title-out / --title-size`；
  `--title-out` 默认 = 首句 cue − 1.2s，保证标题在第一句落定前完全消失。
- 当前 `scenes/bird.json`：35 个 text（33 歌词 + 标题 + 副标题），标题 1.2s→15.375s。
- 编辑器同样支持：`js/editor.html` 加「标题/副标题」输入，`btnLyrics` 里组卡片，并**补上了此前缺失的 `fitWidth`**。

### 5.9 测试
| 侧 | 内容 | 结果 |
| --- | --- | --- |
| Rust | `src/export.rs` 新增 `mod tests` 5 例：`fit_width` 放大/不缩放/跟随 mesh scale/负预算禁用/无 fit 时原样 | `cargo test` **14 + 1 doc**，`clippy --all-targets` 0 警告 |
| JS | `js/lrc.test.mjs` 12 例（node 内置 test runner，零依赖），`npm test` = `node --test lrc.test.mjs` | **12 pass / 0 fail** |
| 端到端 | `node js/editor-check.mjs` | `no page errors`，522 glyphs，探针在首句前 1.5s 命中标题卡 |

**修掉的隐患**：`fit_width: Some(0.0)` 或负值会把 `size` 乘成 0（或负数，网格镜像）——`export::build` 现在与 builder 一致，非正值直接忽略。

### 5.10 渲染选项
- `render.js --gpu`：D3D11 硬件加速开关。实测本机 AMD 核显 **4.6 fps，反而慢于 SwiftShader 的 5.7 fps**（readback 开销），默认保持 SwiftShader。

### 5.11 编辑器配乐
| 端点 | 作用 |
| --- | --- |
| `GET /api/audio` | 列出仓库根目录 + `music/` 下的 `mp3/wav/m4a/aac/flac/ogg/opus` |
| `POST /api/mux?name=<场景>&audio=<相对路径>` | 对已渲染的 `out/<场景>.mp4` 调 `mux-audio.mjs` → `out/<场景>_music.mp4`，**不重渲** |
| `GET /api/render?...&audio=` | 渲染时直接 `--audio` 混流（校验写在 SSE 报头之前，非法路径仍是普通 500） |

- 服务端 `audioPath()` 用 `path.resolve` + `startsWith(ROOT)` 拒绝越权路径。
- UI：头部加 `<select id="audioSel">` + 「合成音频」按钮，boot 时 `refreshAudio()`。
- `editor-check.mjs` 增加 `/api/audio` 非空断言。

### 5.12 全量 cue 校验 `render.js --probe`
```bash
node js/render.js --data target/chamina/scene_data.json --probe cues
# 或 cues@<秒> 指定偏移 / 1.5,20.2,… 指定时点
```
一次浏览器会话遍历 `texts[]`，把时间轴停在每条 cue 的**点亮窗口中点**（不早于 cue+1.5s），读 `__PROBE__.visible` 与该行非空字符数比对；**BLANK 或可见数不足一半都算失败，exit 1**。
`scenes/bird.json` 实跑：**35/35 全部点亮，exit 0**（标题卡与副标题共用 cue 窗口，故首两行显示 20/3、20/17 属正常）。

### 5.13 新动效：相机运动 + 文字抖动（2D 文字动效）

**文字侧**（`src/animation.rs` 权威公式 + `js/anim.mjs` 镜像，`AnimState` 新增 `scale` 乘性因子）：

| 类型 | JSON | 说明 |
|---|---|---|
| `jitter` | `{amplitude:[x,y,z], frequency, rotation, phase, start_at, stagger, ramp}` | 连续手持抖动，**永不停息**；噪声用「两条失谐正弦」而非哈希——`f32::sin` 与 `Math.sin` 只差舍入，哈希则第一帧就分叉。`ramp` 让字在 cue 处平滑起振而不是横跳 |
| `pop` | `{duration, stagger, start_at, from, ease}` | 缩放弹入，`back_out` 冲过 1 再回落 = 2D 弹跳；`from:0` 即零尺寸起跳，`st.scale>1e-3` 时才渲染 |
| `wave` | `{amplitude, frequency, spacing, start_at, ramp}` | 卡拉OK式逐字波浪，第 `idx` 个字落后 `idx*spacing` 弧度 |

`eval_all` 的合成顺序变为：**offset 相加、rotation 相乘、scale 相乘、alpha 相乘**。

**相机侧**（只有 JS 实现，`js/scene.js::updateCamera`；Rust 只带描述）：
- `OrbitCam.movement: Option<CameraMove>` —— `{at, duration, ease, radius_end, elevation_end, target_end}`，把推拉/俯仰/平移叠加在轨道上，可只设其中一两项。
- `OrbitCam.shake: Option<CameraShake>` —— `{amplitude, frequency, roll, at, until, ramp}`，**把相机和注视点沿自身 right/up 轴一起平移**（= 屏幕空间抖动，与 yaw 朝向无关），再叠一点 `rotateZ` 滚转。
- 两个字段都是 `Option` + `#[serde(default)]`：**旧场景 JSON 原样可解析**（`scene.rs::tests` 有专门断言）。

**架构修正：抽出 `js/anim.mjs`**。原先 `clamp01/EASE/fallOffset/fadeFactor/evalAnim/evalAll` 内联在 `scene.js` 里，浏览器外无法测试；现在纯函数进 `anim.mjs`，`scene.js` 只负责接到 Three.js 上，`js/anim.test.mjs` 在 node 里跑**与 Rust 单测一一对应**的断言。

立刻抓到两个只有双端测试才能抓的 bug：
1. `Math.SQRT_2` —— JS 里正确写法是 **`Math.SQRT2`**，写错得到 `undefined` → 整条 y 轴 `NaN` → 字全飞了。Rust 侧 `f32::consts::SQRT_2` 是对的，单看一边永远发现不了。
2. `editor.js::serveStatic` 是**白名单路由**，没登记 `/anim.mjs` → 预览 iframe import 失败、永远报不出 "scene built"。

**`lrc2scene.mjs` 新增开关**（全部默认关闭 → 重新生成既有场景字节不变）：
```
--entrance pop   --jitter <amp> [--jitter-rot <rad>] [--jitter-freq <hz>]
--wave <amp>     [--wave-freq <hz>] [--wave-spacing <r>]
--dolly <u> [--crane <rad>] [--move-at <sec>] [--move-dur <sec>]
--shake <amp>    [--shake-roll <rad>] [--shake-freq <hz>]
```
`lrc.js` 对应加了 `buildJitter/buildWave` 工具；`buildTitleBlock` 也支持 `entrance: 'pop'`。
`visibleWidth()` 现在会看 `movement.radius_end`：**推近会收窄画幅**，`fit_width` 必须按最窄的一刻算，否则开局能装下的长句后半段会出框。

**验证**（`scenes/effects.json` 演示场景，68s / 14 行 / 全开）：
- `cargo test` **21 + 1 doc**、`cargo clippy --all-targets` 0 警告、`cargo fmt` 已格式化。
- `node --test js/anim.test.mjs js/lrc.test.mjs` → **22 pass / 0 fail**。
- **隔离实验**：`yaw-drift 0`（轨道锁死）+ `noparticles=1`（粒子冻结），渲 t=8.0 与 t=8.05 两帧比字节：
  - 开 `--jitter --wave`：**177973 字节不同**
  - 关掉（对照组）：**0 字节不同** → 文本动效确实在动，且静止时完全确定性。
- **相机实验**（`yaw-drift 0`）：t=1.00 → t=1.05 相机 `[-0.043,2.222,13.971] → [-0.007,2.269,13.933]`，轨道锁死下唯一来源就是 `shake`；t=0 `z=13.943`(r=14) → t=29 `z=11.011 / y=1.569`，与 `radius_end=11`、`elevation_end=0.05` 手算吻合 → dolly + crane 生效。
- `node js/editor-check.mjs` → `no page errors`，`preview ready: 116 glyphs`。
- 演示片段 `target/chamina/effects_demo.mp4`（1200 帧 = 20s，`--from 0`）。

### 5.14 动效落进 MV：`scenes/bird.json` v3 + outro 收尾卡

**音轨实测**（决定尾部怎么处理）：`silencedetect` 全曲无静音段，分段 `volumedetect` 均值
t=0…300 都在 −12…−14 dB，**t=330 段 −19.6 dB** → 尾奏在淡出但全程有音乐。
可 LRC 最后一cue 只到 **287.669s**，配 367s 的音轨，`--keep-open` 会让**末句整整挂 79 秒**。
这 79s 空档就是 outro 卡的用武之地。

**v3 场景参数**（在既有命令上追加，`--entrance` 保持 `slide` 不动已定稿的观感）：
```
--jitter 0.035 --jitter-rot 0.01 --jitter-freq 3
--wave 0.09 --wave-freq 1.6 --wave-spacing 0.55
--dolly 12.6 --crane 0.22 --move-at 45 --move-dur 275
--shake 0.03 --shake-roll 0.006 --shake-freq 3
--outro-at 318
```
选值的量纲感：`jitter 0.035` ≈ 字高的 2%（呼吸感，不是抖屏）；`wave 0.09` ≈ 画面高的 1.4%
（≈15px，看得出跳但不抢词）；`shake 0.03` ≈ 画面高的 0.46%（手持味）；
`crane 0.09 → 0.22` 是**缓缓升起**（5°→12.6°），和《鸟之诗》的飞行意象同向。

**`--outro-at <sec>` 新开关**（`lrc2scene.mjs`）：
- 末句 `fade_out_at` 被改写成 `outro_at − 0.6`（`keep-open` 删除的淡出会重新装回去），
  即**末句主动把画面交给卡片**，而不是一直挂到片尾。
- 追加两张卡：主卡复用 `--title`（`--outro-text` 可覆盖）、副卡复用 `--subtitle`，
  `startAt = outro_at` / `outro_at + 0.35`，`fadeOut` 放慢到 1.4s 收尾。
- **不带 `--outro-at` 时输出逐字节不变** —— 回归实测：35 条 texts、末句 `fade_out_at === undefined`，
  与加开关之前的状态完全一致。`node --test lrc+anim` **22/22**。

**上片验证**：
- `fit_width` 全部 35 条自动收窄 **19.7300 → 17.7570**（dolly 14→12.6 的 10%），
  但 `size` 一条都没变 → 说明预算从不触顶，只是上限更紧，**不会出框**。
- 关粒子/辉光渲 t=30 纯字幕 bbox `x[346,1795]` = **1449px / 1920 = 75.5%**，在 85% 预算内。
- `--probe cues` **37/37 全部点亮**（原 35 + outro 两卡）；末句 `fade_out_at = 317.4` ✓。
- 抽帧：t=300 末句仍在、t=330/335 outro 卡已就位、t=364 正在淡出。

**v3 已出片**：`target/chamina/bird_v3.mp4` —— 22020 帧 / **3.52 fps / 6264s（104 分钟）/ 164.5 MB / 零 page error**。
（v2 是 4.0 fps / 92 分钟 / 100.7 MB：逐字 jitter+wave 让帧间可预测性变差，
 同样 CRF18 下码率涨了 64%，属预期，不是编码配置问题。）
`node js/mux-audio.mjs target/chamina/bird_v3.mp4 鸟之诗.mp3 -o out/鸟之诗-v3.mp4`
→ **`out/鸟之诗-v3.mp4` 173.2 MB**，`ffprobe`：**00:06:07.00 / 1920x1080 60fps h264 + AAC 48kHz stereo 192k**，
首 30s `mean_volume −18.2 dB` 与源 mp3 完全一致（确认音轨真的混进去了），
抽帧 t=120（歌词）、t=330（outro 卡）画面正常。

### 5.15 背景时间线 + 后处理 FX 机架（「华丽」改造）

用户要求：**「请设计的十分华丽，背景可以实时切换，可以搜索文字 PV 高级技巧」**。
先检索了 kinetic typography / lyric video 的常用手法（beat sync、chromatic aberration、
vignette+grain、闪白切分、light rays、glitch 等），据此落成两条能力。

**A. 背景时间线（`Scene.backgrounds: Vec<BackgroundKey>`）**

- 数据：`{at, fade, top, bottom, accent, nebula, stars, drift}`，builder `background_key()`。
  `#[serde(default)]` 在 **容器级** → 旧 JSON 缺字段照常解析（`src/scene.rs::tests` 有专门断言）。
- 求值只在 JS：`js/anim.mjs::evalBackgrounds(keys, t)` —— 取最后一个 `at <= t`，
  与前一项按 `fade` 做 `smoothstep` 交叉淡入；`fade <= 0` 即硬切；返回数组是拷贝。
- 渲染：`js/scene.js::buildBackground` 建一个 `SphereGeometry(300, 48, 32)` + `BackSide` 的
  **skydome**。不能用屏幕 quad：`BokehPass` 靠 `scene.overrideMaterial` 渲深度，屏幕 quad 会写错深度；
  半径 300 < `camera.far = 500`。片元 = 垂直渐变 + `fbm` 星云 + 地平线 accent 光带 + hash 星点闪烁。
  颜色用 `setRGB(..., THREE.SRGBColorSpace)` 按 sRGB 线性化一次，与 `THREE.Color` 读 hex 一致。
- CLI：`--bg-look <name>`（8 种预设 `night/void/nebula/aurora/ember/ice/dawn/bloom`）、
  `--bg-cut <t>:<name>`（可重复）、`--bg-fade <sec>`。两个开关都不给 → `backgrounds` 不写进 JSON，
  **输出与加开关前逐字节一致**（回归已验）。

**B. FX 机架（`Scene.fx: FxConfig`）**

- 字段 `{enabled, chromatic, vignette, saturation, flash, punch, hit_duration}`，
  默认全关、`FxConfig::on()` 全开。
- 单个 `ShaderPass`（`makeFxPass`）插在 **FXAA 之后、链尾**：边缘加权色差 + cue 尖峰、
  饱和度、暗角、闪白。`ShaderPass` 会 clone uniforms → 必须从 `pass.material.uniforms` 取句柄。
- cue 冲击：`anim.mjs::hitEnvelope(cues, t, dur)` = `(1-u)²` 衰减，多 cue 取最大、不叠加超 1；
  `updateCamera` 存 `state.lastHit` 并沿视线轴按 `fx.punch * hit` 推进相机
  （= 每句歌词落定时镜头「顶」一下）；`setFrame` → `applyBackdrop(t)` 每帧推背景与 FX uniforms。
- CLI：`--fx` + `--fx-flash/--fx-punch/--fx-vignette/--fx-chromatic/--fx-sat`。**`--fx-grain` 已删除**（见下）。

**C. 性能：SwiftShader 上「逐像素噪声」是天坑**

本机无可用 GPU，渲染走 SwiftShader（软件光栅）。逐步二分，每组渲 60 帧取 `frame 60/60`：

| 配置 | fps |
|---|---|
| 无 bg 无 fx（v3 等价基线） | **4.09** |
| + 背景 skydome | 2.27 |
| + fx（色差/暗角/饱和/闪白） | 2.37 |
| + 逐像素 grain（`fract(sin(dot()))`） | **0.86** |
| + grain 改成噪声纹理（第 4 个 sampler） | 0.89 |
| 纹理 grain、再把 `uTime` 拿掉 | 0.89 |
| 只留 `col += uGrain * 0.5`（纯标量） | 2.27 |

结论：**一个逐像素噪声值 ≈ 0.63 s/帧**（22020 帧多花 3.8 小时），
而 3 次纹理采样 + 暗角/饱和等常规算术几乎零成本。
A/B…N 共十余组对照才定位：**逐像素 `sin`/`fract` 随机、以及「第 4 个 sampler」都会让 SwiftShader 掉出快路径**
（同一套 `fract(sin)` 放在 skydome 里只值 0.19 s/帧，位置不同代价不同，无法靠推理预判）。

→ 处置：**`grain` 从 `FxConfig`、CLI、着色器三处整体删除**；背景 `fbm` 5 阶 → 2 阶、去掉 `asin`，
背景开销 0.19 → 0.10 s/帧，最终 **2.85 fps ≈ 129 分钟**，可接受。

**D. 上片验证（静帧数值级）**

- 四个时点的天顶 RGB：night `[1,4,26]` / ice `[7,27,36]` / ember `[48,7,1]` / bloom `[57,1,39]`
  → 六段调色**逐段换色**，切换确凿发生。
- 暗角：t=205 角 `[48,7,1]` vs 中上 `[81,15,2]`（角暗 ~40%）；关 fx 同区域 `[121,28,6]`
  → 角差 **−48 / −13 / −4**。
- 闪白：t=16.575（cue 命中）角 `[46,48,66]` vs t=16.95（衰减完）`[1,3,24]` → **+45/+45/+42**，近白叠加。
- 镜头 punch：`probe` 相机 `[3.654,2.252,13.147]` → `[3.749,2.260,13.429]`，
  离目标距离 13.70 → 14.00，正好 **0.30 单位 = `fx.punch`** ✓。
- 回归：`cargo test` **25 + 1 doc**、`cargo clippy --all-targets` **0 警告**、`cargo fmt` 已格式化；
  `node --test js/anim.test.mjs js/lrc.test.mjs` **30 pass / 0 fail**；
  `node js/editor-check.mjs` → `no page errors`，542 glyphs。

**E. 顺手修掉的编码损坏（重要，后续会话必读）**

PowerShell 5.1 的 `Get-Content`（**不带 `-Encoding`**）会把无 BOM 的 UTF-8 按 GBK 解码，
再 `Set-Content -Encoding UTF8` 写回 → `—`/`→`/`≈`/`±` 变成 `鈥?`/`閳?`/`璺?`/`闂?`。
本轮修复 `src/animation.rs`、`src/scene.rs`、`js/lrc2scene.mjs`（含 `--help` 首行）、`js/scene.js`
共 15 处，并扫描全部源文件确认零残留；同一函数还修出 `--help` 里 `titleOut` 取错变量的显示 bug。
**以后一律用 read/edit/write 工具，或 `Get-Content -Encoding UTF8`。**

### 5.16 PV 脚本落地 A+B：33 段配色时间线 + 每句独立入场（`scenes/bird.json` v5）

外部 PV 脚本按句给了 `V:背景 / E:文字动效 / P:粒子` 三轨。对账结论：
**V 能换色不能换景**（skydome 只有渐变+星云+地平线光带+星点，没有具象画面）、
**E 只有 4 种且原本是全局的**、**P 完全没有粒子系统**。
本轮只做 A（背景）与 B（入场），粒子/弧形排版/运动模糊留待后续。

**A. 33 段配色 —— `--bg-cut` 支持内联 hex 调色板**

`js/lrc2scene.mjs::parseCut`（原 `buildBackgrounds` 里的内联解析拆出来）：

```
--bg-cut 24.152:aurora                                  # 命名 look（原样支持）
--bg-cut 24.152:#00303a/#01121a/#6cffd0@0.75,0.7,1.1    # top/bottom/accent@nebula,stars,drift
```

- 行尾 ` # 注释` 会先剥掉（hex 前不会有空白，`search(/\s#/)` 判据安全）。
- accent 自带滑杆，所以 `split('/')` 只有 3 段，`parts[2].split('@')` 再拆滑杆。
- 校验 `#rrggbb`，滑杆 `RATIO()` 夹到 0..1 / 0..1 / 0..4。
- `evalBackgrounds` 本就是线性扫 key，**34 个 key（opener night + 33 切点）无上限**。

配色脚本单独存 `scenes/bird.bgcuts`（一行一个切点 + 中文注释），由调用方展开成
`--bg-cut` 参数 —— 不新增 CLI 旗标，文件仍是唯一可 diff 的真源。

**B. 每句独立入场 —— LRC 行内 `{e:…}` 标签**

```
[00:16.575]{e:pop}远离那些繁华悲伤 我们从此告别了过往
[00:11.246]{e:none}天空里早已留下了 永恒的神话
```

- `js/lrc.js::splitTags` + `TAG_RE`：大括号在 LRC 里不可能是歌词内容，两套语法不冲突。
- 只有真带标签的行才写 `tags` 键 → **无标签的场景输出逐字节不变**（`applyOffset` 同理）。
- `lineEntrance()` 解析每句，未知 mode **直接抛错**而不是静默降级（一句 PV 走错动效要能被发现）。
- **⚠ 踩过的坑**：`lrc2scene.mjs::main` 里 `applyOffset(parsed).map(l => ({time, text}))`
  会把 `tags` 丢掉，标签解析了却用不上。**新增行级字段必须同时补这条 map。**

**C. 反推原命令（值得复用的手法）**

场景 JSON 里全量留着生成参数，倒推 `lrc2scene` 命令后，用
「无标签 LRC + 原 6 切点 → 生成 → 与 `bird.v4.bak.json` 逐字节比对」自证：
**BYTE-IDENTICAL ✓**。反推时漏了 `--keep-open`，表现为末行 fade 对象的
`fade_out` / `fade_out_at` **键序**不同（值完全一样）—— 这就是 keepOpen 删键后重加的痕迹。

**D. 验证（全过）**

- `evalBackgrounds` 逐切点取稳定时刻（切点 + 2.4s，密间距取下一刀前 0.1s）：
  **33/33 的 top/bottom/accent 与手写调色板逐字节一致**。
- 入场分布：`pop 12 / slide 10 / none 6 / fall 5`，标题卡与尾卡仍 `slide`；
  渲染文本零 `{}` 泄漏。
- `node --test js/anim.test.mjs js/lrc.test.mjs` **37 pass / 0 fail**（新增 7 条标签测试）；
  `cargo test` **25 + 1**、`cargo clippy --all-targets` **0 警告**、`cargo fmt --check` 干净。
- `render.js --probe cues` → **probe ok — every cue is lit**（37/37）。
- 成片抽帧天空色：t=19 / 102.5 / 185.8 / 266.5 / 290.1 → **5 个样本 5 个不同**；
  其中 t=266.5 全 `[0,0,0]` 正是 `#000000` 那把调色板，数据→像素直接对上。

**E. 复现命令（v5）**

```
$cuts = Get-Content scenes/bird.bgcuts -Encoding UTF8 | ? { $_.Trim() -and -not $_.Trim().StartsWith('#') }
$a = @('scenes/bird.lrc','-o','scenes/bird.json',
  '--size','1.7','--y','-0.3','--color','#eafcff','--outline','#6bd9ff',
  '--title','鸟之诗','--subtitle','中文版 feat. 重音テト · HANA',
  '--jitter','0.035','--jitter-rot','0.01','--jitter-freq','3','--wave','0.09',
  '--dolly','12.6','--crane','0.22','--move-at','45','--move-dur','275',
  '--shake','0.03','--shake-roll','0.006','--shake-freq','3',
  '--duration','367','--outro-at','318','--keep-open',
  '--bg-look','night','--bg-fade','2',
  '--fx','--fx-flash','0.18','--fx-punch','0.30','--fx-vignette','0.5',
       '--fx-chromatic','0.0014','--fx-sat','1.10')
foreach($c in $cuts){ $a += '--bg-cut'; $a += $c }
node js/lrc2scene.mjs @a
```
`--bg-fade` 从 3 收到 **2**：33 刀之后最近的两刀只隔 2.33 s（278.693 / 281.024），
fade 3 会被下一刀打断而跳变。

## 六、v5 成片已交付 + 下一步

**`out/鸟之诗-v5.mp4` 已交付**（231.7 MB，当前成片）：
00:06:07.00 / 1920x1080 / 60 fps / h264 High yuv420p / AAC-LC 48 kHz stereo **192 kb/s**。
内容 = v4 全部内容 + **33 段逐句配色背景（`--bg-cut` 内联 hex，fade 2 s）** +
**33 句各不相同的入场（`{e:pop/slide/fall/none}` 行内标签）**。
渲染方式同 F 节：8 段 × 3000 帧 + `ffmpeg -f concat -c copy`，一次通过。

**v4 落片验证（数值级，全部通过）**

| t | 参考静帧 `f_<t>.png` 顶点 | 成片 `v4chk_<t>.png` 顶点 | 参考角落 | 成片角落 |
|---|---|---|---|---|
| 10 | `[5,34,0]` | `[4,32,0]` | `[2,18,0]` | `[1,17,0]` |
| 140 | `[23,35,3]` | `[22,34,3]` | `[22,30,5]` | `[20,29,4]` |
| 205 | `[12,0,77]` | `[10,0,77]` | `[4,0,35]` | `[3,0,32]` |
| 330 | `[0,48,71]` | `[0,47,68]` | `[0,30,43]` | `[0,30,43]` |

四点互不相同（背景确实在切），且与新 shader 的参考静帧逐点吻合；角落比顶点明显更暗 = 暗角在位。
`lum.mjs` 看图 t=140：歌词行高亮、四角压暗、背景渐变正常。

**F. 长渲染必须分段（重要，后续会话必读）**

一次性 129 分钟的后台渲染**必死**，试过两条路都被回收：
- `Start-Process` 启动 → 工具的进程树清理把它杀掉（`ffmpeg exited with 3221225786` = `0xC000013A`）。
- `schtasks /create + /run`（脱离 shell）→ 一样 `Last Result = -1073741510`（同为 `0xC000013A`）。
  附带坑：计划任务的 `.cmd` 必须用 **ANSI/GBK 编码**写盘（`[System.Text.Encoding]::Default`），
  否则 `cmd.exe` 按 OEM 936 读 UTF-8 → `cd /d F:\项目\Chamina` 失败、日志文件都建不出来。

**可行做法 = 分段渲染再无损拼接**（本次采用，8 段全部一次通过）：
```
node js/render.js --data … --out target/chamina/v4_p<i>.mp4 --from <i*3000> --frames 3000 --port 880<i>
ffmpeg -y -f concat -safe 0 -i concat.txt -c copy -movflags +faststart bird_v4.mp4
```
- 每段 3000 帧 ≈ 18 分钟，稳稳落在「单次前台调用 ≤ 30 分钟」的已验证窗口内。
- `concat.txt` 里的路径**相对 ffmpeg 的 cwd**（不是 list 文件所在目录），所以要 `workdir` 切到
  `target/chamina` 并只写裸文件名 `v4_p0.mp4`（也顺带避开路径里的非 ASCII）。
- 每段的 x264 参数完全一致（render.js 固定 `-crf/-preset/pix_fmt/scale`），首帧强制 IDR，
  拼接处干净；`-c copy` 不重编码。
- 已知小坑：某段收尾会偶发 `Error writing trailer … Permission denied`（疑似杀软扫文件），
  **删掉该段重跑即可**，不要去动别的。
- 拼接前记得删掉上一次失败残留的 `bird_v5.mp4`（它会占住输出路径）。

**已交付（保留作对照）**
- v6：`out/鸟之诗-v6.mp4`（166.7 MB）—— + 把常驻抖动改成「落词才动」的打击感（§七，**当前成片**）。
- v5：`out/鸟之诗-v5.mp4`（231.7 MB）—— + 33 段逐句配色 + 每句独立入场。
- v4：`out/鸟之诗-v4.mp4`（232.2 MB）—— + 六段背景时间线 + FX 机架。
- v3：`out/鸟之诗-v3.mp4`（173.2 MB / 6:07.00 / AAC 192k）—— jitter/wave/推镜/手持/outro。
- v2：`out/鸟之诗.mp4`（109.4 MB）—— 无动效、无 outro。
- 中间产物：`target/chamina/v6_p0..7.mp4` 与 `v5_p0..7.mp4`、`v4_p0..7.mp4`（分段，可删）、
  `v6chk_*` / `v5chk_*` / `v4chk_*` / `f_*.png`、`bird_v6.mp4` / `bird_v5.mp4` / `bird_v4.mp4`（无音轨母版）、
  `target/chamina/bird.v6.bak.json`（v6 场景备份）、`bird.v4.bak.json`（v4 场景备份，反推命令的比对基准）。

**下一步**
1. 就片对片，讨论 ⑤PV 脚本剩余的 P 轨（粒子）/ ④Test-Debug / ⑥⑦Bug 修复 / ⑨GUI 上传 / ⑩后续更新。
2. **粒子系统**（PV 脚本 P 轨，33 句全缺）—— 必须先跑 2 秒性能样板：
   SwiftShader 对逐像素开销极敏感（§5.15 C 的教训）。
3. 编辑器 UI 还没有背景切换、`{e:…}` 入场编辑与 `--fx` / `--outro-at` 的开关
   （目前只能改 JSON / LRC 表单）。
4. 弧形排版 / 逐字高亮 / glitch —— PV 脚本 E 轨里 `scene.js` 目前做不到的部分。
5. 粒子按行配色、line2/line3 视觉权重（沿用第四节遗留项）。
6. `docs/session-summary.md` 第四节仍停在更早一轮，需与五、六节合并去重。
7. 清理：`target/chamina/pf*.mp4`、`hit_*`、`bgv_*`、`scene.js.bak`、
   `v5_*.png` / `v4chk_*` 等抽帧。

## 七、逐字入场（word timing）—— v7 第 1 步

**输入**：用户给了新 LRC `C:\Users\马文\Downloads\鸟之诗.lrc`，每字一个
`<mm:ss.xx>`，要求「一个一个词地出现」。

**时间轴换源**：33 行歌词逐行比对，文字 33/33 完全一致，但行起点与旧
`scenes/bird.lrc` 差 **-0.59 ~ +0.49 s（均值 +0.18 s）**，不是常数偏移，说明
两份 LRC 是不同的对轴结果。新文件行起点与它自己的逐字戳自洽（`[00:16.54]`
= 第一个 `<00:16.54>`），因此**整份采信新时间轴**：

- `scenes/bird.lrc` ← 新文件正文 **+ 旧行内 `{e:pop/slide/fall/none}` 标签**
  （33/33 按行序合并，合并前先断言两份歌词文本逐行相等）。
- `scenes/bird.bgcuts` 33 个切点按同一逐行 delta 平移（切点与歌词 cue 保持
  1:1 的既有约定），首刀 `16.575 → 16.540`。
- 标题卡出场随之从 `15.375 → 15.34`（`--title-out` 默认 = 首句 - 1.2）。

### 7.1 数据通路（一条数据，四处落地）

`splitGlyphTimes` (`js/lrc.js`)
→ `line.charTimes`（`parseLrc` / `applyOffset` / `lrc2scene::main` 三处都要带，
  否则**解析了却用不上**——老坑的逐字版）
→ `texts[].char_times` (`buildLyricTexts`)
→ `TextSpec.char_times` (Rust 反序列化) → `TextExport.char_times` (序列化)
→ `state.chars[].charTimes` (`scene.js::buildText`)
→ `evalAll(anims, t, idx, size, charTimes)` → `charDelay(...)`

语义：**块级一个 `char_times` 数组**（每字一个绝对时刻），不是塞进某个动画。
因为 fade、入场、抖动三层必须踩同一个节奏；而且 `AnimationSpec` 是 `Copy`，
往动画变体里加 `Vec` 会把整条 Copy 链打断。

### 7.2 语义细节

- 印章管到下一个印章为止 → **一个词共用一个戳**（天然「一个一个词」），
  中间没打戳的空格跟着它前面的词亮；第一个戳之前的文字回退到行 cue。
- `char_delay` 缺戳就回落到 `start_at + idx * stagger` 网格（Rust `char_delay`
  / JS `charDelay`，含「数组比行短」的情况），**无戳场景一个字节都不变**。
- `outAt = max(原值, 最后一字 + 0.15)`，仅在有 `charTimes` 时生效：否则行尾
  会在刚出现时就开始淡出。bird.lrc 实测最紧的一句（`在我们身旁`）仍留 0.21 s
  余量，33 句无一与下一句重叠（`lrc.test.mjs` 有断言）。
- `render.js --probe cues` 的采样点：有戳的行改取**最后一字 + 0.4 s**（夹在
  cue 窗口内），否则按老规矩取窗口中点——否则会抓到「只亮了半行」的瞬间。

### 7.3 ⚠ 踩到的坑

- 只改 `scene.js::buildText` 把 `charTimes` 存进字符、**忘了改 `setFrame` 的
  `evalAll(c.animations, t, c.idx, c.size)`**，跑起来一切正常但逐字节奏完全
  没生效。靠 `--probe 16.6,17.0,17.5,…` 逐时刻数可见字数才看出来（16.6/17.0/
  17.5 = 1/8/15 是**均匀网格**的形状；补上传参后变成 1/2/3 = 与戳完全对齐）。
  → 新增任何动画参数，**必须同时改「求值入口」**，不只是数据入口。

### 7.4 验证（全过）

- `node --test js/anim.test.mjs js/lrc.test.mjs` → **56 pass / 0 fail**（+10）。
- `cargo test` **30 + 1 doc-test**、`cargo clippy --all-targets` 0 警告、
  `cargo fmt --check` 干净。
- **字节回归**：拿无戳的旧 LRC + 旧 bgcuts 跑同一条命令 →
  `85A489BEBB637C71261F96E39DF553E0CCD2110F2DE9D91E9CA07F991ADC411A`，
  与 `target/chamina/bird.v6.bak.json` 完全一致 ✓。
- `cargo build` → `chamina-render scenes/bird.json --dry-run` → 导出的
  `scene_data.json`：37 行里 33 行带 `char_times`，`line0` 18 字 18 戳、
  17 个有轮廓 mesh（空格不发 mesh，但 `CharExport.idx` 是**原字符串下标**，
  所以戳数组必须含空格）。
- `render.js --probe cues` → **probe ok — every cue is lit（37/37）**。
- 逐时刻字数（line0，戳 16.54…22.56）：
  `16.6→1 / 17.0→2 / 17.5→3 / 18.0→5 / 18.5→7 / 19.0→8 / 20.3→9 / 21.0→11 /
  21.5→13 / 22.6→17`，与 LRC 逐字戳一一对上。
- 静帧 t=17.0 / 19.0 / 23.0 → 420 / 515 / 628 KB，文字行灰度 `.` → `=+` →
  `=+==-`（越来越亮，非黑）。
- `node js/editor-check.mjs` → `no page errors`，542 glyphs。

### 7.5 复现命令（v6 基线，同时也是 v7 的生成命令）

反推自 `bird.v6.bak.json`（场景里全量留着生成参数），验证方式 = 用它跑
**旧** LRC/bgcuts 与备份逐字节比对。v7 只需把输入换成现在这版
`scenes/bird.lrc` + `scenes/bird.bgcuts`：

```powershell
$cuts = Get-Content scenes/bird.bgcuts -Encoding UTF8 | ? { $_.Trim() -and -not $_.Trim().StartsWith('#') }
$a = @('scenes/bird.lrc','-o','scenes/bird.json',
  '--size','1.7','--y','-0.3','--color','#eafcff','--outline','#6bd9ff',
  '--title','鸟之诗','--subtitle','中文版 feat. 重音テト · HANA',
  '--jitter','0.035','--jitter-rot','0.01','--jitter-freq','3','--wave','0.09',
  '--dolly','12.6','--crane','0.22','--move-at','45','--move-dur','275',
  '--shake','0.03','--shake-roll','0.006','--shake-freq','3',
  '--shake-hit','--jitter-hit','--wave-hit',
  '--duration','367','--outro-at','318','--keep-open',
  '--bg-look','night','--bg-fade','2',
  '--fx','--fx-flash','0.18','--fx-punch','0.30','--fx-vignette','0.5',
       '--fx-chromatic','0.0014','--fx-sat','1.10')
foreach($c in $cuts){ $a += '--bg-cut'; $a += $c }
node js/lrc2scene.mjs @a
cargo build
D:\ChaminaTarget\x86_64-pc-windows-gnu\debug\chamina-render.exe scenes\bird.json --dry-run
node js/render.js --data target/chamina/scene_data.json --probe cues
```
（无戳基线的 SHA 见 7.4；`target/chamina/v6_src.lrc` / `v6_src.bgcuts` 是
从 `git show HEAD:` 导出的旧输入，留作回归比对。）

### 7.6 下一步（用户已给出的方向）

1. **空间排版 + 逐句运镜**：文字不再挤在同一位置，各句摆到画面不同处，
   相机按句换目标（`OrbitCam` 加 `stops: Vec<CameraMove>` 序列），
   上一句**出画框才淡出**；布局「各种都有」。
2. **入场加张力**：位移过冲 + 缩放过冲（`ENTRANCES` 加新 mode，
   `Ease::BackOut` 已在 `pop` 上验证过）。
3. 粒子云 x∈[-35,35] / y∈[-25,45] / z∈[-42.5,2.5] 是硬编码的，
   排版一拉开就得改由文字 bbox 推导（`js/scene.js:295`）。


