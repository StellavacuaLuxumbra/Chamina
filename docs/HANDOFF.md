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

## 0a. ✅ lrc2scene 已移植为 `chamina-cpp gen`（2026-10-28）

**Node 的 lrc2scene 依赖已甩掉一半**：LRC → 场景 JSON 生成链移植进
`cpp/src/chamina.h`（gen 节，~1500 行），入口 `chamina-cpp gen <lrc> -o out.json`。

- **对齐标准**：与 `node js/lrc2scene.mjs` 的 `JSON.stringify(scene, null, 2)`
  输出**字节级一致**。方法：`nlohmann::ordered_json` 保 JS 键插入序 +
  自写 `js_dtoa`（ECMAScript Number→String，最短往返、1e21/1e-7 指数阈值）
  + 自写 `js_json` 序列化器（不用 nlohmann dump——它打 `"14.0"` 而 JS 打 `"14"`）。
  详见 `docs/cpp-migration.md` §十。
- **已验证**：自写测试 LRC + 14 组 flag 组合全部字节级一致；真实语料
  `scenes/bird.lrc`、`scenes/sample.lrc` 字节级一致（0 失败）。
- **第二阶段已补齐（同日，见 cpp-migration §十四）**：`--layout field` /
  `--groups auto`（js/layout.mjs layoutField + js/lrc.js wordGroups）、
  `--roll-power`（whip）、`--force-whip`、`--energy-file`（loadEnergy）——
  bird 8 组合 + sample 4 组合 + keep-camera 1 组合，13/13 字节级一致。
  gen 至此**全量接管** node lrc2scene。
- **gen 渲染回归**：argv UTF-8 重解码 + u8path + PNG to_func 改动后，
  新旧二进制同刻 A/B t=8 与 t=20.35 全区比值 1.000；20 帧视频段管道正常。

## 0b. ✅ audio-env 已移植为 `chamina-cpp audio`（2026-10-28）

**Node 的音频分析依赖也甩掉了**：`chamina-cpp audio <audio> -o audio_env.json`
（--fps/--bands/--lo/--hi 同 node 版），输出与 `node js/audio-env.mjs`
**字节级一致**（默认参数 22068 帧 / 64 频带 / 7,703,325 字节 + 3 组 flag 组合
全部 0 差异）。

- **实现决策**：不走早先设想的 miniaudio+pocketFFT——JS 版实际是 ffmpeg
  子进程解码 + 手写 radix-2 FFT，C++ 端同构复刻即字节级一致，渲染器本就
  依赖 ffmpeg，零新 vendored 库。关键是复刻 JS 的 Float32Array 舍入链：
  **float 存储、double 运算**——蝶形每次存回舍入到 f32、twiddle 累乘保持
  double；libm 1 ulp 差(~1e-13)被 f32 舍入(6e-8)完全淹没，实测无差异。
- **Windows 坑×2（均已修，渲染端连带受益）**：
  1. popen 的命令行走 ANSI(=ACP 936，本机实测)，UTF-8 路径转换失败 →
     _popen 直接返回 NULL。**渲染端视频管道同样中招**——HANDOFF 旧说
     「中文输出路径实测正常」不成立。两处都改为 CreateProcessW 直起
     ffmpeg（UTF-16 参数 + 匿名管道，stdout 丢 NUL、stderr 透传）；
  2. 句柄继承陷阱：bInheritHandles=TRUE 时子进程复制**所有**可继承句柄，
     管道两端都被带走 → EOF 永不到达（症状：最后帧后挂死）。必须
     SetHandleInformation 把我方持有的端设为不可继承。
- **错误分支**：audio too short / 坏文件 / 未知 flag / --help 均复刻 node
  行为；管道断裂（stub ffmpeg）在 frame 1 响亮报错 exit 1，不假 done。
- find_ffmpeg 顺带补 exe 相对路径解析（dist\chamina-cpp.exe 任意 cwd 可用）。

## 0c. ✅ 崩溃/边界 fuzz 第一轮全清（2026-10-28）

交接文档 §7.4 点名的全部边界项已人工矩阵化（48 例：渲染端突变 22 +
gen LRC 边界 26），harness 判定 exit∈{0,1}=清，其余=崩。

- **首轮 6 例崩溃**，全是同一类：渲染端 main 没有 try/catch，nlohmann
  `out_of_range` 直抛 terminate。gen 端 26 例**首轮即全清**（LRC 解析器
  手写正则语义，无异常逃逸）。
