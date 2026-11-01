# chamina — 单头 C++ 音乐可视化成片引擎

**chamina.h** 是整套 Chamina 渲染管线的 C++ 实现：stb 风格单头库，一个 `.cpp`
里 `#define CHAMINA_IMPLEMENTATION` 后 include 即可渲染。仓库另带一个薄壳
命令行（`chamina_cli.cpp`）与最小嵌入示例（`example/main.cpp`）。

`chamina.h` 是生成物：可编辑源码在 `src/`，每个文件不超过 1000 行；
`tools/amalgamate.cpp` 按固定顺序把它们拼成完整的单头文件。构建前会自动
重新生成，**不要直接改 `chamina.h`**。

与此前的 Rust 参考实现功能对等：字体解析、排版、挤出字形网格、
动画求值、相机编排、背景时间线、软件光栅、全套音频可视化、后处理、视频
编码，以及 `gen`（LRC → 场景 JSON）与 `audio`（音轨 → 频谱包络）两个
子命令。不依赖 Rust、Node、浏览器或 GPU。

## 功能面

| 模块 | 内容 |
|---|---|
| 字体引擎 | stb_truetype 轮廓 → 二次曲线展开 → even-odd 孔洞分组 → earcut → 挤出（盖面/侧壁/法线）→ 描边 |
| 排版 | hmtx 步进、光学行居中、fit-width 自动缩放、字级时间戳 |
| 动画 | fall / fade / slide / pop / jitter / wave / spin，逐字符 stagger |
| 相机 | orbit + movement（dolly/crane）+ shake + cue punch + 逐词 stops 轨道（yaw/roll 为偏移量） |
| 背景 | 时间线关键帧交叉淡化（top/bottom/accent 三色板）+ skydome（fbm 星云 / 星点 / 地平线 / 漂移） |
| 光栅 | 三角形深度 + alpha、加性圆点精灵、roll 烘进视基 |
| 可视化 | 彩虹频谱环 ×2、3D 频谱条、落词火花、甩镜风线（需要音频包络） |
| 后处理 | bloom（UnrealBloom 同构）→ DOF → ACES + sRGB → grade（色差/暗角/饱和/闪白） |
| 管道 | ffmpeg rawvideo rgb24 → libx264；静帧走 stb_image_write（不需要 ffmpeg） |
| gen | LRC 解析 + 两种布局（单行 / field 蛇形世界位），与 node 版输出字节级一致 |
| audio | ffmpeg 解码 + radix-2 FFT → 频谱包络 JSON，与 node 版字节级一致 |
| SIMD | AVX2 运行时分发（tonemap / grade / skydome / 云场 / atan2f 多项式）；`CHAMINA_NO_SIMD=1` 强制标量 |

## 构建

Windows + MinGW-w64（已验证）：

```bat
build.bat        → 生成 chamina.h → dist\chamina-cpp.exe（静态链接，无运行时 DLL）
```

`build.bat` 先编译 `tools/amalgamate.cpp`，用它把 `src/*.cpp` 拼成
`chamina.h`，再编译 CLI。任何 chunk 超过 1000 行都会直接报错。

手动构建：

```bat
g++ -O2 -std=c++17 tools/amalgamate.cpp -o dist\amalgamate.exe
dist\amalgamate.exe
g++ -O2 -std=c++17 -fopenmp -static -static-libgcc -static-libstdc++ ^
    -Ivendor chamina_cli.cpp -o dist\chamina-cpp.exe -lshell32
```

- 依赖全部在 `vendor/`：nlohmann/json、stb_truetype、mapbox/earcut.hpp、
  stb_image_write。无包管理器。
- `-fopenmp` 可选（多线程光栅；不加则单线程，同样可编）。
- Windows 链接需要 `-lshell32`。
- 视频编码需要 ffmpeg：`PATH` 上的 `ffmpeg`，或环境变量
  `CHAMINA_FFMPEG` 指向完整路径。静帧不需要 ffmpeg。

## 快速开始

```bat
:: 1) 歌词 → 场景 JSON（examples/gift.json 为模板）
dist\chamina-cpp.exe gen examples\sample.lrc -o out\scene.json -t examples\gift.json

:: 2) 音轨 → 频谱包络（可选，有音乐才有频谱环/条/火花）
dist\chamina-cpp.exe audio song.mp3 -o out\audio_env.json

:: 3) 出片（--audio-env 可省；--frames 0 = 到片尾）
dist\chamina-cpp.exe out\scene.json --audio-env out\audio_env.json -o out\video.mp4

:: 静帧抽查（不需要 ffmpeg）
dist\chamina-cpp.exe out\scene.json --still 8 --png out\check.png
```

## CLI 用法

```
chamina-cpp <scene.json> [--audio-env f] -o out.mp4 [--from n] [--frames n]
chamina-cpp <scene.json> --still t --png f
chamina-cpp gen <song.lrc> -o scene.json [options]
chamina-cpp audio <audio-file> -o audio_env.json [--fps 60 --bands 64 --lo 40 --hi 14000]
```

