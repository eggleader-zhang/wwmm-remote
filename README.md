# WWMM Remote

一款面向办公的纯局域网远程桌面工具。macOS 和 Windows 共用 C++20、Qt 6 Widgets 界面和网络核心。

在 GitHub Releases 可按芯片下载 macOS 或 Windows 的原生包。Mac 在「关于本机」中查看芯片或处理器，Windows 在「设置 → 系统 → 系统信息」中查看系统类型。[下载最新版本](https://github.com/eggleader-zhang/wwmm-remote/releases/latest)

## 第四批 · 键鼠控制与中文输入（0.4.0）

两端升级到 0.4.0 后，选中已配对的电脑，点「控制桌面」。对方点「允许本次控制」，看到主屏幕后即可操作。Mac 首次作为被控端，需要在「系统设置 → 隐私与安全性」中允许屏幕录制和辅助功能，授权后再请求一次。Windows 支持普通交互桌面，UAC 安全桌面和高权限窗口受系统限制。两端也可以继续选择「查看桌面」。

| 想验收什么 | 操作和预期 |
| --- | --- |
| 点击、拖选和滚动 | 调整控制窗口的大小，在文档内点击行首、拖选文字，然后滚轮或触控板滚动。画面边缘留白不响应点击，拖到边缘松开能结束选择。 |
| 办公快捷键 | 在远端文档复制、粘贴、撤销、重做、查找、保存，或把选中文字加粗。按主控电脑习惯的 `⌘` 或 `Ctrl` 组合即可，操作的是远端文档和剪贴板。文本框中也适配按词移动、行首和行尾。同系统保留本机组合。 |
| 中文 | 在远端记事本、文本编辑或标准文本框内，使用主控机的中文输入法打拼音、选词，再确认「会议纪要」。候选和组词留在本机，远端收到确认后的文字。终端和未识别控件使用被控机的键盘布局及输入法。 |
| 终端与切换应用 | 在默认 Windows Terminal 或 Mac 终端测试 `Ctrl+C` 中断。复制用主控端的 `⌘C` 或 `Ctrl+Shift+C`。本机系统接管切换应用快捷键时，点控制窗口底部的「切换远端应用」。 |
| 结束控制 | 按住 Shift 或正在拖选时切走控制窗口，再回到远端打字。已按下的键和鼠标按钮应已释放。双方都可结束会话。重新连接需要被控方再次同意。 |

按键、点击、滚轮和确认后的文字走已认证的 TLS 连接，启用 TCP_NODELAY，当次事件就提交给网络。鼠标移动走加密 UDP，首个位置立即发送，连续移动每 4 ms 最多更新一次。点击携带自己的绝对坐标和序号。指针停下后通过可靠通道补送最后位置。视频继续走硬件 H.264，键鼠发送不等下一帧。

本批在 Mac 上验证了真实加密回环会话、四种平台组合的快捷键映射、窗口失焦释放、中文组词与确认事件，并交叉构建了 Windows x64 版本。双机上的实际操作手感和端到端延迟留待四向真机验收。

## 第三批 · 只读桌面（0.3.0）

已配对的电脑可以请求查看对方的主屏幕。共享方每次确认，画面保持宽高比，最高 1920×1080、60 fps，包含主屏幕光标。这一版面向 Apple Silicon Mac 和 Windows 11 x64，需要共享端能硬编 H.264、查看端能硬解 H.264。

### 分步验收

| 步骤 | 操作 | 预期结果 |
| --- | --- | --- |
| 1. 准备两端 | 两台电脑接入同一有线或 Wi-Fi 网段，都打开 0.3.0。Mac 解压后打开 WWMM Remote，Windows 解压整个目录后打开 WWMM Remote.exe。在准备共享的电脑上勾选「允许局域网设备发现和检测本机」。 | 查看方能在「设备 → 附近设备」看见共享方。系统询问网络访问时，允许应用使用当前局域网。 |
| 2. 配对 | 已经相互信任的设备可直接到下一步。首次使用时，共享方点「显示本机配对码」，查看方选中它，输入八位数字并点「配对」。 | 两边的「本机信任的设备」都出现对方。 |
| 3. 验证同意 | 查看方在「附近设备」选中共享方，点「查看桌面」。共享方先点「拒绝」，再让查看方重试，点「允许本次查看」。 | 拒绝后请求结束。允许后共享方显示正在向谁共享，查看方打开只读窗口。Mac 首次共享时按系统提示授予「隐私与安全性 → 屏幕与系统音频录制」权限，如系统要求，重开应用后再请求一次。 |
| 4. 看办公画面 | 在共享方滚动一页有中英文小字的文档、移动光标，然后停在一行文字上。 | 查看方看到同一位置，可阅读停下来的文字。窗口底部显示最近一秒的帧数，静止时显示「保持当前画面」。 |
| 5. 停止、换向 | 共享方点「停止共享」，确认查看窗口关闭。将原查看方的接收开关打开，交换角色再做第 3、4 步。也可在会话中点「撤销本机信任」。 | 交换后由新的共享方确认。撤销后会话结束，再次查看前需重新配对。 |

四向验收分别记录 Mac 查看 Windows、Windows 查看 Mac、Mac 查看 Mac、Windows 查看 Windows 的结果。

如果配对成功但等不到第一帧，先看共享方的录屏授权或显卡提示，再检查系统防火墙是否允许 WWMM Remote 在当前局域网收发 UDP。「连接检测」测的是 TCP，视频使用会话分配的 UDP 端口。

### 画面如何传输

双方用配对时保存的 Ed25519 公钥完成双向 TLS 1.3 认证，在加密会话里确认查看权限、交换 UDP 端口，并为本次会话导出两个方向的媒体密钥。桌面协议只向同一直连网段的对端发送加密数据。视频采用原生 libwebrtc 的 H.264 RTP 分包、发包预算与 AIMD 码率控制，libSRTP 保护 RTP 画面及 RTCP 反馈。丢包时先做短时重传，需要时再请求关键帧。桌面静止后保留最后一帧。[TLS 1.3 密钥导出](https://www.rfc-editor.org/rfc/rfc8446.html#section-7.5) · [所用 WebRTC 源码](https://github.com/webrtc-mirror/webrtc/tree/3710345e71ad3a7c1f4ed4efef152fd4fc24869a) · [libSRTP](https://github.com/cisco/libsrtp/releases/tag/v2.8.1)

macOS 使用 ScreenCaptureKit、VideoToolbox 和 Metal。Windows 使用 DXGI Desktop Duplication、Media Foundation 和 D3D11。两端共享设备页、授权流程、查看窗口、网络协议和丢包处理。视频表面在会话中按需创建，采集、格式转换与呈现走原生 GPU 路径。[Apple 屏幕采集](https://developer.apple.com/documentation/screencapturekit) · [Windows 桌面复制](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api)

### 0.3.0 验证记录

在一台 M3 Pro、macOS 27.2.1 上，两个独立进程通过本机局域网接口传输 1920×1080 的合成滚动文档，送入 H.264 硬编，硬解后在应用的查看窗口用 Metal 呈现。连续运行 10 分钟，共提交 36,000 帧，每个进程采集 600 个物理内存样本。

| 指标 | 中位数 | P95 |
| --- | ---: | ---: |
| 源帧提交到 Metal 通知已呈现 | 23.16 ms | 40.30 ms |
| 共享端进程物理内存 | 41.06 MiB | 41.31 MiB |
| 查看端进程物理内存 | 148.11 MiB | 151.92 MiB |

预热后的实际呈现速率为 **60.00 fps**，相邻呈现回调间隔 P95 为 **29.16 ms**。查看端内存比 150 MiB 的原型预算高 1.92 MiB，回调间隔也高于 25 ms 的帧间隔参考值。这次计时从合成源帧提交开始，含呈现回调调度。系统录屏耗时、物理网络和真实按键到屏幕的延迟需要双机测量。[测试源码](research/desktop-video-bench.mm)

画质另用 14、16 像素的中英文和蓝色小字滚动样本测了三轮，每轮预热 45 帧，再统计 300 帧。源帧提交至硬解输出的 P95 为 16.31–17.06 ms。对齐源图和解码图后，亮度 PSNR 为 46.51–49.90 dB，色度 PSNR 为 47.91–49.85 dB，文字等高对比边缘的亮度平均绝对误差为 1.17–1.61 个灰阶（满量程 255）。

在 M3 Pro 上启用本机钥匙串、系统 DNS-SD 和实际局域网接口的测试开关，三套 Qt Test 连续三轮通过，每轮共 61 项，含各套的初始化和清理。IPv4、IPv6 都完成了双向认证、授权后的视频接收和撤销断开。只读桌面套件另连续运行 30 轮通过。

macOS arm64 开发包的最低部署目标是 14.4，解压后已验证应用与随包框架签名。Windows x64 应用及三套测试程序已交叉编译，便携包已核对 DLL 及其导入符号。真实主屏幕采集、Windows 显卡和四个方向的双机体验留待分步验收。

## 第二批 · 自动发现与配对（0.2.0）

1. 两台电脑接入同一有线或 Wi-Fi 网段，打开应用的「设备」页。接收方开启「允许局域网设备发现和检测本机」，点击「显示本机配对码」。
2. 在另一台电脑的「附近设备」列表选中接收方，输入它屏幕上的 **8 位配对码**，点击「配对」。接收方和发起方各自保存对方经过验证的设备公钥。两台电脑可以交换角色。
3. 重开应用后仍可在「本机信任的设备」中查看记录。选中某台设备并点击「撤销本机信任」即在本机删除该记录，离线也可操作。重新建立信任需要对方生成新码。

应用先打开窗口，再在后台读取本机设备身份。macOS 开发包更新后，钥匙串可能请求确认读取 WWMM Remote 保存的身份。等待期间仍可使用「连接检测」，读取失败后可以在「设备」页点击「重新读取」。

macOS 15 及更新系统首次使用时会询问是否允许本地网络访问。允许后即可发布和发现设备，也可以在「系统设置 → 隐私与安全性 → 本地网络」中更改选择。[Apple 本地网络权限说明](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy)

配对码有效 **120 秒**，每个码最多受理 **5 次连接尝试**，首次成功即失效。可以随时取消。关闭接收或本机监听接口发生变化，也会使当前配对码失效。关闭接收的一端仍可主动发现对方、发起配对。

macOS 用 Bonjour，Windows 用系统 DNS-SD API 在允许的接口上发布和浏览 `_wwmm-remote._tcp.local.`。每个公告携带该接口的数字地址，发起方先核对接口和直连网段，再连接服务记录给出的端口。设备名称和地址用于发现，信任记录以公钥区分设备。同名电脑可通过列表中的短指纹区分。

配对使用固定版本 BoringSSL 的 TLS 1.3 SPAKE2+ 接口。双方在加密通道内用 Ed25519 签署本次 TLS 会话和双方公钥，验证签名后写入本机信任库。macOS 把身份和信任记录保存在钥匙串，Windows 使用当前用户的 DPAPI 加密并原子替换本地文件。配对码仅在内存中使用。BoringSSL 将 TLS 中的 PAKE 绑定标为实验接口，本应用固定了依赖版本和配对协议版本。[BoringSSL 配对接口](https://boringssl.googlesource.com/boringssl/+/3dc4b63808921e914a3fb1fa61869abc3938781b/include/openssl/ssl.h) · [SPAKE2+](https://www.rfc-editor.org/rfc/rfc9383.html)

每台电脑独立管理自己的信任记录。如果最后一次应答中断或一端保存失败，两边列表可能暂时不同。用新码重配会按公钥更新原记录。0.3.0 使用这些公钥认证桌面会话。

## 第一批 · 局域网直连检测

「连接检测」页保留手动诊断。两台电脑均可主动检测对方，也可接收对方的检测。接收开关与「设备」页同步，应用会记住选择。

1. 在同一有线或 Wi-Fi 网段的两台电脑上打开 WWMM Remote。
2. 在接收方打开接收开关，从下拉框选择和对方相连的接口，点击「复制地址」。
3. 把完整地址粘贴到发起方，点击「检测连接」或按回车。成功后会显示对方自报的系统和本次建连加应答的耗时。交换两台电脑即可反向检测。

支持数字 IPv4 和 IPv6，例如 `192.168.1.20:52183`、`[fd00::20]:52183`。默认端口为 TCP 52183，端口被占用时应用会显示实际分配的端口。IPv6 链路本地地址跨多个本机接口产生歧义时，可在发起方填写本机接口名或编号，例如 `[fe80::20%4]:52183`。

检测仅对本机已连接的以太网和 Wi-Fi 接口开放。目标须匹配直连前缀，套接字绑定选定接口，发送的 IP 跳数限制为 1。应用跳过系统代理，按用户输入的数字地址直连。系统询问本地网络权限时，可允许该应用在当前局域网通信。

检测耗时包含 TCP 建连和一轮应答。桌面视频使用独立的 UDP 媒体通道，操作延迟需要从输入到屏幕回显另行测量。

## 构建与测试

需要 CMake 3.24 或更新版本、Ninja、C/C++20 工具链和 Qt 6.8 或更新版本的桌面 SDK。使用的 Qt 模块是 Core、Gui、Widgets、Network、Concurrent，测试另用 Qt Test。开发包使用 Qt 6.12.1。首次配置会下载并校验 BoringSSL 0.20261105.0、WebRTC 提交 `3710345e71ad`、Abseil 20260817.0 和 libSRTP 2.8.1。开发包运行时可断开外网。

离线构建可分别用 `-DFETCHCONTENT_SOURCE_DIR_BORINGSSL`、`-DFETCHCONTENT_SOURCE_DIR_WEBRTC_SRC`、`-DFETCHCONTENT_SOURCE_DIR_MEDIA_ABSL` 和 `-DFETCHCONTENT_SOURCE_DIR_MEDIA_SRTP` 指定这四份源码的解压目录，并传 `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`。

**macOS** 在包含 Qt 的 Mac 上运行以下命令。本工作区已将 SDK 安装到 `.deps/qt-6.12.1-macos`。默认 macOS 部署目标为 14.4。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/.deps/qt-6.12.1-macos"
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/build/dist"
open "build/dist/WWMM Remote.app"
```

**Windows x64** 在与 Qt 套件匹配的编译器环境中打开 PowerShell，将下面的示例路径换成本机 Qt 路径。MSVC 套件使用 x64 Native Tools 环境，MinGW 套件使用匹配的 MinGW 工具链。

```powershell
$qtKit = "C:\Qt\6.12.1\msvc2022_64"
$env:Path = "$qtKit\bin;$env:Path"
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$qtKit"
cmake --build build-windows
ctest --test-dir build-windows --output-on-failure
cmake --install build-windows --prefix "$PWD/build-windows/dist"
& ".\build-windows\dist\bin\WWMM Remote.exe"
```

测试通过系统回环接口建立真实 TCP 连接，覆盖协议字节、IPv4/IPv6、网段与接口选择、双向收发、分段到达、超时、取消、网络接口变化以及两个窗口的复制和检测。平台组合测试使用可注入的系统标识。四向真机互操作另行验收。

配对测试覆盖两端窗口、正确和错误配对码、过期和重用、设备密钥不符、并发争用同一码、存储失败及重开后的撤销。同机系统集成测试可以另外启用。在已连接的网卡上设置 `WWMM_TEST_NATIVE_DNSSD=1` 会启动独立测试进程，通过系统发现并配对，再验证设备下线。设置 `WWMM_TEST_SYSTEM_VAULT=1` 会使用独立测试记录验证本机密钥存储。这两个开关只影响测试程序。

**0.2.0 实测**　在 M3 Pro 上启用两项系统集成测试，两套 Qt Test 每轮共报告 51 项通过（含各套初始化与清理），连续三轮全部通过。两个独立测试进程通过 Bonjour 和本机的实际局域网接口完成发现、配对和下线。钥匙串重开后保留了设备身份和撤销结果。Mac 开发包在等待钥匙串确认时，主窗口已显示且事件循环正常。该包的本地网络授权及对外广播尚待确认，完整发现状态下的待机内存也待测。Windows x64 应用及测试程序已交叉编译，开发包已核对随包 DLL 和导入符号，Windows 真机及跨系统互测待验。

**0.1.0 基线**　2026-11-26 在 M3 Pro 上验证了打包后的 Cocoa 窗口、随包 Qt 运行库，以及同机两个独立进程在各自局域网监听端口的应答。仅保留一个开启接收的窗口后，每秒采集一次进程物理内存，20 个样本的中位数为 33.41 MiB，P95 为 33.50 MiB。

### 复现桌面链路测试

设置 `WWMM_TEST_DESKTOP_LAN=1` 会在本机有线或 Wi-Fi 接口上启动两个独立测试进程，用临时设备身份验证双向认证、授权和加密视频。桌面测试还覆盖拒绝、撤销、换向、错误密钥、重复包、乱序、丢包重传与关键帧恢复。测试画面使用合成素材。

```sh
WWMM_TEST_DESKTOP_LAN=1 WWMM_TEST_NATIVE_DNSSD=1 WWMM_TEST_SYSTEM_VAULT=1 ctest --test-dir build --output-on-failure --repeat until-fail:3
./build/wwmm_video_bench --lan
./build/wwmm_video_bench --present --lan
./build/wwmm_video_bench --split 600 --lan
```

后三条在 Mac 上依次测量硬解后的画质、Metal 呈现回调延迟和两个独立进程十分钟的物理内存。基准程序自带 14、16 像素中英文文档和测试身份，运行时会打开合成画面的测试窗口。Windows 可以运行相同的协议和会话测试。

## 真机验收

Mac → Windows、Windows → Mac、Mac → Mac、Windows → Windows 四个方向分别记录中文上屏、快捷键、拖选、断线释放和输入到画面的延迟。0.3.0 的合成视频数据可作为视频链路基线，0.4.0 的物理按键到画面延迟仍待双机测量。[技术选型与验收标准](docs/technical-selection.md)