- **修复**：main 拆出 render_main 整体兜底 + 入口兼容归一化扩容（缺字段
  补默认值；合法场景零路径变化）。修复后 48/48 全清，t=8 门禁与 gen 字节
  复核逐位不变。
- 细节与例表见 `docs/cpp-migration.md` §十二。

## 0d. ✅ 单头库化：`chamina.h`（stb 风格）+ 薄壳 CLI（2026-10-28）

最终交付形态落地：**`cpp/src/chamina.h` 单头库**（~5900 行，stb 风格；
gen 第二阶段后）。宿主 C++ 工程在一个 `.cpp` 里 `#define
CHAMINA_IMPLEMENTATION` 后 include 即可渲染；`cpp/src/chamina_cli.cpp`
（~120 行薄壳）= 旧 `chamina.cpp` 的 main+参数解析，编译出同一个
`dist/chamina-cpp.exe`。

- **头文件结构**：banner/用法文档 → include 守卫（`CHAMINA_H_INCLUDE`）→
  `namespace chamina` 公共 API 声明 → `#ifdef CHAMINA_IMPLEMENTATION` →
  引擎体（原 chamina.cpp 第 16–4177 行**逐字**迁移）→ 公共 API 实现 → 两个
  `#endif`。vendored 依赖经 `-Icpp/vendor` 解析。
- **✅ Phase 2 amalgamation 已完成**（同日，见 cpp-migration §十三）：
  `node cpp/tools/amalgamate.cjs` 产出 `dist/chamina.h`（38,719 行，确定性
  生成），4 个 vendored 头逐字内联——宿主工程只需**一个文件**。验证 9/9：
  活代码零引号 include（stb_truetype `#if 0` 示例块的 2 处自 include
  inert，接受）；单文件拖进普通目录即可编译 example 与 CLI 薄壳，出图与
  金标准逐字节相等；仅声明 TU 双 TU 用法成立。dist/ 不入库，按需再生成。
- **公共 API**（`namespace chamina`）：`Engine`（不透明；持有 scene/meta
  拷贝 + World + FontEngine + w/h/fps/total）、`engine_create(scene_json,
  audio_env_json, err, err_cap)`（不抛异常，失败填 err 返回 nullptr）、
  `engine_render_rgb`（RGB24 直出，异常时清零+stderr）、
  `engine_still_png`、`engine_encode_video`（frames=0 到片尾；CPP_PROF
  语义保留）、`engine_width/height/fps/total_frames`、`set_trace`、
  `read_file`、`argv_utf8`、`gen_main`/`audio_main`（CLI 形参入口）。
- **悬垂指针关键点**：`PreparedText::anims/char_times` 指向场景 JSON——
  engine_create 必须用 `e->scene`（Engine 自己的拷贝）迭代装配，绝不能
  引用函数局部 scene。
- **回归（全部绿）**：静帧 t=8/20.35/325 新旧二进制**逐字节相等**，且
  t=8 门禁 1.005/1.014/1.030/1.020、t=20.35 full 1.026 与记录值精确吻合；
  gen bird/sample 与 node 字节级一致；audio 与 node 字节级一致（7,703,325
  字节）；60 帧视频段新旧 mp4 字节相等（925,553）；fuzz 48/48；错误分支
  （缺文件/坏 --still/无参数 usage/gen 无输入/audio too short）全保留。
- **嵌入验证**：`cpp/example/main.cpp`（自身定义 IMPLEMENTATION）独立编译
  运行成功，输出 PNG 与 CLI 静帧**逐字节相等**；仅声明 TU（不含
  IMPLEMENTATION）单独编译通过——stb 双 TU 用法成立。
- 一次性拆分工具 `tmp/split.cjs`（源料 `tmp/chamina_head.txt` banner/API
  声明、`tmp/chamina_api.inc` API 实现；chamina.cpp 已删，此后 chamina.h
  即唯一直接编辑的引擎源），细节见 `docs/cpp-migration.md` §十三。

## 0e. ✅ fuzz 第二轮 + JS 越界语义修复（2026-10-29）

