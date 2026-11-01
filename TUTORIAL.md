# Chamina 从零到成片教程

本教程走完一条完整链路：**准备歌词 → `gen` 生成场景 →（可选）`audio`
生成频谱包络 → 渲染视频 → 抽帧验收 → 手调 JSON**。所有命令都在仓库根目录
执行，`dist\chamina-cpp.exe` 为构建产物（见 §0）。

---

## 0. 环境与构建

**需要**：

- C++17 编译器（Windows 上已验证 MinGW-w64 g++ 15.x）；
- ffmpeg（渲染视频需要；`PATH` 里能直接 `ffmpeg -version`，或设
  `CHAMINA_FFMPEG=完整路径`）。静帧 PNG 不需要 ffmpeg。

```bat
build.bat
```

看到 `[build] smoke ok` 即成功。`build.bat` 会先编译 `tools/amalgamate.cpp`，
把 `src/*.cpp` 拼成 `chamina.h`，再编译 CLI；每个源码分片必须 ≤1000 行。
手动构建：

```bat
g++ -O2 -std=c++17 tools/amalgamate.cpp -o dist\amalgamate.exe
dist\amalgamate.exe
g++ -O2 -std=c++17 -fopenmp -static -static-libgcc -static-libstdc++ ^
    -Ivendor chamina_cli.cpp -o dist\chamina-cpp.exe -lshell32
```

不接参数运行会打印用法（exit 1 属正常）：

```bat
dist\chamina-cpp.exe
```

---

## 1. 准备歌词（LRC）

`gen` 吃标准 LRC：每行 `[分:秒.百分秒] 歌词`，按行推进；末行时间决定
片长（`--tail` 可在尾部留白）。示例：

```
[00:00.00]夜色落在窗台
[00:04.20]风把远方吹来
[00:08.60]我把心事写进星海
```

- 每行 6–14 个汉字最稳（超长行用 `--fit-width` 自动缩）；
- 时间戳从 00:00 起、单调递增；空行不写；
- 仓库自带 `examples/sample.lrc`（短样例）与 `examples/bird.lrc`
  （全功能验证语料，约 5 分钟）。

---

## 2. 生成场景 JSON

```bat
dist\chamina-cpp.exe gen examples\sample.lrc -o out\scene.json -t examples\gift.json
```

`-t` 是**舞台模板**：从模板拷贝分辨率/帧率/字体回退/bloom/dof/相机等
底座，再按参数生成文字与动画。`examples/gift.json` 是一个 7 秒生日卡
模板（1920×1080@60fps、系统微软雅黑回退），可以直接改它当你的模板。

### 2.1 参数组（常用）

**文本**：`--size --y --depth --color --strength --outline --outline-width`
（字号/垂直位置/挤出深度/颜色/霓虹强度/描边）。

**入场与运动**：`--entrance fall|fade|slide|pop`、`--jitter`（随机抖动
幅度）、`--jitter-freq`、`--jitter-hit`（节拍重抖）、`--wave`（波浪）、
`--wave-freq`、`--wave-spacing`、`--wave-hit`。

**相机**：`--radius`（轨道半径）、`--dolly`（推近量）、`--crane`（升镜）、
`--move-at` / `--move-dur`（何时/多久完成运镜）、`--shake`（震动幅度）、
`--shake-freq`、`--shake-hit`、`--yaw-drift`（缓慢偏航）、`--keep-camera`
（完全保留模板相机，不做自动编排）。

**节奏**：`--offset`（整体延迟）、`--fade-in` / `--fade-out`、
`--stagger`（逐字错峰）、`--tail`（片尾留白）、`--duration`（强制总长）。

**卡片**：`--title "标题" --subtitle "副标题"` 开片卡；
`--outro-at 时间 --outro-text "结束语" --outro-wish "祝福"` 片尾卡。

**舞台**：`--palette "0xff88cc,#66ddff"`（背景主色对，十六进制）、
`--bg-look`（背景风格名）、`--bg-cut` / `--bg-fade`（背景切/淡时长）、
`--fx`（开特效）、`--fx-flash --fx-punch --fx-vignette --fx-chromatic
--fx-sat`、`--bloom`（辉光强度）、`--fps`（输出帧率）。

**field 布局（进阶）**：`--layout field --groups auto` 让歌词词块在世界
空间蛇形铺开，配 `--roll-power`（滚转力度）、`--force-whip "关键词"`
（对某行强制甩镜）、`--energy-file out\audio_env.json`（按音频能量甩镜）。

### 2.2 两组起步预设

温柔抒情：

```bat
dist\chamina-cpp.exe gen examples\sample.lrc -o out\soft.json -t examples\gift.json ^
  --entrance fade --size 2.6 --y 0.4 --stagger 0.14 --palette "0xff8fc7,#8fd0ff" ^
  --radius 16 --yaw-drift 0.02 --fx --fx-vignette 0.4 --fx-sat 1.05 --bloom 0.25
```

