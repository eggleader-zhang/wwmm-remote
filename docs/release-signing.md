# 配置平台发布签名

WWMM Remote 的托盘更新会离线验证 GitHub Actions 为本仓库 main 分支发布工作流签发的 Sigstore 证明。首次双击安装包时，系统另外检查平台发布者身份。每次发布的说明都列出该版本的实际签名状态。

## macOS

1. 加入 Apple Developer Program，在 Apple Developer 账户「Certificates, Identifiers & Profiles」为发布团队申请 **Developer ID Application** 证书。在拥有对应私钥的 Mac 上，从「钥匙串访问」导出带密码的 P12。
2. 在 App Store Connect「用户和访问 → 集成」建立用于公证的团队 API 密钥，记录 Key ID、Issuer ID 并安全保存下载的 P8。
3. 在仓库 **Settings → Secrets and variables → Actions → New repository secret** 配置下表。P12 和 P8 的值取原文件的 Base64 编码。macOS 可用 `base64 -i 文件 | pbcopy` 直接复制到剪贴板后粘贴到 GitHub 的密钥输入框。密钥只保存在发布者控制的密钥库和 GitHub Secrets 中。

| Secret | 内容 |
| --- | --- |
| `MACOS_CERTIFICATE_P12` | Base64 编码的证书和私钥 |
| `MACOS_CERTIFICATE_PASSWORD` | 导出 P12 时使用的密码 |
| `APPLE_NOTARY_KEY_P8` | Base64 编码的 P8 原文件 |
| `APPLE_NOTARY_KEY_ID` | Key ID |
| `APPLE_NOTARY_ISSUER_ID` | 团队 Issuer ID |

下一次发布时，CI 对嵌套二进制、通用应用和 DMG 签名，提交公证并装订票据。可以使用 `xcrun stapler validate WWMM-Remote.dmg` 核对下载包。[Apple Developer ID](https://developer.apple.com/developer-id/) · [Apple 公证流程](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)

## Windows

1. 向受信任的代码签名提供商申请 Authenticode 发布身份。先确认提供商的密钥交付方式。许多公开受信任的证书要求硬件或云端保管私钥。
2. 对于提供商允许用于 CI 的可导出证书，添加 `WINDOWS_CERTIFICATE_PFX`（包含私钥的 PFX 的 Base64 编码）和 `WINDOWS_CERTIFICATE_PASSWORD`。流水线会签署两种架构的应用、更新组件、内部安装器和最终 EXE，并添加可信时间戳。
3. 如果选择 Microsoft Artifact Signing 或硬件托管的证书，私钥留在该服务中。需要把仓库的 Windows 签名步骤接到提供商的官方签名工具。Microsoft 云签名通常通过 Entra 工作负载身份联合授权给特定仓库工作流，再配置签名账户、证书配置文件和服务地址。

正式签名后可用 PowerShell 的 `Get-AuthenticodeSignature .\WWMM-Remote-Setup.exe` 核对发布者和 Valid 状态。SmartScreen 会单独评估新发布者与文件的信誉。[Microsoft 代码签名](https://learn.microsoft.com/windows/apps/package-and-deploy/code-signing-options) · [SmartScreen 信誉](https://learn.microsoft.com/windows/apps/package-and-deploy/smartscreen-reputation)
