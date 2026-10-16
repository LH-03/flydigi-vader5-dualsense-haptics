# 安装与恢复

## 前置条件

1. Windows 10/11 x64、飞智黑武士 5 Pro，使用 USB 或飞智 2.4G 接收器。
2. 正常安装飞智空间站，并在设置中开启“允许第三方应用接管映射”。
3. 从官方签名发布页安装 HidHide。不要安装未知来源的驱动包。
4. 安装发行说明写明兼容的 VIIPER；当前基线是 v0.0.9 兼容运行时。不要在
   增强模式运行时更新 VIIPER。

## 从源码构建发布包

在“x64 Native Tools Command Prompt for VS 2022”或正常 PowerShell 中运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\Build-Release.ps1
```

生成目录为 `artifacts\VaderHaptics`。也可以用：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\Install-VaderHaptics.ps1 -PackageDirectory .\artifacts\VaderHaptics
```

这只复制 VaderHaptics 文件到当前用户目录；它不会下载驱动、修改飞智配置、也不
会替你接受 UAC。

## 第一次配置

1. 双击 `VaderHaptics.exe`，在 Windows 的管理员提示中确认启动。
2. 如果空间站正在运行，启动器会尝试自动识别并保存安装位置。
3. 未识别时，点击 **选择空间站目录…**，选择包含
   `Flydigi Space Station.exe` 与 `SpaceStationService.exe` 的目录。
   路径会自动保存，支持中文和空格，下次无需再选；无需手动编辑 INI。

启动增强前会先检查桥接程序、震动配置、VIIPER 与 HidHide 文件是否存在；
缺少时会在窗口内提示，尚不会退出 Steam 或关闭空间站。文件检查通过不代表
驱动或设备已就绪，实际接管仍由桥接程序检查。

## 日常使用

- 需要原生 DS5 震动时，点击 **启动 PS5 增强**。启动器会关闭空间站、
  创建虚拟 DS5 并开始桥接。
- 要修改飞智扩展键/震动设置，直接点击 **打开飞智设置**，它会先恢复 Native。
- 断开设备或排查问题时，点击 **返回 Native**。切换过程中按钮会禁用，
  增强运行时不会重复启动；返回 Native 后可更改空间站目录。
- 增强过程中 Steam 可重新打开。若要让空间站恢复可编辑状态，Steam 也需要退出。

## 恢复步骤

失败原因和处理建议直接显示在主窗口。点击 **查看诊断** 可在应用内查看
虚拟手柄服务错误与恢复结果，点击 **复制诊断信息** 即可反馈，无需寻找日志文件。
复制内容仅包含错误与恢复摘要，不包含服务首次启动时的 INFO 密钥输出。

本程序启动的 VIIPER 使用本机回环端口 43241/43242，以避开部分 Windows
环境保留的默认端口。不会修改系统端口保留范围或防火墙。更新程序前先返回
Native 并退出启动器，不要在增强会话中混用新旧程序。

如果窗口提示“手柄未允许第三方接管”：点击 **打开飞智设置**，在空间站的
设置中开启 **允许第三方应用接管映射**，再回到启动器点击 **启动 PS5 增强**。
设置被锁定时先正常退出 Steam。窗口会分别显示接管失败原因和 Native 恢复结果；
“已恢复 Native”只表示清理成功，不表示增强曾经接管成功。

如果游戏无输入、出现双输入或震动异常：

1. 在启动器点 **返回 Native**，等待状态显示 Native 已恢复。
2. 若启动器未响应，关闭它后以管理员方式运行发布目录中的
   `vader_ds5_bridge.exe mode native`，然后启动飞智空间站。
3. 查看 `logs\native.log`；确认其末尾包含 `NATIVE_RESTORE result=PASS`。
4. 若仍失败，拔掉/重新连接控制器，再保持 Native 状态收集问题报告。

不要在受反作弊保护的游戏中尝试此软件；此项目不提供任何绕过方法。