动感燃向：

```bat
dist\chamina-cpp.exe gen examples\sample.lrc -o out\hype.json -t examples\gift.json ^
  --entrance pop --jitter 0.5 --jitter-hit --wave 0.3 --wave-hit --shake 0.04 ^
  --shake-hit --dolly 2 --palette "0xff4d6d,#ffd166" --fx --fx-flash 0.3 ^
  --fx-punch 0.5 --bloom 0.5 --fps 60
```

两个 JSON 都可以继续手调（见 §5）。想直接看参考产物，仓库带
`examples/bird.json`（`examples/bird.lrc` 的全量生成结果）。

---

## 3. 音频包络（可选）

有音乐时才有频谱环/频谱条/火花/风线：

```bat
dist\chamina-cpp.exe audio song.mp3 -o out\audio_env.json
```

- 采样率参数：`--fps 60 --bands 64 --lo 40 --hi 14000`（默认即此；
  `--fps` 应与场景 fps 一致）；
- 渲染时传 `--audio-env out\audio_env.json`；
- field 布局还可把同一份包络传给 `gen --energy-file out\audio_env.json`，
  让甩镜跟着音乐能量走。

没有音频时，所有可视化组件静默关闭，其余一切照常。

---

## 4. 渲染与验收

**先抽静帧**（快，不需要 ffmpeg）：

```bat
dist\chamina-cpp.exe out\soft.json --still 3 --png out\check3.png
dist\chamina-cpp.exe out\soft.json --still 8 --png out\check8.png
```

看构图/文字/背景色是否满意——不满意回到 §2 调参数或 §5 改 JSON。

**再试一小段**（比如 5 秒）：

```bat
dist\chamina-cpp.exe out\soft.json --audio-env out\audio_env.json ^
  -o out\preview.mp4 --from 0 --frames 150
```

**最后出全片**（`--frames 0` = 到片尾）：

```bat
dist\chamina-cpp.exe out\soft.json --audio-env out\audio_env.json -o out\final.mp4
```

`CPP_PROF=1` 会在结束时打印每帧毫秒数：

```bat
set CPP_PROF=1
dist\chamina-cpp.exe out\soft.json -o out\final.mp4
```

**排错开关**：`CPP_TRACE=1` 打印相机/文字块状态；`CPP_NOVIZ=1` 关可视化；
`CPP_SKIP_CLOUD=1` / `CPP_SKIP_STARS=1` 关星云/星点；
`CHAMINA_NO_SIMD=1` 退回纯标量（与 SIMD 结果对照）。

---

## 5. 手调场景 JSON

场景 JSON 是纯文本，字段平铺在根级。常用字段：

### 5.1 顶层

| 字段 | 说明 |
|---|---|
| `width` / `height` / `fps` | 分辨率与帧率（如 1920×1080@30） |
| `duration` | 总时长（秒） |
| `background` | 清屏底色（十六进制） |
| `font_fallback` | 字体路径数组（`.ttc/.ttf`，按序尝试） |
| `bloom` / `dof` | 后处理开关与参数（见下） |
| `camera` | 视场/近远裁面 |
| `camera_animation` | 相机轨道/运镜/震动/逐词 stops |
| `lights` | 三点光 + 环境光（key/rim/fill/ambient） |
| `seed` | 随机种子（抖动/星点布局） |
| `texts[]` | 文字块（见 5.2） |
| `backgrounds[]` | 背景时间线（见 5.3） |
| `fx` | 画面特效（见 5.4） |

### 5.2 文字块 `texts[i]`

```json
{
  "text": "夜色落在窗台",
  "size": 2.6,
  "depth": 0.12,
  "position": [0, 0.4, 0],
  "material": { "type": "neon", "color": "#00e5ff", "strength": 1.7,
                "outline": { "color": "#eafcff", "width": 0.02, "strength": 1 } },
  "alpha": 1,
  "fit_width": true,
  "animations": [
    { "type": "fade", "duration": 0.9, "stagger": 0.1,
      "fade_out_at": 8.5, "fade_out": 0.8 },
    { "type": "slide", "delta": [0, -1.6, 0], "duration": 1.2,
      "stagger": 0.05, "ease": "ease_out_quad" }
  ]
}
```

`animations[].type` 支持：`fall`（下落回弹，键 `height/gravity/drift`）、
`fade`（淡入淡出，键 `fade_out_at/fade_out`）、`slide`（位移入场，
键 `delta/ease`）、`pop`（弹出）、`jitter`（持续抖动，键 `amplitude/
frequency/phase`）、`wave`（波浪，键 `amplitude/frequency/spacing`）、
`spin`（绕轴旋转，键 `ax/ay/az/speed`）。入场类动画都吃
`start_at / stagger`（逐字符时间戳）。

### 5.3 背景时间线 `backgrounds[]`

按 `at`（秒）升序，相邻关键帧以 `fade` 秒交叉淡化：