第二轮 fuzz（`tmp/fuzz2/harness.mjs`，打 gen 旗标面：随机可解析 LRC
语料 × 随机旗标组合，node/C++ 双跑逐字节 + exit 对齐 + 渲染冒烟 + 错误
路径）种子 20261028 全绿后，种子 77 抓出 2 个真 bug——详情与取证链见
`docs/cpp-migration.md` §十五。核心：PHRASE_CUTS 的 `chunks(n,3)` 在
窄短语时 piece 之和可超过词组长，组链越过行尾。node 的语义是
`charTimes[i]` → undefined、`out[i]=v` 数组就地生长（char_times 多出
一条 null）、`Math.max` 任一 NaN → 全 NaN（最终 fade_out_at=null）；
C++ 侧则是三处 UB/语义差（越界读 → case_26 的 0xC0000474 崩溃；
`std::max_element` 跳过 NaN → case_14 的 0.15 假正常值）。修复：
`js_max_of`/`js_min_of`（NaN 传播折叠）、flush 越界读 NaN 守卫、
`group_char_times` 越界写改 JS 生长语义（resize 填 NaN）。

**回归（全绿）**：seed 77 32/32、seed 20261028 48/48、追加种子
424242/9001/31337 各 32/32（**五种子累计 176 例**）、amalgamation
9/9（dist/chamina.h 重生成，38,748 行）、渲染门禁 t=8 与历史值一致、
t=20.35 新旧二进制 A/B 全 1.000、t=325 带内。

## 1. 使命与现状

用户目标：**整个项目彻底 C++ 化（字体/排版/动画/渲染全重写）= 交付软件
本身**。视频按需渲染，不再默认出成片。

现状：C++ 全引擎已完成并通过验收，存在三条等价渲染通路（同一份场景 JSON
→ 同一画面）：
1. JS + 浏览器（`js/scene.js`，作者预览，Edge 156 已无法 headless 启动）
2. Rust 软件光栅（`src/native`，chamina-native.exe，参考实现）
3. **C++ 单头库（`cpp/src/chamina.h`，chamina-cpp.exe 为其 CLI 薄壳，
   ~5.9 fps，主交付）**

此外 `chamina-cpp gen` 已全量接管 lrc2scene（含 --layout field /
--groups auto / whip，见 §0a）、`chamina-cpp audio` 已接管 audio-env
（见 §0b）；Node 仅剩 editor。

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

最近一次通过值：t=8 天空 1.005 / 地平线 1.014 / 文字 1.030 / 全区 1.020；
t=20.35 维持历史已知采样伪影（sky 1.348 / text 0.908，exit 2 与历史同判）；
t=325 全区 0.989。**任何比值漂移 >0.05 都是新 bug。**

**Tier A / Tier B 双门禁语义**（cpp-migration §十六）：Tier A 位精确切片
用字节门禁（AVX-on 逐字节相等）；**Tier B 近似切片走双枝**——(a)
`CHAMINA_NO_SIMD=1` 的 dist 对 pre-SIMD 基线跑
`tmp/gate_sim_nosimd.mjs` 须 **8/8 ALL EQUAL**（证标量参考线未动）；
(b) AVX-on 字节 DIFF 是预期，主验收 = 同上 region 三帧（±0.05 且 test
亮度与历史批逐位一致）。

## 3. 性能现状与优化清单

当前（CPP_PROF=1 输出 render/finish 两段毫秒数）：

| 阶段 | 优化前 | 现在 | 已做 |
|---|---|---|---|
| render | 138ms | **28.7ms**（本轮同刻配对中位） | skydome 半分辨率+LUT；格点 hash/星点胞/闪烁查表；**skydome 全幅 + 云场 prepass AVX2 内核（Tier A）；atan2f 向量多项式（Tier B）** |
| 　skydome | 132ms | **15.9ms**（配对 base 58.1 → new 15.9，`[prof] skydome` 中位） | base 11.9 / 星云 19.5 / 星点 19.8（桩阶梯分解）；**全幅循环 + fbm 场 lane 化（域内零 sinf）+ atan2f 多项式** |
| finish | 168ms | **47.5ms**（fx-on bird，2026-10-31 同刻配对中位；同口径基线复测 69.3） | sRGB LUT、bloom 级联上采样、blur 缓冲复用+跳死代码水平 pass、ACES fit LUT、全链静态缓冲/原地化；**AVX2 tonemap/grade 内核 + pack 并行（位精确）** |
| 帧分配 | — | 已省 | Frame 复用、color 跳清屏（已验证）、文字块窗口预剔除 |
| 视频段 | 3.25 fps | **8.29 fps**（同窗交替配对 60 帧×3 轮，同刻基线 5.42） | ~~SIMD stage 1~~ + skydome Tier A + 云场 prepass lane 化 + atan2f Tier B |

