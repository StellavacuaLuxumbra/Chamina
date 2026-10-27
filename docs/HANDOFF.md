# Chamina C++ 引擎 — 交接文档（给下一个 AI）

> 写于 2026-10-25。读这一份即可接手；背景细节在 `docs/cpp-migration.md`
>（§一~§八）与 `docs/session-summary.md`（§十二~§十四）。本文只写「现在
> 处于什么状态、下一步做什么、哪些坑别再踩」。

## 0. ✅ 已解决：resize 修复已验证（2026-10-28）

原状态：`render_frame_into()` 的颜色清屏从 `assign` 改成 `resize`（背景激活时
跳过 24MB 清屏）但未验证。**现已验证成立**：

- `cpp\build.bat` 编译通过（注意：冒烟测试管道行在本机 PowerShell 转发下会
  卡住——见 §5 新增条目；编译本身 ~2 分钟，可用直接 g++ 命令替代）。
- 静帧 `--still 8 --png t.png` 与 30 帧视频段 `--from 570 --frames 30 -o t.mp4`
  均成功，画面正常（3.25-3.61 fps）。
- 静帧路径每次新建 `Frame`，`resize` 零填充 == `assign`，静帧像素必然不变；
  视频路径 dome 全覆盖每像素，旧帧残留不会泄漏。
- 旧版（git 提交态，无 LUT/resize）与新版同刻渲染对比：t=8 / t=20.35 各区域
  比值 0.998–1.000，优化对画面无漂移。
- 回归脚本已转正：`js/region-cmp.mjs`（exit 0=过 / 2=漂移>0.05），
  区域约定：sky=上 20%、horizon=60-75% 行、text=中央带 38-62%×30-70%。

回归值（js/region-cmp.mjs，C++/Rust native）：t=8 sky 1.005 / horizon 1.014 /
text 1.030 / full 1.020；t=20.35 full 1.026；t=325 full 0.989。
（历史记录里 t=20.35 全区 0.970 是当时另一套临时采样约定，旧二进制在
region-cmp.mjs 下同样给 1.026——两代二进制一致即为无回归，不与旧约定值比。）

## 1. 使命与现状

用户目标：**整个项目彻底 C++ 化（字体/排版/动画/渲染全重写）= 交付软件
本身**。视频按需渲染，不再默认出成片。

现状：C++ 全引擎已完成并通过验收，存在三条等价渲染通路（同一份场景 JSON
→ 同一画面）：
1. JS + 浏览器（`js/scene.js`，作者预览，Edge 156 已无法 headless 启动）
2. Rust 软件光栅（`src/native`，chamina-native.exe，参考实现）
3. **C++（`cpp/`，chamina-cpp.exe，主交付，2.7-3.7 fps）**

## 2. 构建与验收（每次改动的标准流程）

```bat
cpp\build.bat        → dist\chamina-cpp.exe（静态单文件；自动用 D:\mingw64 的 g++ 15.2）
```

**回归金标准**（Rust 原生静帧 = 浏览器成片的已验收代理，在
`target/chamina/native_{8,20.35,325}.png`）：

```bash
# 三个关键帧 + 区域亮度比（比值应稳定在 0.97–1.03）
for t in 8 20.35 325; do dist/chamina-cpp.exe scenes/bird.json \
  --audio-env target/chamina/audio_env.json --still $t --png target/chamina/cpp_$t.png; done
# 然后区域亮度比对比（比值应稳定在 0.97–1.03，exit 2=漂移>0.05）：
node js/region-cmp.mjs target/chamina/cpp_$t.png target/chamina/native_$t.png
```

最近一次通过值：t=8 天空 1.004 / 地平线 1.029 / 文字 1.018；
t=20.35 全区 0.970；t=325 全区 1.001。**任何比值漂移 >0.05 都是新 bug。**

## 3. 性能现状与优化清单

当前（CPP_PROF=1 输出 render/finish 两段毫秒数）：

| 阶段 | 优化前 | 现在 | 已做 |
|---|---|---|---|
| render | 138ms | **99ms** | skydome 星云半分辨率+LUT（powf/expf→1D LUT）|
| finish | 168ms | **140ms** | sRGB 编码 LUT、bloom 级联上采样（5 次全幅→1 次）|
| 帧分配 | — | 已省 | Frame 复用（render_frame_into）、color 跳清屏（⚠️未验证）|

**待做优化（按性价比排序）**：
1. `finish()` 里 bloom 的 5 级 `blur()` 每帧分配 tmp+out（~15MB）→ 静态
   线程局部缓冲复用；`dof()` 同理。
2. `grade()`/`dof()` 的 half-res 打包循环可以合并进 blur pass（省一次
   全幅读写）。
3. tonemap 的 ACES `fit()` 每像素 3 次——可换 256 级 1D LUT（输入域
   clamp 后单调）。
4. 文字光栅：可见字通常 <40 个，但每帧对全部 38 块做 `eval_all`——
   按 `fade.start_at ± 5s` 预剔除（纯 CPU 节流，不影响画面）。
5. 若还想要 2 倍：把 skydome/blur 换 SIMD（f32x4 手写或 highway），
   或回到 wgpu 方案（docs/cpp-migration.md §二 有接口设计）。

## 4. 已修 bug 清单（防回归，改相关代码前先读）