```json
{ "at": 0, "fade": 2, "top": "#01040f", "bottom": "#05060f",
  "accent": "#2f6dff", "nebula": 0.55, "stars": 0.75, "drift": 1 }
```

- `top/bottom`：天空渐变两端色；`accent`：星云/氛围强调色；
- `nebula`：星云强度（0 关闭，省 1–2ms/帧）；`stars`：星点密度/亮度；
- `drift`：星云漂移速度倍率。

### 5.4 相机 `camera_animation`

```json
{
  "type": "orbit", "radius": 14, "elevation": 0.09,
  "speed": 0.002, "start_yaw": 1.27, "target": [0, 1, 0],
  "movement": { "ease": "ease_in_out_cubic", "radius_end": 12.6,
                "elevation_end": 0.22, "at": 45, "duration": 275 },
  "shake": { "amplitude": 0.03, "frequency": 3, "roll": 0.006,
             "at": 0, "until": null, "ramp": 0.8, "hit": true,
             "hit_duration": 0.55 },
  "stops": [
    { "at": 15.34, "duration": 1.2, "ease": "ease_in_out_cubic",
      "target": [0, 0, 0], "radius": 14, "elevation": 0.09,
      "yaw": 0.05, "roll": 0.03 }
  ]
}
```

- `speed` 是轨道角速度；`movement` 是整段缓慢运镜（`radius_end /
  elevation_end`）；
- `stops` 是逐词/逐段的相机停：`yaw/roll` 是**偏移量**（不是绝对角），
  `duration` 为过渡时长；
- `shake` 的 `hit` 让震动对音频重拍敏感（需音频包络）。

### 5.5 特效与后处理

```json
"fx": { "enabled": true, "chromatic": 0.0014, "vignette": 0.5,
        "saturation": 1.1, "flash": 0.18, "punch": 0.3,
        "hit_duration": 0.35 },
"bloom": { "enabled": true, "intensity": 0.3, "threshold": 1.0,
           "scale": 1.2, "high_quality": true },
"dof":   { "enabled": true, "focal_distance": 14, "focal_radius": 1,
           "sensor_height": 0.035, "aperture_f_stops": 5.6 }
```

- `vignette` 0–1；`chromatic` 建议 ≤0.003；`flash/punch` 是节拍闪白/推拉；
- `bloom.intensity` 0–1（0.05–0.5 常用）；`threshold` 越高越只亮高光；
- `dof` 关掉最省时间；开着时 `focal_distance` 对准文字所在深度。

改完 JSON 先按 §4 抽静帧复验，再出片。

---

## 6. 嵌进自己的程序

```cpp
#define CHAMINA_IMPLEMENTATION      // 恰好一个 .cpp
#include "chamina.h"
```

```bat
g++ -O2 -std=c++17 -fopenmp -static -static-libgcc -static-libstdc++ ^
    -I. -Ivendor example\main.cpp -o dist\chamina-example.exe -lshell32
dist\chamina-example.exe examples\bird.json out\audio_env.json out\frame.png 8
```

API：`read_file`（UTF-8 路径安全）→ `engine_create(scene, env, err, len)`
→ `engine_width/height/fps/total_frames` → `engine_render_rgb`（RGB24 直出，
喂纹理/表面）或 `engine_still_png` / `engine_encode_video` →
`engine_destroy`。完整示例见 `example/main.cpp`。

注意事项：

- `engine_create` 不抛异常（失败返回 `nullptr` + `err`）；
- 帧缓冲是函数级 static，跨引擎实例不线程安全；
- 实现 TU 不加 `-fopenmp` 也能编（单线程）；Windows 必须 `-lshell32`。

---

## 7. 常见问题

| 症状 | 处理 |
|---|---|
| `error: cannot read ...` | 路径不存在或编码不对；用 `chamina::read_file`/CLI 都支持中文路径，检查拼写 |
| 渲染报 ffmpeg 相关错误 | `ffmpeg -version` 是否可用；否则设 `CHAMINA_FFMPEG=完整路径` |
| 视频没有声音 | 本工具只出画面；用 ffmpeg 自行合流：`ffmpeg -i video.mp4 -i song.mp3 -c:v copy -c:a aac -shortest final.mp4` |
| 出片太慢 | 降 `fps` 或分辨率；`CPP_PROF=1` 看 render/finish 占比；关闭 `dof`/`bloom` |
| 文字缺字/方块 | `font_fallback` 换成本机存在的中文字体（如 `C:\Windows\Fonts\msyh.ttc`） |
| 片长不对 | 检查 LRC 末行时间；`--tail` 加尾部留白，`--duration` 强制总长 |
| 画面全黑 | 先 `--still` 抽帧确认场景 JSON 合法；`CPP_TRACE=1` 看文字块是否激活 |
| 想对照 SIMD/标量差异 | `CHAMINA_NO_SIMD=1` 渲染同一帧对比 |

---

更多参数以 `chamina-cpp gen --help` 与源内注释为准。