**待做优化（按性价比排序）**：
1. ~~bloom 5 级 blur 的每帧 tmp+out 分配~~（已完成：blur_into 静态缓冲 +
   dof/grade/srgb/bytes 静态化；顺带发现水平 pass 是死代码——见 §4 条目 8）
2. ~~tonemap ACES fit 换 LUT~~（已完成：4097 级域 [0,8]，域外精确求值）
3. ~~文字块预剔除~~（已完成：fade 窗口 ±5s，PreparedText.vis_from/vis_to，
   剔除整块 eval_all；无 fade 块永不剔除——零像素变化，t=12/100 A/B 验证）
4. ~~`grade()`/`dof()` 的 half-res 打包循环合并~~（**已像素级验证但被测驳回**
   2026-10-29：dof mix 内联 tonemap + 无-fx 直灌 pack 两路融合，dof×fx 四
   组合新旧二进制 PNG 逐字节相等；但配对中位测量 **+1.8ms/f**——tonemap
   从独立的全幅 pass（编译器可向量化）被并进 dof 的标量混合热循环，失去
   自动向量化机会，省下的 ~1 趟读写（≤1ms）盖不回。**revert，保留三
   pass 结构**；结论：这条不再碰，直到 SIMD/wgpu（§二 接口设计）。
5. ~~finish 里 grade/tonemap/pack 三趟全幅~~（**已完成**，2026-10-31：AVX2
   Tier A 位精确内核，字节 8/8 全等；口径注记见 cpp-migration §十六）。
   ~~skydome 全幅 Tier A~~（**已完成**，2026-10-31 同日第二批：字节门禁
   8/8 全等；同刻配对 render 70.7→45.0ms、`[prof] skydome` 子段
   57.1→32.1ms、视频段 5.37→7.24 fps）。~~云场 prepass lane 化~~
   （**已完成**，同日第三批：fbm2 跨 lane 四角 gather + 域外 fallback
   lane 提取混回；域内帧已零 sinf；ingress render 71.1→32.7ms、
   skydome 子段 58.8→20.2ms、视频段 5.40→**8.00 fps**，字节 8/8 全等）。
   ~~atan2f 向量多项式~~（**已完成**，同日第四批，首例 Tier B：
   `atan2f_vec256` 八分域 + 双重减半 + 9 项 Horner，探针
   max|err| 2.384e-7 rad；render 32.7→28.7ms、skydome 20.2→15.9ms、
   视频段 8.00→**8.29 fps**；双枝门禁 = NO_SIMD 字节 8/8 全等 +
   AVX 字节 DIFF 预期 + region 三帧 test 亮度与历史逐位一致，见
   cpp-migration §十六 atan2 批）。**裁定收尾**（同日，两驳回见
   cpp-migration §十六）：cell_tw 向量化（<0.3ms 且需大参约简，驳回）、
   byte-pack 内核（净省 <0.4ms 而 AoS 转置 bug 面大，驳回）——
   SIMD 大项 CPU 侧切片**全部完成**；wgpu 按帧函数段签名后接管（暂不
   引入）。分发层/政策见 cpp-migration §十六。

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
| 8 | （语义发现，非 bug）| blur 的水平 pass 写 tmp 后从不被读——两方参考实现（src/native/post.rs 与本文件）都只做垂直单轴模糊，金标准即此语义渲染。**保持一致，勿修**；修了反而偏离金标准。浏览器版（UnrealBloomPass）本就是另一实现，区域比值 0.97-1.03 已覆盖差异 | blur_into 跳过水平 pass（省一半模糊计算，像素不变）|
| 9 | gen 报 unknown argument | argv 循环未跳过 `"gen"` token（分发入口自身） | main 的 gen 分支：跳过 argv[0]、argv[1] |
| 10 | `[00:05.500]` 解析越界 | match_clock 带分数分支 len 公式多加 1（11 字节算出 12）→ substr 抛 | match_clock 小数分支：len = n + 1 + frac_len |
| 11 | MinGW 编译 NOMINMAX 冲突 | windows.h 顺序敏感的重定义 | #ifndef NOMINMAX 守卫 |
| 12 | 突变场景崩溃（0xC0000409）| 渲染端 main 无 try/catch，nlohmann .at() 的 out_of_range 直抛 terminate | main 拆出 render_main + 整体兜底（干净 exit 1）|
| 13 | 老场景缺字段崩溃 | texts 无键/缺 material·position·size·alpha·animations、backgrounds 缺 top·bottom·accent·at、fps 缺失或 0、duration 缺失 | render_main 入口兼容归一化（默认=gen 旗标默认；warning 一次）|

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
  -static-libstdc++ -Icpp/vendor cpp/src/chamina.cpp -o dist/chamina-cpp.exe
  -lshell32`
  （~2 分钟；-lshell32 是 CommandLineToArgvW 所需，缺了链接失败）。
  build.bat 本体待查（可能是 `2>&1 |` 管道 + cmd /c 的交互）。
- 读 PNG 用 `js/lum.mjs` / `js/preview.mjs`（本会话 Read 工具看图不可信，
  见 session-summary §二）。
- **Windows 非 ASCII（gen 移植期修掉，勿回退）**：
  1. argv 中文必挂——`main(char* argv[])` 拿到的是 ANSI codepage。已改
     `GetCommandLineW` + `CommandLineToArgvW` + `WideCharToMultiByte(CP_UTF8)`
     重解码（为此 build.bat 加 `-lshell32`，链接失败先查它）；
  2. MinGW `ifstream(char*)` 走 ANSI codepage——文件读写一律
     `filesystem::u8path(path)`（构造 fstream）；
  3. stb_image_write 的窄路径 fopen 同理——静帧 PNG 已改
     `stbi_write_png_to_func` + 宽路径写文件。
- ~~ffmpeg popen 的中文路径问题~~：**已修**——popen 命令行走 ANSI(=ACP)，
  本机 ACP 936 对 UTF-8 路径直接转换失败（_popen 返回 NULL，连损毁文件都
  不会留）。decode_audio 与渲染端 VideoPipe 均改为 CreateProcessW 直起
  （UTF-16 参数 + 匿名管道）。**句柄继承陷阱**：子进程复制所有可继承句柄，
  管道两端都泄露会让 EOF 永不到达（收尾挂死）——我方持有的端必须
  SetHandleInformation(HANDLE_FLAG_INHERIT, 0)。
- ACP=936（GBK），不是 UTF-8——旧文「本机 UTF-8 代码页」不成立。真实
  cmd/PowerShell 用户敲中文参数走 CreateProcessW 正常；harness 里敲中文
  参数会双重损坏，测试请用 node spawnSync 的 UTF-16 参数通道（见
  tmp/audio/ab.mjs 的做法，tmp/ 不入库）。
- git 看得见 `nul.png` 幽灵文件而常规 API 看不见（NTFS 保留名）——用
  `cmd /c del "\\.\F:\项目\Chamina\nul.png"` 删（本次已处理）。

## 6. 文件地图

```
cpp/
├─ build.bat           一键构建 + 冒烟（需 -lshell32，见 §5）
├─ README.md           用法（库 + CLI）/开关/架构速览
├─ src/chamina.h       单头库交付物（~4800 行 stb 风格；引擎体逐字来自
│                      原 chamina.cpp 第 16–4177 行 + 公共 API 实现）
├─ src/chamina_cli.cpp CLI 薄壳（~120 行：UTF-8 argv 解码 + 参数解析 + 调库）
├─ example/main.cpp    最小嵌入示例（自身定义 IMPLEMENTATION，出静帧）
└─ vendor/             nlohmann/json.hpp, stb_truetype.h, earcut.hpp, stb_image_write.h
dist/chamina-cpp.exe   交付产物（静态；CLI 薄壳 + 库，gen/audio/渲染三合一）
dist/chamina-example.exe 嵌入示例产物
tmp/split.cjs          一次性拆分工具（源料 tmp/chamina_head.txt、tmp/chamina_api.inc；
                       chamina.cpp 已删，此后 chamina.h 即唯一直接编辑的引擎源）
