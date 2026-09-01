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

1. 将发布目录内的 `VaderHapticsLauncher.ini.example` 复制为
   `VaderHapticsLauncher.ini`。
2. 填写：

```ini
[paths]
space_station_dir=C:\Program Files\Flydigi Space Station
```

路径必须是其中包含 `Flydigi Space Station.exe` 与 `SpaceStationService.exe`
的目录；这只是示例路径，请填你机器的真实位置。
3. 右键以管理员身份运行 `VaderHaptics.exe`。

## 日常使用

- 需要原生 DS5 震动时，点击 **Start PS5 Enhanced**。启动器会关闭空间站、
  创建虚拟 DS5 并开始桥接。
- 要修改飞智扩展键/震动设置、断开设备或排查问题时，先点击 **Return Native**。
- 增强过程中 Steam 可重新打开。若要让空间站恢复可编辑状态，Steam 也需要退出。

## 恢复步骤

如果游戏无输入、出现双输入或震动异常：

1. 在启动器点 **Return Native**，等待状态显示 Native 已恢复。
2. 若启动器未响应，关闭它后以管理员方式运行发布目录中的
   `vader_ds5_bridge.exe mode native`，然后启动飞智空间站。
3. 查看 `logs\native.log`；确认其末尾包含 `NATIVE_RESTORE result=PASS`。
4. 若仍失败，拔掉/重新连接控制器，再保持 Native 状态收集问题报告。

不要在受反作弊保护的游戏中尝试此软件；此项目不提供任何绕过方法。
