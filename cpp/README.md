# chamina-cpp — 独立 C++ 渲染引擎

整个 Chamina 的 C++ 实现：**字体解析、排版、挤出网格、动画求值、相机编排、
软件光栅、全套音频可视化、后处理、视频编码管道**，一个约 1900 行的翻译单元
（`src/chamina.cpp`）+ 四个 vendored 单头库。不依赖 Rust、不依赖 Node、
不依赖浏览器、不依赖 GPU——同一份场景 JSON，与旧管线逐位等价
（验收比值 0.97–1.03，详见 `docs/cpp-migration.md` §八）。

## 构建

```bat
cpp\build.bat          → dist\chamina-cpp.exe（静态链接，无运行时 DLL）
```

要求：MinGW-w64 g++（仓库既有 D:\mingw64）。依赖全部在 `vendor/`：
nlohmann/json、stb_truetype、mapbox/earcut.hpp、stb_image_write。

## 用法

```bat
:: 渲一段视频（rawvideo 管道 → x264，需要 ffmpeg 在 CHAMINA_FFMPEG 或 PATH）
chamina-cpp scenes\bird.json --audio-env target\chamina\audio_env.json -o out.mp4
chamina-cpp scenes\bird.json -o out.mp4 --from 0 --frames 300

:: 单帧静图（PNG，自带 stb_image_write，不需要 ffmpeg）
chamina-cpp scenes\bird.json --still 20.35 --png frame.png
```

输入是 `lrc2scene` 产出的场景 JSON（字段在根级）；`--audio-env` 是
`js/audio-env.mjs` 产出的频谱包络，给了才有频谱环/频谱条/火花/粒子呼吸
（未给时这些组件静默关闭，其余一切照常）。

### 运行时开关（环境变量）

| 变量 | 作用 |
|---|---|
| `CPP_TRACE=1` | 每帧打印相机状态与各文本块状态（求值诊断） |
| `CPP_NOVIZ=1` | 关闭音频可视化组件（隔离文字/背景问题） |
| `CHAMINA_FFMPEG` | ffmpeg 完整路径（默认找 js/node_modules 的安装版） |

## 架构速览（源内注释是权威文档）

```
chamina.cpp
├─ 数学/缓动        port of src/math.rs
├─ 动画求值          port of src/animation.rs（fall/fade/slide/pop/jitter/wave/spin + 词级时间戳）
├─ 相机              orbit + movement + stops（yaw/roll 为偏移量）+ shake + cue punch
├─ 背景时间线        34 段调色板交叉淡化 + skydome（fbm 星云/星点/地平线）
├─ 字体引擎          stbtt 轮廓 → 二次曲线展开 → even-odd 孔洞分组 → earcut
│                    → 挤出（盖面/侧壁/法线）→ smooth_hull 描边
├─ 排版              hmtx 步进 / line_center_y 光学居中 / fit_width
├─ 光栅器            三角形（深度+alpha）+ 加性圆点精灵；roll 烘进视基
├─ 可视化            彩虹频谱环×2 / 3D 频谱条 / 落词火花 / 甩镜风线
├─ 后处理            bloom（UnrealBloom 同构）→ DOF → ACES+sRGB → grade
└─ 管道              ffmpeg rawvideo rgb24 → libx264；静帧走 stb_image_write
```

## 三条等价通路（互为回归对照）

| 通路 | 位置 | 用途 |
|---|---|---|
| JS + 浏览器 | `js/scene.js` | 作者实时预览、编辑器 |
| Rust | `src/native` + `chamina-render --dry-run` | 与字体库同进程的参考实现 |
| **C++** | `cpp/src`（本目录） | **独立交付的渲染软件** |

同一帧静图三方区域亮度比见 `docs/cpp-migration.md` §七/§八。