src/native/            Rust 参考实现（对照验收用）
js/                    作者工具链——lrc2scene（含 layout.mjs）、audio-env 已全部
                       C++ 化（gen field/groups/whip 全量）；未 C++ 化：editor
docs/cpp-migration.md  全部决策与实施记录（§一~§十四）
docs/session-summary.md §十二~§十四（v10 特效、Rust/C++ 落地）
scenes/bird.json       主测试场景（30fps/11010 帧/314 相机 stop/34 背景键）
scenes/bird.lrc        gen 的真实语料（与 node 版字节级一致）
target/chamina/        audio_env.json、native_*.png 金标准、场景备份
```

## 7. 建议的工作队列（接手后按序）

1. ~~验证 §0 的 resize 修复~~（已完成，2026-10-28，见 §0）。
2. ~~§3 的「待做优化」1/3/4~~（已完成：blur 静态缓冲+死代码跳过、ACES fit
   LUT、文字块预剔除；回归全部像素级零漂移，见 §3 表）。剩余项见 §3 待做。
3. 功能缺口（对照 JS 版逐一移植）：
   - ~~`lrc2scene` → `chamina-cpp gen`~~（**已完成**，2026-10-28，single
     布局默认路径字节级一致，见 §0a 与 cpp-migration §十；第二阶段
     `--layout field` / `--groups auto` / whip 全量也已补齐，见 §十四——
     gen 至此全量接管 node lrc2scene）；
   - ~~`audio-env` → `chamina-cpp audio`~~（**已完成**，2026-10-28，见 §0b：
     ffmpeg 解码 + 手写同构 FFT，输出字节级一致，零新 vendored 库）；
   - editor（Web GUI）——低优先级。
4. ~~崩溃/边界 fuzz 第一轮~~（**已完成**，2026-10-28，48 例全清，见 §0c 与
   cpp-migration §十二）。
   ~~单头库化 chamina.h~~（**已完成**，2026-10-28，见 §0d 与 cpp-migration
   §十三）。~~Phase 2：完全自包含 amalgamation~~（**已完成**，2026-10-28：
   `cpp/tools/amalgamate.cjs` → dist/chamina.h，验证 9/9，见
   cpp-migration §十三）。~~gen 旗标面 fuzz 第二轮~~（**已完成**，
   2026-10-29：首种子 48/48 绿后换种子抓出 JS 越界语义三连 bug，修复后
   两种子全绿，见 §0e 与 cpp-migration §十五；harness 不入库，修复在
   chamina.h）。gen 至此全量接管 node lrc2scene 且两轮 fuzz 验收；
   fuzz 第三面（stamp 宽域 + astral/emoji）也已全绿，累计 304/304
   （cpp-migration §十五）。
5. ~~SIMD/wgpu 大项立接口~~（**已完成**，2026-10-31，见 cpp-migration
   §十六）：AVX2 运行时分发（target_clones 在 PE 不可用→手工函数指针）、
   Tier A 位精确双内核（tonemap/grade）+ pack 并行，字节门禁 8/8 全等。
   ~~skydome 全幅 Tier A~~（**已完成**，2026-10-31 同日第二批：字节 8/8
   全等，render 70.7→45.0ms、skydome 57.1→32.1ms、视频段 5.37→7.24 fps）。
   ~~云场 prepass lane 化~~（**已完成**，同日第三批：fbm2 跨 lane、域内
   零 sinf；render 71.1→32.7ms、skydome 58.8→20.2ms、视频段 5.40→8.00
   fps，字节 8/8 全等）。
   ~~atan2f 向量多项式~~（**已完成**，同日第四批，首例 Tier B：
   `atan2f_vec256`，探针 2.384e-7 rad；render 32.7→28.7ms、skydome
   20.2→15.9ms、视频段 8.00→8.29 fps；双枝门禁 OK）。
   **裁定收尾**：cell_tw sinf 与 byte-pack 内核均驳回（收益/风险比不足，
   依据见 cpp-migration §十六）；SIMD CPU 侧切片全部完成。wgpu 后端按
   同签名帧函数段前缀约定暂不引入。
6. 每完成一个大项，更新 `docs/cpp-migration.md` §八~§十六 与本文件。
   可选：更多 fuzz 种子长跑；editor（Web GUI）低优先级。

## 8. 用户偏好（长期有效）

- 视频成片按需；**交付物是软件**。
- 不要问「要不要继续」——自主循环：改 → 编译 → 回归 → 记录。
- 中文交流；文档写中文；代码注释英文（跟仓库惯例）。
- 一切公式改动必须同时核对 js/anim.mjs 与 src/animation.rs 两处权威。
