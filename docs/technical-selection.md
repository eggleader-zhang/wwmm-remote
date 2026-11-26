# WWMM Remote 技术选型

2026-11-26 · 首轮架构决策

**建议首版用 C++20 和 Qt 6 Widgets 共用界面与会话核心。原生 libwebrtc 通过局域网 UDP 直连，默认用 H.264 硬件编解码传桌面。** macOS 和 Windows 各自只装一个应用，都能发起控制，也都能共享当前桌面。Mac → Windows、Windows → Mac、Mac → Mac、Windows → Windows 共用同一套会话协议，平台差异集中在采集、显示和输入适配。

第一轮以同一有线或 Wi-Fi 网段、已登录用户的活动桌面、1080p60 SDR 为基线。硬件先覆盖 Apple Silicon Mac 和 Windows 11 x64，包含 Home 版。最低系统版本、Intel Mac 和 Windows on Arm 的兼容范围以真机验证为准。

## 办公场景的实现栈

**首选一份 C++20 + Qt 6 Widgets 桌面应用，局域网会话走原生 libwebrtc，视频使用平台硬件编解码和 GPU 呈现。** 设备列表、配对提示、会话窗口和工具栏写一次，由同一 CMake 工程构建 macOS 和 Windows 安装包。两端使用相同的协议、码率与丢包恢复策略、输入映射规则和测试用例。新增一个配对步骤或修改一条编辑快捷键规则，都只改一份业务代码。

### 桌面方案对比

