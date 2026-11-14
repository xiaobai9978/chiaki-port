# Chiaki Brick Pro — PS5 串流掌机移植

把 chiaki-ng 移植到 TrimUI Brick（Allwinner A133 / aarch64 / SDL2）上的自制前端。
单文件 SDL2 UI + 直接调用 libchiaki，目标是低延迟 540p/720p 串流。

## 目录

| 路径 | 说明 |
| --- | --- |
| `port/` | 移植层源码（**核心**）：`main.c` UI/输入、`stream.c` 会话/解码/震动、`ui.c` 文本绘制 |
| `port/package/` | 掌机侧包结构（`config.json` / `launch.sh` / `config.ini`） |
| `port/assets/` | 字体与图标 |
| `research/` | A133 H.265 硬解研究资产：`cedar_src/` libcedarc 源码、`test_cedar*.c` 探针、测试码流 |
| `setup_ct.sh` | 编译容器（Debian aarch64 chroot）初始化脚本 |
| `xfer.py` | 服务器/掌机文件传输助手（paramiko） |
| `交接说明.md` | 环境、构建链、排障全记录 |

不入库（见 `.gitignore`）：`build/` 构建产物、`dist/` 发布包、`src/` 上游 chiaki-ng 源码、`hosts*.txt`（含 PS5 regkey/morning key 密钥）。

## 构建

```bash
# 容器内 /opt/arm64/build
sh build_fast.sh            # cmake -G Ninja ../port && ninja -j6
# 产物 /opt/arm64/build/out/chiaki_brick
```

推送掌机：`md5 校验 → killall chiaki_brick → rm → put → md5sum 复核`（运行中的二进制受 ETXTBSY 保护，必须先 rm）。

## 当前状态

- **A133 cedar 硬件解码已打通并接入 chiaki**：H.264 / H.265 都走
  `port/src/hwdec.c`（运行时 dlopen，不需全志 SDK 头文件）。
  设置菜单第 7 项「硬解」或 `config.ini` 的 `hwdec=1` 开启，关掉即回退 ffmpeg 软解。
- 硬解帧以 NV12 直接交给 `SDL_UpdateNVTexture`（SDL 2.0.16+；掌机 2.30.8 的
  opengles2 渲染器已用 `research/test_nv12.c` 验证），不做逐像素 U/V 拆分；
  软解路径仍用 IYUV。构建链 SDL 较老，因此该函数用 `dlopen/dlsym` 运行时解析。
  相比拆分版 dec 约低 1ms，60 秒长测 3399 帧稳定、画面无偏色。
- 根因与逆向过程见 `research/`：`test_cedar2.c` 探针、Tina 5.0 `libcedarc/`
  头文件、`run_cedar.py` / `push_hwdec.py` 一键编译-推送-实测脚本。
  关键是 `VConfig` 必须是 208 字节且字段偏移与 `libvdecoder.so` 一致。
- 实机验证（PS5 192.168.50.116）：
  - H.265 720p60 连续 3 分钟：10458 帧成功 / 4 帧失败，
    fps 58–60，dec 9–10ms，RTT 3–4ms，lost=0；
    framebuffer 截屏确认画面与 OSD「H.265 · 硬解」均正常。
  - H.264 540p60：fps=60，dec=3ms。
  - H.265 1080p30 @10000kbps：fps 29–31，dec 18–19ms，lost=0，529 帧成功
    （1080 被 cedar 对齐成 1088 行后裁回，模块已处理）。
- 软解 540p60 时 `dec≈15ms` 曾是帧率瓶颈，硬解后不再是瓶颈。

### 软解 / 硬解同参数实测对比（720p60 H.265 @ 6000kbps，同一台 PS5）

| 模式 | fps | 解码 dec | 转换 conv | RTT | 丢包 |
| --- | --- | --- | --- | --- | --- |
| ffmpeg 软解 (`hwdec=0`) | 13–18 | 53–65ms | 2ms | 2ms | 0 |
| cedar 硬解 (`hwdec=1`) | 58–60 | 9–10ms | 0ms | 3ms | 0 |

约 **4 倍帧率、1/6 解码耗时**；软解在这个分辨率下根本跑不动，硬解才能满帧。