| # | 症状 | 根因 | 修法位置 |
|---|---|---|---|
| 1 | earcut 前段错误 | 孔洞收集写错容器（rings/cut 混用）| build_glyph_mesh 孔洞循环 |
| 2 | 文字带亮度 +27% | base_position 漏加 bbox 中心补偿（org_x/org_y）| main 的字形循环 |
| 3 | **中段全片镜头朝向错误** | stop 的 yaw/roll 是**偏移量**（JS yawOff），初版写成替换轨道 yaw | eval_orbit 的 yaw_off/roll_off 通道 |
| 4 | 视频报 done 但 mp4 不落盘 | popen→cmd /c 吞首引号 + 拒正斜杠路径 | find_ffmpeg 反斜杠归一 + 命令外层引号 |
| 5 | ffmpeg 死了照样打 done | pclose 返回值被忽略、无 ferror 检查 | 渲染循环尾部（已加，返回 1）|
| 6 | 老场景（gift/lyrics/effects）崩 | 硬访问 fx/bloom/dof/backgrounds/start_at | main 的 meta 归一化 + cues 收集容错 |
| 7 | MinGW popen | `_popen` 不可用 | 用 POSIX `popen/pclose` |

**对照语义的权威来源**：`js/anim.mjs`（相机/背景/命中包络）、
`src/animation.rs`（字符动画）、`js/scene.js`（可视化/后处理公式）、
`src/text/mesher.rs`（挤出）。改 C++ 前先读对应 Rust/JS，别凭记忆。

## 5. 已知坑（本机环境，全部踩过）

- **Git Bash 内联 `node -e "..."` 里写 `\\n` 会变成真换行** → 破坏 C++
  字符串字面量。补丁一律用 Write 工具落盘成 .cjs 再 `node file.cjs`。
- bash heredoc 同样吃反斜杠；C++ 测试代码用 Write 工具写。
- `cmd /c` 引号规则：整条命令外多包一层引号；可执行路径必须反斜杠；
  含中文/空格的输出路径在本机（UTF-8 代码页）实测正常。
- PATH 上的 `g++` 是 `D:\mingw` 的 32 位 8.2（坏）——必须用
  `D:\mingw64\bin\g++.exe`（build.bat 已写死探测）。
- MinGW `popen/pclose` 可用；`-static -static-libgcc -static-libstdc++`
  必加，否则运行时缺 DLL（症状：bash 报 127）。
- Edge 156 无法 headless 启动（启动即退 Code 0）——JS 通路的回归只能靠
  已存静帧；浏览器侧想再验证用 `D:/pbrowsers/chrome-headless-shell/...`
  + `--gpu`（CHAMINA_BROWSER）。
- 逐像素对比无意义（fbm 的 sin 在 f32/GPU/f64 三方混沌不同）——**只比
  区域统计均值**（见 §2），阈值 ±0.05。脚本：`node js/region-cmp.mjs
  <test.png> <gold.png>`（exit 0=过，2=漂移）。
- **build.bat 的冒烟行在本机 PowerShell→cmd 转发下会卡住**（无输出、
  15 分钟不返回；exe 直跑正常）。直接用 g++ 命令编译：
  `D:\mingw64\bin\g++.exe -O2 -std=c++17 -fopenmp -static -static-libgcc
  -static-libstdc++ -Icpp/vendor cpp/src/chamina.cpp -o dist/chamina-cpp.exe`
  （~2 分钟）。build.bat 本体待查（可能是 `2>&1 |` 管道 + cmd /c 的交互）。
- 读 PNG 用 `js/lum.mjs` / `js/preview.mjs`（本会话 Read 工具看图不可信，
  见 session-summary §二）。

## 6. 文件地图

```
cpp/
├─ build.bat           一键构建 + 冒烟
├─ README.md           用法/开关/架构速览
├─ src/chamina.cpp     全引擎（~2000 行单 TU；各节注释标明公式来源）
└─ vendor/             nlohmann/json.hpp, stb_truetype.h, earcut.hpp, stb_image_write.h
dist/chamina-cpp.exe   交付产物（静态，3.5MB）
src/native/            Rust 参考实现（对照验收用）
js/                    作者工具链（lrc2scene/audio-env/editor）——未 C++ 化
docs/cpp-migration.md  全部决策与实施记录（§一~§八）
docs/session-summary.md §十二~§十四（v10 特效、Rust/C++ 落地）
scenes/bird.json       主测试场景（30fps/11010 帧/314 相机 stop/34 背景键）
target/chamina/        audio_env.json、native_*.png 金标准、场景备份
```

## 7. 建议的工作队列（接手后按序）

1. **验证 §0 的 resize 修复**（未做，见最上）。
2. 把 §3 的「待做优化」逐项做掉：每项做完必须跑 §2 回归（比值不变才算
   过，画面差异>0.05 回滚）。
3. 功能缺口（对照 JS 版逐一移植）：
   - `lrc2scene` 的场景生成链（LRC 解析/词组/甩镜布局/调色板）→ 做成
     `chamina-cpp gen` 子命令，彻底甩掉 Node；
   - `audio-env`（miniaudio 解码 + pocketFFT）→ `chamina-cpp audio` 子命令；
   - editor（Web GUI）——低优先级。
4. 崩溃/边界 fuzz：空 texts、无 camera_animation、超长行、单字场景、
   depth=0.001、fit_width=0——都应不崩（老场景兼容模式已开路）。
5. 每完成一个大项，更新 `docs/cpp-migration.md` §八/§九 与本文件。

## 8. 用户偏好（长期有效）

- 视频成片按需；**交付物是软件**。
- 不要问「要不要继续」——自主循环：改 → 编译 → 回归 → 记录。
- 中文交流；文档写中文；代码注释英文（跟仓库惯例）。
- 一切公式改动必须同时核对 js/anim.mjs 与 src/animation.rs 两处权威。