| 方案 | 两端可共用的代码 | 对低延迟视频和维护的影响 |
| --- | --- | --- |
| **Qt 6 Widgets + C++20（首选）** | UI、会话状态、输入规则、网络和媒体调度均为 C++ | Qt 提供现成的原生子窗口嵌入方式，可把视频表面直接放进共享的会话窗口。本机试验验证了 Mac 上的嵌入和 GPU 纹理路径，窗口内存数据见下文。[Qt 原生窗口嵌入示例](https://doc.qt.io/qt-6/qtdoc-demos-windowhosting-example.html) |
| wxWidgets + C++20 | 同样可以共用界面和核心，常用控件调用各系统的原生实现 | 适合设备列表和表单。视频仍可接同一原生后端。它是很接近的备选，需按相同方法验证视频子窗口的焦点、中文组词和内存。Qt 在本项目中已验证的原生窗口路径是选择 Qt 的依据。[wxWidgets 的原生控件策略](https://wxwidgets.org/about/) |
| Flutter + 原生媒体核心 | Dart 共用界面，C++ 核心通过插件或 FFI 接入 | 外部纹理可传递 GPU 资源，具有高性能实现路径。需要额外维护 Dart/C++ 边界、两端纹理注册及 Flutter 引擎的合成时序。对当前几个简单窗口，跨语言带来的维护成本较高。[Flutter 桌面支持](https://docs.flutter.dev/platform-integration/desktop) · [Windows GPU 纹理接口](https://api.flutter.dev/windows-embedder/flutter__texture__registrar_8h_source.html) · [Mac 外部纹理](https://api.flutter.dev/macos-embedder/protocol_flutter_texture-p.html) |
| SwiftUI + WinUI 3，或 AppKit + Win32 | 共用 C++ 网络、输入规则和媒体核心，两端各写界面 | 可以直连相同的 Metal/D3D11 视频后端，适合作为性能对照。配对、会话工具栏和焦点切换需要分别实现、分别回归。下文的原生对照数据来自 AppKit。 |

Moonlight PC 也将 Qt 管理界面与串流窗口分开，项目用的是 Qt Quick 和 SDL。它提供了一个已在 Mac 和 Windows 落地的结构参考。本项目的 Qt Widgets 选择和延迟指标仍以自己的实现验证。[Moonlight 会话窗口源码](https://github.com/moonlight-stream/moonlight-qt/blob/master/app/streaming/session.cpp) · [Moonlight 支持平台](https://github.com/moonlight-stream/moonlight-qt)

### 共享代码与平台适配的边界

| 共享部分 | macOS 适配 | Windows 适配 |
| --- | --- | --- |
| Qt 界面、设备和配对流程、权限状态展示 | Bonjour 接口、系统授权、设备密钥存储与打包 | DNS-SD 接口、系统授权、设备密钥存储与打包 |
| C++ 会话、原生 libwebrtc、帧调度、码率和恢复策略 | ScreenCaptureKit 采集，VideoToolbox 硬编硬解，Metal 呈现 | DXGI 采集，Media Foundation 硬编硬解，D3D11 呈现 |
| 键盘动作、物理键和文本事件模型、断线释放逻辑 | 读取原生键码与输入法事件，Quartz 执行远端输入 | 读取 Windows 扫描码与输入法事件，SendInput 执行远端输入 |

Qt 负责视频区域的位置、尺寸和焦点。0.3.0 的查看窗口通过 `QWidget::createWindowContainer` 嵌入 `QWindow`，将其 NSView 或 HWND 交给平台呈现器。Mac 后端将解码得到的 CVPixelBuffer 映射为 Metal 纹理，在 CAMetalLayer 呈现。Windows 后端用同一 D3D11 设备处理解码纹理，转换成显示格式并提交交换链。格式转换留在 GPU 上，界面线程向呈现器传递压缩帧和窗口状态。本版在 CMake 中按固定提交编译所用的 libwebrtc 媒体模块，Qt、协议和平台适配由同一个工程构建。[Qt 窗口容器](https://doc.qt.io/qt-6/qwidget.html#createWindowContainer) · [Windows D3D11 解码](https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation) · [所用 WebRTC 源码](https://github.com/webrtc-mirror/webrtc/tree/3710345e71ad3a7c1f4ed4efef152fd4fc24869a)

工具栏放在视频区域旁边，原生子窗口的叠放和焦点行为由系统决定。焦点进入视频区域后，由该视图接收原生键盘和输入法事件，交给同一套映射规则。Qt 默认在 Mac 上把 Command 表示为 `ControlModifier`，远控输入以原生键码和修饰键为准，避免第二次交换。关闭窗口或断开时，先停用呈现并释放按住的键，原生视图保持存活到 Qt 容器销毁。[Qt 容器限制](https://doc.qt.io/qt-6/qwidget.html#createWindowContainer) · [Qt 修饰键定义](https://doc.qt.io/qt-6/qt.html#KeyboardModifier-enum)

按键和点击从窗口事件回调立即写入 TLS 连接，指针移动交给会话的 UDP 线程。被控端收到输入后交给 Quartz 或 SendInput。查询前台控件类型在后台执行，键鼠和编码队列分别处理。授权建立会话后才创建媒体线程、编解码器和纹理池，会话结束即释放这些资源。

已连接的静止桌面采用不同策略。保留采集监听和已预热的编解码器，跳过重复画面。下一次输入改变了屏幕，随即提交新画面编码，避免静止后的第一次打字等低频轮询或重建编码器。可见的光标闪烁、选区和通知也算桌面变化。若回收会话资源会拖慢首键回显，优先保留预热状态，从重复的整屏副本中节省内存。[ScreenCaptureKit 静止帧状态](https://developer.apple.com/documentation/screencapturekit/scframestatus/idle) · [DXGI 桌面更新通知](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-acquirenextframe)

在 M3 Pro 上用 `CVPixelBufferCreate` 分配带 IOSurface 的表面，通过 `IOSurfaceGetAllocSize` 读取单张分配大小。1080p 的 NV12、BGRA 分别为 **2.97、7.91 MiB**，4K UHD 分别为 **11.87、31.64 MiB**。因此采集、编码、解码和显示优先复用兼容的 GPU 表面，格式转换在 GPU 上完成。仅一张多余的 4K BGRA 整屏副本就会占用约 32 MiB。这些数字仅覆盖图像表面，完整进程内存按下文预算实测。[Apple IOSurface 分配大小](https://developer.apple.com/documentation/iosurface/iosurfacegetallocsize%28_%3A%29)

## 画面和输入分别怎么传

| 内容 | 首版选择 | 对操作的影响 |
| --- | --- | --- |
| 桌面画面 | 原生 libwebrtc 的 RTP/RTCP，线上用 SRTP/SRTCP over UDP | 丢包时利用时限内的重传和关键帧请求恢复。拥塞控制与发包节奏交给成熟实现，接收端选交互式低延迟播放策略。[RTP 媒体传输](https://www.rfc-editor.org/rfc/rfc8834.html) · [libwebrtc 播放延迟设计](https://webrtc.googlesource.com/src/+/main/docs/native-code/rtp-hdrext/playout-delay/README.md) |
| 按键、鼠标按下和抬起、滚轮、已确认文本 | 复用双向认证的 TLS 1.3/TCP 连接，启用 TCP_NODELAY | 按下和抬起按顺序到达。点击与滚轮自带主屏幕绝对坐标，输入直接写入网络。 |
| 鼠标绝对移动 | 复用媒体 UDP 套接字，通过 SRTCP 应用报文保护位置和序号 | 首个位置立即发送，连续位置按 4 ms 合并。只应用当前点击或拖动阶段的最新位置。停止移动 35 ms 后在 TLS 上补发最终位置。 |

0.3.0 引入视频通道。0.4.0 在已有的 TLS 和 UDP 会话上增加了两种输入交付方式，两端直接使用已建立的连接和会话密钥。查看和键鼠控制分别授权。

### 候选比较

| 方案 | 决定及依据 |
| --- | --- |
| 原生 WebRTC 局域网直连 | **首选媒体方案。** 提供 UDP 媒体路径、丢包反馈和码率调节。应用复用已配对的连接传输键鼠。整机表现仍需双机对测。[RTP 媒体传输](https://www.rfc-editor.org/rfc/rfc8834.html) |
| Sunshine + Moonlight（GameStream 系） | **流畅度对照。** 两端覆盖 Mac 和 Windows，已经有硬件视频路径。Sunshine 的 macOS 指南还记录了用右 Option 代替 Command 的限制，与本项目的键位目标直接相关。若实测这套媒体路径明显领先，可复用媒体部分并保留自己的输入层，届时一起评估上游 GPL-3.0 许可。[Sunshine 平台矩阵](https://docs.lizardbyte.dev/projects/sunshine/latest/) · [macOS 按键说明](https://docs.lizardbyte.dev/projects/sunshine/v2026.516.143833/md_docs_2getting__started.html) · [Moonlight PC](https://github.com/moonlight-stream/moonlight-qt) |
| QUIC DATAGRAM + 可靠流 | 可在同一加密连接里承载画面和按键。DATAGRAM 也受拥塞控制和路径 MTU 限制，若采用它，需要另外实现媒体分片、帧截止时间与丢帧恢复。保留作传输对照，缺少端到端优势数据时优先使用现成 RTP 媒体栈。[RFC 9221](https://www.rfc-editor.org/rfc/rfc9221.html) |
| 系统 RDP、Apple 高性能屏幕共享、标准 VNC/RFB | Windows Home 的内建 RDP 只支持主控角色，Apple 的高性能模式对两端 Mac 有要求。标准 RFB 的矩形更新适合静态管理桌面，持续运动场景仍需要另配编码和反馈策略。统一走硬件视频路径可以覆盖四个方向。[微软 RDP 版本说明](https://learn.microsoft.com/en-us/windows-server/remote/remote-desktop-services/remotepc/remote-desktop-allow-access) · [Apple 屏幕共享模式](https://support.apple.com/zh-cn/guide/mac-help/mchl1883115d/27/mac/27) · [RFB 规范](https://www.rfc-editor.org/rfc/rfc6143.html) |

## 两个平台的视频路径

| 平台 | 被控端采集、编码与执行输入 | 主控端解码、显示与读取输入 |
| --- | --- | --- |
| macOS | ScreenCaptureKit 取得 CVPixelBuffer，在兼容格式下直接交给 VideoToolbox 硬编。Quartz CGEvent 执行远端输入 | VideoToolbox 硬解，Metal 显示。Qt 会话窗口内的原生视频子视图负责焦点下的键盘和输入法。[Apple 采集接口](https://developer.apple.com/documentation/screencapturekit) · [VideoToolbox 低延迟编码](https://developer.apple.com/videos/play/wwdc2021/10158/) · [CGKeyCode](https://developer.apple.com/documentation/coregraphics/cgkeycode) |
| Windows | DXGI Desktop Duplication 取得 D3D11 纹理，在 GPU 上转 NV12，接入支持该设备的 Media Foundation 硬件 MFT。SendInput 执行远端输入 | 硬解到 D3D11 表面，原生子窗口的交换链显示。该子窗口接收 Win32 键鼠和输入法事件。[DXGI 采集与光标](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api) · [硬件 MFT](https://learn.microsoft.com/en-us/windows/win32/medfound/hardware-mfts) · [D3D11 解码](https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation) |

视频从 H.264、8 位 4:2:0、60 fps 起测，1080p 的起始目标码率取 20 Mb/s。连接时探测实际可用的编解码器、profile、level、分辨率和刷新率。两端支持 H.264 High 时可使用 Apple 的低延迟模式，关闭帧重排。Windows 申请低延迟模式并验证驱动实际交付情况。HEVC 需要在固定版本的 libwebrtc 中启用，验证 RTP 打包、协商和两端硬件，再以同等画质下的延迟决定是否自动选用。缺少合适硬件时验证软件 H.264 和较低分辨率、帧率的兼容档。AV1 留到硬件覆盖和实测收益明确后评估。[libwebrtc 编解码构建选项](https://webrtc.googlesource.com/src/+/refs/heads/main/webrtc.gni) · [Apple 低延迟模式约束](https://developer.apple.com/documentation/videotoolbox/kvtvideoencoderspecification_enablelowlatencyratecontrol) · [微软低延迟模式](https://learn.microsoft.com/en-us/windows/win32/medfound/codecapi-avlowlatencymode) · [微软 H.264 解码能力](https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder)

60 Hz 下每积压一帧就多约 16.7 ms。应用只保留最新的待编码原始帧，监测是否有 GPU 回读或隐式转 I420。ScreenCaptureKit 自身保留系统要求的缓冲深度。0.3.0 的接收端在 GPU 画面就绪后提交呈现，遇到无法修复的丢包会请求关键帧。Wi-Fi 拥塞时先调整码率。两端使用 1.4 或更新版本时，持续拥塞还会触发分辨率降档，传输恢复稳定后在用户选择的上限内逐级回升。[ScreenCaptureKit 队列深度](https://developer.apple.com/documentation/screencapturekit/scstreamconfiguration/queuedepth) · [libwebrtc 原生帧缓冲](https://webrtc.googlesource.com/src/+/refs/heads/main/api/video/video_frame_buffer.h)

桌面画质单独看小字。按主控窗口的实际像素与远端 Retina/DPI 协商合适尺寸，避免反复缩放。滚动停下后，用最后一张源图补一次清晰刷新。4:2:0 会损失细小彩色笔画的色度细节。4:4:4 作为独立能力，只有相应 profile、两端硬件和延迟都通过验证时才启用。[微软桌面色度说明](https://learn.microsoft.com/en-us/azure/virtual-desktop/graphics-chroma-value-increase-4-4-4)

## 开箱即用的按键规则

主控端读取当前键盘布局、实际修饰键和左右键状态。普通物理键用 USB HID Usage 表示，被控端再转换成 macOS 键码或 Windows 扫描码。跨系统的常用快捷键先识别编辑动作，再由被控端按前台应用生成目标组合。单独按下或未命中快捷键时，按对应修饰键发送，保留左右侧，即 `Control ↔ Ctrl`、`Option ↔ Alt`、`Command ↔ Win`。识别以系统上报的键位和布局为准，也覆盖外接另一平台键盘的情况。[USB HID 键盘用法表](https://www.usb.org/sites/default/files/hut1_4.pdf) · [Windows 扫描码和 Unicode 输入](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-keybdinput)

| 使用场景 | 默认行为 |
| --- | --- |
| Mac → Windows 普通编辑器 | `⌘C / V / X / Z / A / F / S` 分别触发 Windows 的 `Ctrl` 对应组合。Mac 键帽上的 `Delete` 执行退格，`Fn+Delete` 在系统给出前向删除事件时执行向前删除。 |
| Windows → Mac 普通编辑器 | 同一组 `Ctrl` 编辑快捷键转成 `⌘` 组合。`Backspace` 和 `Delete` 保持各自的删除方向。 |
| Mac → Mac、Windows → Windows | 快捷键和左右修饰键保持本机语义。方向键、功能键、游戏及未知应用的原始输入保留物理键位。 |
| 切换应用 | 主控系统允许远控窗口接收该组合时，`⌘Tab` 与 `Alt+Tab` 互转。系统保留的组合可通过会话内的“切换远端应用”动作发送。 |
| 终端 | 两个方向都将 `Ctrl+C` 作为 `Control+C` 键交给远端终端，由终端依其选择状态和绑定处理复制或中断。Mac 发出的 `⌘C` 在默认 Windows Terminal 生成明确的复制组合 `Ctrl+Shift+C`，反向的 `Ctrl+Shift+C` 在 Mac 终端生成 `⌘C`。按被控端前台应用及可识别的焦点选择规则，未知上下文回到物理键。[Windows 控制台中断](https://learn.microsoft.com/en-us/windows/console/ctrl-c-and-ctrl-break-signals) · [Windows Terminal 快捷键](https://learn.microsoft.com/en-us/windows/terminal/customize-settings/actions) |
| 中文输入法、重音键和 AltGr | 在可识别的标准文本框，主控端负责组词和候选，只发送一次确认上屏的 Unicode 文本。由本地输入法消费的键不再转发，避免重复上屏。原始输入程序和未知控件由被控端输入法处理物理键。[AppKit 文本输入](https://developer.apple.com/documentation/appkit/nstextinputclient) · [Windows 组合输入](https://learn.microsoft.com/en-us/windows/win32/intl/wm-ime-composition) |

被控端在焦点变化时向主控端报告上下文类型，例如标准文本框、终端或未知控件。主控端据此选择文本提交或物理键，焦点离开时取消尚未提交的组词。

同一次组合从按下到松开沿用同一条映射，只发送该组合实际需要的目标修饰键，避免一次 `⌘C` 顺带弹出 Windows 开始菜单。跟踪本次会话按住的键和鼠标按钮，失焦、断线或切换角色时在被控端释放，避免 Ctrl 被卡住。只在远控窗口获得焦点时接收输入。Fn 等特殊键仅在硬件或系统上报时转发。

## 拔掉外网也能连接

两端在有线或 Wi-Fi 接口上通过 mDNS/DNS-SD 发布和发现设备。首次连接由被控端显示短时有效的一次性配对码，主控方输入后用 SPAKE2+ 验证并绑定双方设备公钥，限制失败尝试次数。第三批直接复用已配对的 Ed25519 公钥完成双向 TLS 1.3 认证。双方通过这个会话传递只读请求、批准、UDP 端口与结束消息，并用 TLS exporter 派生本次 SRTP 和 SRTCP 的密钥。共享端可以看到当前查看者，随时停止或撤销信任。[mDNS](https://www.rfc-editor.org/rfc/rfc6762.html) · [DNS-SD](https://www.rfc-editor.org/rfc/rfc6763.html) · [SPAKE2+](https://www.rfc-editor.org/rfc/rfc9383.html) · [TLS 1.3 exporter](https://www.rfc-editor.org/rfc/rfc8446.html#section-7.5)

本版只接收允许接口和直连前缀内的控制与媒体报文。UDP 对端地址取自已认证的 TCP 连接，端口在 TLS 内交换，媒体套接字绑定相同接口，IP 跳数为 1。这个局域网专用协议只在两台 WWMM Remote 之间协商，运行时无需 ICE、SDP、STUN/TURN 或公网服务。监听接口、网段或信任发生变化时重新核验。首轮自动发现范围是同一链路，跨 VLAN 的企业网络另需界定可信网段。

macOS 在被控时按系统提示授予录屏和辅助功能权限，适用版本还会询问本地网络权限。Windows 普通用户会话受完整性级别和安全桌面限制。遇到锁屏或提权提示时显示具体状态。授权由系统和被控方决定，键位映射本身全自动。[Apple 录屏权限](https://support.apple.com/zh-cn/guide/mac-help/mchld6aa7d23/27/mac/27) · [Apple 辅助功能授权](https://support.apple.com/zh-sg/guide/mac-help/mh43185/mac) · [Apple 本地网络权限](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy) · [SendInput 权限](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput) · [安全桌面采集限制](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutput1-duplicateoutput)

## Mac 上的界面和视频对照

在 Apple M3 Pro、macOS 27.2.1 上，用相同的 Objective-C++ 源码编译两个经 `-O2` 优化的程序。一个用 AppKit，一个用 Qt Widgets 6.12.1。两者均创建一个 640×400 的透明、不激活窗口，包含两个设备选项和一个连接按钮。每个进程独立启动，处理约 0.5 秒事件后读取 `task_vm_info.phys_footprint`，此时还未创建解码器。完整测量和复跑各做一组，每组每种窗口交错启动五次，合计每种十次。

| 窗口 | 进程物理内存中位数 | 十次范围 |
| --- | ---: | ---: |
| AppKit | 16.57 MiB | 16.22–16.72 MiB |
| Qt 6 Widgets | 28.71 MiB | 28.61–28.89 MiB |

这个小窗口采用 Qt 的增量约为 **12.14 MiB**。50 MiB 是包含设备发现和监听在内的完整应用待机预算，仍需在两个平台实测。上述测量只覆盖这两种 Mac 窗口。

随后两种程序各自把同一个实现的 CAMetalLayer 子视图接入窗口，用 VideoToolbox 强制硬解同一段 6 帧 1080p H.264 视频。每帧 NV12 的两个 Metal 纹理都通过了与解码输出相同的 IOSurface ID 检查，并在 GPU 完成离屏颜色转换。这个实验验证解码纹理可以复用，覆盖范围到离屏 GPU 输出为止。0.3.0 的整条视频链路和真实查看窗口测量见项目 README，输入焦点和四向真机性能另行验收。[对照测量源码](../research/macos-ui-compare.mm)

<details>
<summary>在 Mac 上复现上述对照</summary>

在仓库根目录运行，`wwmm_qt_kit` 指向含 `lib` 与 `plugins` 的 Qt 6 macOS SDK。以下命令在临时目录生成合成视频和测试程序，完成一组交错测量。

```bash
set -e
: "${wwmm_qt_kit:?请先设置 Qt 6 macOS SDK 路径}"
wwmm_tmp=$(mktemp -d -t wwmm-ui)
args=(-std=c++20 -O2 -fobjc-arc -Wno-deprecated-declarations research/macos-ui-compare.mm
      -framework AppKit -framework Foundation -framework QuartzCore -framework AVFoundation
      -framework CoreMedia -framework CoreVideo -framework VideoToolbox -framework Metal
      -framework IOSurface -framework CoreFoundation)
xcrun clang++ "${args[@]}" -o "$wwmm_tmp/native"
xcrun clang++ "${args[@]}" -DWWMM_USE_QT -F"$wwmm_qt_kit/lib" -framework QtWidgets \
  -framework QtGui -framework QtCore -Wl,-rpath,"$wwmm_qt_kit/lib" -o "$wwmm_tmp/qt"
ffmpeg -nostdin -v error -f lavfi -i "testsrc2=size=1920x1080:rate=60,format=nv12" \
  -frames:v 6 -an -c:v h264_videotoolbox -allow_sw 0 -realtime 1 -bf 0 -b:v 20M "$wwmm_tmp/sample.mp4"
for variant in native qt qt native qt native native qt native qt; do
  QT_PLUGIN_PATH="$wwmm_qt_kit/plugins" "$wwmm_tmp/$variant" "$wwmm_tmp/sample.mp4"
done
```

</details>

## 本机编码实测

Apple M3 Pro、macOS 27.2.1，使用同一段预生成的 1920×1080 NV12 灰阶滚动图案，以 60 fps 送入 VideoToolbox。两种编码都要求硬件和低延迟模式，目标平均码率 20 Mb/s。完整测量和复跑各做三轮，每种编码累计六轮。每轮 360 帧，前 60 帧暖机，余下 300 帧统计从提交到收到编码回调的时间。表中是六轮各自分位数的范围，每种编码共 1,800 个稳态样本。

| 编码 | 每轮 P50 | 每轮 P95 | 实际视频载荷 |
| --- | ---: | ---: | ---: |
| H.264 | 8.41–8.67 ms | 10.90–12.00 ms | 约 0.95 Mb/s |
| HEVC | 8.83–8.93 ms | 11.32–12.23 ms | 约 0.10 Mb/s |

每轮提交的 360 帧均返回了编码结果。这个简单图案很容易压缩，上表只用于判断这台 Mac 的编码时间预算。实际桌面码率、彩色小字质量、Windows 路径和整机操作延迟仍要分别测量。在仓库根目录运行以下命令即可复现每种编码的三轮测试，输入只用合成图案。[测量源码](../research/videotoolbox-encode.mm) · [Apple 硬编要求](https://developer.apple.com/documentation/videotoolbox/kvtvideoencoderspecification_requirehardwareacceleratedvideoencoder)

```sh
research_tmp=$(mktemp -d -t wwmm-vt)
xcrun clang++ -std=c++20 -O2 -fobjc-arc research/videotoolbox-encode.mm \
  -framework Foundation -framework CoreFoundation -framework CoreMedia \
  -framework CoreVideo -framework VideoToolbox -o "$research_tmp/vt-encode"
for codec in h264 hevc hevc h264 h264 hevc; do
  "$research_tmp/vt-encode" "$codec" || exit 1
done
```

## 四向真机验收

以下数值是建议验收门槛。四个方向分别出结果，优先做跨系统互控，再用两台 Mac 和两台 Windows 机器分别验证同系统组合。

| 测什么 | 测法和门槛 |
| --- | --- |
| 操作延迟 | 1080p60、60 Hz 显示、硬编硬解。用 240 fps 或更快相机同框拍摄物理按键指示与主控屏上由远端应用触发的变色，排除本地光标。每方向至少 200 次，有线千兆 P50 ≤ 35 ms、P95 ≤ 50 ms，独占 5/6 GHz Wi-Fi 的 P95 ≤ 75 ms。 |
| Qt 窗口对照 | 同一接收机、同一码流、同一原生呈现器，分别嵌入 Qt 容器和原生独立窗口。记录解码回调至实际呈现的 P95、整帧 CPU 回读次数和在途帧数。若 Qt 稳定多出一个刷新周期（60 Hz 约 16.7 ms），优先排查额外合成、队列及全屏切换。四向复测输入到屏幕的指标，并验证窗口缩放、焦点切换和中文输入法候选。 |
| 静止后的首个操作 | 保持会话并停在无动画的测试页，每次静置 30 秒后敲一个字符或点击单元格。单独统计首个远端结果回显的 P95，使用与连续操作相同的有线 50 ms 目标。中文输入法测确认上屏之后的远端字形。 |
| 内存 | 原型建议预算为待机每端 ≤ 50 MiB，1080p60 单屏单向会话每端 ≤ 150 MiB。主控、被控分别测，运行十分钟逐秒采样取稳态 P95，另记首次连接峰值。反复连接、断开 20 次后，待机占用应能回到预算以内，记录有无持续增长。macOS 记录进程 physical footprint，Windows 记录私有工作集并附私有提交量。两端另列 GPU 分配，并标明与统一内存重叠的部分。[Apple 内存占用指标](https://developer.apple.com/videos/play/wwdc2022/10106/) · [Windows 进程指标](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex2) · [DXGI 显存统计](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/ns-dxgi1_4-dxgi_query_video_memory_info) |
| 帧率与画质 | 用滚动代码、拖动窗口和全屏 60 fps 视频分别测试。连续运动十分钟，干净有线下实际呈现 ≥ 59 fps，帧间隔 P95 ≤ 25 ms。对齐源图与接收端截图，检查 12–16 px 中文、彩色代码与细线。停止滚动 500 ms 后，固定文本样本应可逐字读对。在 Wi-Fi 抖动和 1% 可控丢包下分开记录长帧、画面恢复和输入是否卡住。 |
| 按键体验 | 四向逐项跑复制、粘贴、撤销、终端中断、按住拖动、失焦和断线。加测中英文输入法、ANSI/ISO/JIS、德语 AltGr、外接另一平台键盘、Retina 与 Windows 150% 缩放。键位设置保持默认，验收要求零重复输入、零残留按键、点击命中目标。 |
| 纯局域网 | 断开路由器 WAN、清空本应用的配对记录，从发现、配对到双向控制完整跑通。检查应用流量全部在本地，未配对设备无法观看或注入输入，换到其他网络会重新验证。 |

对照测试固定硬件、显示刷新率、编码规格、实际码率、加密状态和桌面动作，同时记录采集、编码、网络 RTT、接收缓冲、解码、呈现各段耗时。如果原型在同等画质下反复比 Sunshine + Moonlight 多一帧及以上，先定位差异，再用同一份已编码视频单独对照媒体传输。如果相同码流下 GameStream 的传输仍明显领先，且四向均能达到上述门槛，就采用该媒体路径。最终以四向实机的操作延迟、帧稳定性和文字可读性定案。[WebRTC 统计指标](https://www.w3.org/TR/webrtc-stats/)
