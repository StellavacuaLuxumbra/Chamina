# chamina-cpp — 独立 C++ 渲染引擎（单头库）

整个 Chamina 的 C++ 实现：**字体解析、排版、挤出网格、动画求值、相机编排、
软件光栅、全套音频可视化、后处理、视频编码管道，以及 LRC → 场景 JSON
（`gen`）与音轨 → 频谱包络（`audio`）两个子命令**。

交付形态是 **stb 风格单头库 `src/chamina.h`**（约 4800 行）+ 四个 vendored
单头库。宿主 C++ 工程在一个 `.cpp` 里 `#define CHAMINA_IMPLEMENTATION`
后 include 即可渲染；`src/chamina_cli.cpp` 是库之上的薄壳命令行（本目录的
构建产物）。不依赖 Rust、不依赖 Node、不依赖浏览器、不依赖 GPU——同一份
场景 JSON，与旧管线逐位等价（验收比值 0.97–1.03，详见
`docs/cpp-migration.md` §八；拆库回归见 §十三）。

## 构建

```bat
cpp\build.bat          → dist\chamina-cpp.exe（CLI 薄壳，静态链接，无运行时 DLL）
```

要求：MinGW-w64 g++（仓库既有 D:\mingw64）。依赖全部在 `vendor/`：
nlohmann/json、stb_truetype、mapbox/earcut.hpp、stb_image_write。

## 当库用（嵌入宿主工程）

```cpp
#define CHAMINA_IMPLEMENTATION   // 恰好一个 .cpp
#include "chamina.h"

int main() {
    std::string scene, env;
    chamina::read_file("scenes/bird.json", scene);            // UTF-8，中文路径 OK
    chamina::read_file("target/chamina/audio_env.json", env); // 可省：无音频组件
    char err[512];
    auto* e = chamina::engine_create(scene.c_str(), env.c_str(), err, sizeof err);
    if (!e) { fprintf(stderr, "error: %s\n", err); return 1; }

    // RGB24 直出（w*h*3 字节，喂给自家的纹理/表面）
    std::vector<unsigned char> rgb((size_t)chamina::engine_width(e) *
                                   chamina::engine_height(e) * 3);
    chamina::engine_render_rgb(e, 8.0f, rgb.data());

    chamina::engine_still_png(e, 20.35f, "frame.png");   // 或直接出 PNG
    chamina::engine_encode_video(e, 0, 0, "out.mp4");    // frames=0 到片尾
    chamina::engine_destroy(e);
}
```

- 其余翻译单元 include 头文件只用 API（已验证可编）。
- 实现 TU 建议 `-fopenmp`（多线程光栅；不加则单线程，头文件同样可编）。
- Windows 链接要 `-lshell32`（CommandLineToArgvW）。
- `engine_create` 不抛异常：失败返回 `nullptr` 并填 `err`；场景/包络 JSON
  直接以 UTF-8 字符串传入（读文件用 `chamina::read_file`，中文路径安全）。
- 最小可编译示例：`example/main.cpp`（`dist/chamina-example.exe`，其输出
  与 CLI 静帧逐字节一致）。
- 非 killable 约束：帧缓冲是函数级 static（与原单 TU 引擎一致），跨引擎
  不线程安全；逐帧跟踪 `chamina::set_trace` / `CPP_TRACE=1`。

## CLI 用法（薄壳）

```bat
:: LRC → 场景 JSON（lrc2scene 的 C++ 移植，输出与 node 版字节级一致）
chamina-cpp gen scenes\bird.lrc -o scenes\bird.json
chamina-cpp gen scenes\bird.lrc -o scenes\bird.json ^
  --entrance fall --jitter 0.4 --title "标题" --palette "0xff88cc,#66ddff"

:: field 布局（蛇形世界位 + 每词相机停 + 按音频能量甩镜）——已全量移植
chamina-cpp gen scenes\bird.lrc -o scenes\bird.json ^
  --layout field --groups auto --roll-power 0.7 ^
  --energy-file target\chamina\audio_env.json --force-whip "遥远飞行"

:: 音轨 → 频谱包络（audio-env 的 C++ 移植，输出与 node 版字节级一致）
chamina-cpp audio gift.mp3 -o target\chamina\audio_env.json
chamina-cpp audio gift.mp3 --fps 30 --bands 32 --lo 30 --hi 16000

:: 渲一段视频（rawvideo 管道 → x264，需要 ffmpeg 在 CHAMINA_FFMPEG 或 PATH）
chamina-cpp scenes\bird.json --audio-env target\chamina\audio_env.json -o out.mp4
chamina-cpp scenes\bird.json -o out.mp4 --from 0 --frames 300

:: 单帧静图（PNG，自带 stb_image_write，不需要 ffmpeg）
chamina-cpp scenes\bird.json --still 20.35 --png frame.png
```

输入是 `chamina-cpp gen`（或 `js/lrc2scene.mjs`）产出的场景 JSON（字段在根级）；
`--audio-env` 是 `js/audio-env.mjs` 产出的频谱包络，给了才有频谱环/频谱条/
火花/粒子呼吸（未给时这些组件静默关闭，其余一切照常）。

gen 支持与 node 版**相同且全量**的 CLI（entrance/jitter/wave/shake/
palette/fx/标题副题片尾卡/相机运动/fit-width/offset/duration/fps/tail、
--layout field/--groups auto/--roll-power/--force-whip/--energy-file，
详见 `--help`）；已知偏差（1 ulp 级）见 `docs/cpp-migration.md` §十/§十四。

### 运行时开关（环境变量）

| 变量 | 作用 |
|---|---|
| `CPP_TRACE=1` | 每帧打印相机状态与各文本块状态（求值诊断） |
| `CPP_NOVIZ=1` | 关闭音频可视化组件（隔离文字/背景问题） |
| `CPP_PROF=1` | encode_video 结束时打印渲染/后处理每帧毫秒数 |
| `CHAMINA_FFMPEG` | ffmpeg 完整路径（默认找 js/node_modules 的安装版） |

## 架构速览（源内注释是权威文档）

```
chamina.h
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
├─ gen               LRC 解析（js/lrc.js 直译）→ 场景 JSON（与 node 版字节级一致）
├─ audio             ffmpeg 解码 + radix-2 FFT → 频谱包络 JSON（字节级一致）
├─ 管道              ffmpeg rawvideo rgb24 → libx264（CreateProcessW 直起）；静帧走 stb_image_write
└─ 公共 API          namespace chamina：engine_create / render_rgb / still_png /
                     encode_video / …；CLI 形参入口 gen_main / audio_main
```

## 三条等价通路（互为回归对照）

| 通路 | 位置 | 用途 |
|---|---|---|
| JS + 浏览器 | `js/scene.js` | 作者实时预览、编辑器 |
| Rust | `src/native` + `chamina-render --dry-run` | 与字体库同进程的参考实现 |
| **C++ 单头库** | `cpp/src/chamina.h` | **独立交付的渲染软件**（CLI 与嵌入两用） |

同一帧静图三方区域亮度比见 `docs/cpp-migration.md` §七/§八。