`gen` 的完整参数组（`--help` 为权威清单）：

| 组 | 参数 |
|---|---|
| 文本 | `--size --y --depth --color --strength --outline --outline-width` |
| 运动 | `--entrance --jitter --jitter-rot --jitter-freq --jitter-hit --jitter-hit-dur --wave --wave-freq --wave-spacing --wave-hit --wave-hit-dur` |
| 相机 | `--dolly --crane --move-at --move-dur --shake --shake-roll --shake-freq --shake-hit --shake-hit-dur --yaw-drift --radius --keep-camera` |
| 时间 | `--offset --fade-in --fade-out --stagger --tail --duration --fit-width --keep-open` |
| 卡片 | `--title --subtitle --title-in --title-out --title-size --outro-at --outro-text --outro-subtitle --outro-wish --outro-out` |
| 舞台 | `--bg-look --bg-cut --bg-fade --fx --fx-flash --fx-punch --fx-vignette --fx-chromatic --fx-sat --bloom --fps --palette` |
| field 布局 | `--layout field --groups auto --roll-power --force-whip --energy-file` |

## 当库用

```cpp
#define CHAMINA_IMPLEMENTATION   // 恰好一个 .cpp
#include "chamina.h"

int main() {
    std::string scene, env;
    chamina::read_file("examples/bird.json", scene);   // UTF-8，中文路径安全
    chamina::read_file("out/audio_env.json", env);     // 可省：无音频组件
    char err[512];
    auto* e = chamina::engine_create(scene.c_str(), env.c_str(), err, sizeof err);
    if (!e) { fprintf(stderr, "error: %s\n", err); return 1; }

    std::vector<unsigned char> rgb((size_t)chamina::engine_width(e) *
                                   chamina::engine_height(e) * 3);
    chamina::engine_render_rgb(e, 8.0f, rgb.data());   // RGB24 直出
    chamina::engine_still_png(e, 20.35f, "frame.png");
    chamina::engine_encode_video(e, 0, 0, "out.mp4");  // frames=0 到片尾
    chamina::engine_destroy(e);
}
```

- 其余翻译单元只 include 头文件用 API（已验证可编）。
- `engine_create` 不抛异常：失败返回 `nullptr` 并填 `err`。
- 帧缓冲是函数级 static（与原引擎一致），跨引擎实例不线程安全。
- 完整 API：`engine_create / engine_destroy / engine_width / engine_height /
  engine_fps / engine_total_frames / engine_render_rgb / engine_still_png /
  engine_encode_video / read_file / set_trace / gen_main / audio_main`。
- 最小可编译示例见 `example/main.cpp`。

## 运行时开关（环境变量）

| 变量 | 作用 |
|---|---|
| `CPP_TRACE=1` | 每帧打印相机状态与各文本块状态（求值诊断） |
| `CPP_NOVIZ=1` | 关闭音频可视化组件（隔离文字/背景问题） |
| `CPP_PROF=1` | 渲染结束打印 render/finish 每帧毫秒数（`[prof]` 行） |
| `CPP_SKIP_CLOUD=1` / `CPP_SKIP_STARS=1` | 分别关闭星云 / 星点（排查 skydome） |
| `CHAMINA_NO_SIMD=1` | 强制全标量路径（与 SIMD 版对照排错） |
| `CHAMINA_FFMPEG` | ffmpeg 完整路径（默认找 PATH） |

## 性能

开发机（Windows / MinGW-w64 g++ 15.2 / 24 线程）实测，1920×1080：

| 口径 | 数值 |
|---|---|
| render（fx-on，含 skydome） | **28.7 ms/帧** |
| └ skydome 子段 | 15.9 ms/帧 |
| finish（bloom/DOF/ACES/grade/pack） | 47.5 ms/帧 |
| 视频整段（60 帧交替配对） | **8.3 fps** |

SIMD 说明：AVX2 内核在运行时按 CPU 能力分发（`__builtin_cpu_supports`），
数值政策分两级——位精确内核（tonemap/grade/skydome 全幅/云场 prepass，
与标量逐字节一致）与近似内核（atan2f 向量多项式，region 门禁 ±0.05）。
`CHAMINA_NO_SIMD=1` 可随时退回纯标量。

## 仓库结构

```
chamina.h            生成物：完整单头库（stb 风格，实现 TU 定义 CHAMINA_IMPLEMENTATION）
src/                 可编辑源码分片（00_api.h + 01..10 *.cpp，每片 ≤1000 行）
tools/amalgamate.cpp 单头文件生成器（顺序拼接 + 行数门禁）
chamina_cli.cpp      CLI 薄壳（渲染 / gen / audio）
example/main.cpp     最小嵌入示例
examples/            gift.json 模板、sample.lrc / bird.lrc 歌词、bird.json（gen 产物参考）
vendor/              四个 vendored 单头依赖
build.bat            一键构建（先生成 chamina.h，再编译）
README.md            本文件
TUTORIAL.md          从零出片教程（歌词 → 场景 → 视频）
PROMPT.md            给 AI 的成片提示词（把需求直接变成 chamina 视频）
```
