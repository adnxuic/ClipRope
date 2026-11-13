<p align="center">
  <img src="resources/icons/cliprope.png" width="88" height="88" alt="ClipRope 图标">
</p>
<h1 align="center">ClipRope</h1>
<p align="center">把 Windows 剪贴板历史挂在绳上，需要时，摘一张放在桌面。</p>
<p align="center"><a href="README.md">English</a> · <strong>简体中文</strong></p>
<p align="center"><a href="https://github.com/adnxuic/ClipRope/releases/tag/v0.1.0">下载 Windows x64 安装包</a> · <a href="https://adnxuic.github.io/ClipRope/website/?lang=zh-CN">试用交互演示</a> · <a href="https://adnxuic.github.io/ClipRope/user/ClipRope-Docs.html">阅读用户手册</a> · <a href="https://github.com/adnxuic/ClipRope/issues">反馈问题</a></p>

<p align="center">
  <img src="resources/cliprope-demo.gif" width="960" alt="切到 Field notes 绳后，复制的句子挂成一张卡片，双击即可将原句粘贴到编辑器的空行。灯塔全图和框选的窄幅截图随后挂上绳子。截图被拖到桌面成为悬浮卡片，点击回绳后切回 main 绳。">
</p>

复制一段文字、一张截图或一组文件，当前绳子上就会多一张**绳上卡片**。你可以用一根绳子收集参考资料，用另一根整理写作素材，也可以把卡片拖到正在编辑的文档旁，作为**悬浮卡片**随时查看。绳子和卡片都保存在本机，重启 ClipRope 后，悬浮卡片也会回到原来的位置。

## 安装与校验

从 v0.1.0 发布页的 **Assets** 下载 `ClipRope-0.1.0-windows-x64.exe` 和同名的 `.exe.sha256` 文件。在下载目录打开 PowerShell，运行下列命令。将输出的 **Hash** 与校验文件开头的 64 位十六进制值逐位比对。两者一致后，运行安装程序，从开始菜单打开 **ClipRope**。

```powershell
Get-FileHash -Algorithm SHA256 .\ClipRope-0.1.0-windows-x64.exe
```

## 快速上手

1. 在文本编辑器里先后复制两行不同的文字，然后把光标放到想粘贴的位置。
2. 按 **Ctrl+Shift+V** 或左键单击托盘图标。找到显示第一行文字的卡片，用左键双击卡片的纸面。它会回到绳首，工作区随即收起。如果原来的编辑器仍可接收粘贴，ClipRope 会向它发送 **Ctrl+V**。你也可以在目标位置手动粘贴刚恢复的文字。
3. 再次打开工作区。在卡片中央按住鼠标左键，向下拖出整行卡片的范围，在桌面上松开。卡片就会悬浮在编辑器旁。单击左上角的**回绳**，它就会挂到**当前绳子**的首位。

如果习惯手动粘贴，在**全局设置 → 卡片**中将**绳上卡片双击操作**设为**仅复制**即可。

## 常用操作

| 想做什么 | 如何操作 |
| --- | --- |
| 调整卡片顺序 | 按住卡片中央，拖到同一根绳上两张卡片之间。 |
| 把卡片移到另一根绳上 | 用 **Ctrl+Tab** 打开多绳总览，将卡片拖到目标绳的空位。 |
| 编辑一段文字 | 双击悬浮文字卡片，修改后按 **Ctrl+S** 保存。绳上文字卡片可从右键菜单选择 **编辑文字…**。 |

跨应用拖放、回收绳和滚轮操作详见[中文用户手册](https://adnxuic.github.io/ClipRope/user/ClipRope-Docs.html)。

## 快捷键

| 默认快捷键 | 功能 | 生效范围 |
| --- | --- | --- |
| **Ctrl+Shift+V** | 显示或隐藏工作区 | 全局 |
| **Ctrl+Shift+Space** | 新建悬浮快速笔记 | 全局 |
| **Tab** | 切换到下一根收集绳 | 鼠标位于绳子上，或工作区获得焦点时 |
| **Ctrl+Tab** | 切换单绳视图与总览 | 鼠标位于绳子上，或工作区获得焦点时 |
| **Ctrl+F** | 查找卡片 | 鼠标位于绳子上，或工作区获得焦点时 |

默认情况下，切换到非空绳子时，首张卡片的内容也会复制到剪贴板。你可以在**全局设置 → 绳子**中调整这一行为。

打开**查找卡片**后，可以输入文字、链接或部分文件路径。按 **Enter** 跳到下一处匹配，按 **Shift+Enter** 回到上一处。单绳视图查找当前绳子，总览可以跨绳查找。

## 历史记录与隐私

工作区收起时，当前绳子仍会收集新复制的内容。历史记录默认保存在 `%LOCALAPPDATA%\ClipRope`。在**全局设置 → 存储与隐私**中，可以排除指定应用、查看磁盘占用或迁移历史目录。

ClipRope 保存能够还原的剪贴板格式，由接收应用决定使用哪一种。例如，Excel 单元格区域可以保留数值和格式，图表则可能以图片形式粘贴。

## 从源码构建

在 Windows x64 上安装 Git、PowerShell 7、Python 3.11 或更高版本，以及 Visual Studio 2022 的“使用 C++ 的桌面开发”工作负载。Release 构建要求 MSVC 19.40.33811（v143）、CMake 3.28.3-msvc11 和 Windows SDK 10.0.22621.0。在 Git 克隆的仓库中运行 `git switch --detach v0.1.0`，即可切换到此版本的源码。

在仓库根目录执行以下命令，编译并启动使用独立历史目录的开发版。如果日常使用的 ClipRope 正在运行，请先从托盘菜单选择**退出**。

```powershell
pwsh -File tools/build.ps1
pwsh -File tools/build.ps1 -Action RunDev
```

构建产物是 `build/app/ClipRope.exe`。`RunDev` 使用 `build/test-data/manual` 存放开发版历史。要制作安装包，先退出开发版，再运行核心回归测试并打包通过验证的 Release 构建。

```powershell
pwsh -File tools/build.ps1 -Action Test
pwsh -File tools/build.ps1 -Action Package
```

测试与打包会使用 NSIS 3.13，构建脚本会在使用前校验压缩包。离线构建时，请先将 `nsis-3.13.zip` 放入 `.local-artifacts/`。生成的安装程序为 `build/packages/ClipRope-0.1.0-windows-x64.exe`。

## 问题反馈

请附上 Windows 版本、ClipRope 版本，以及用示例文字或文件复现问题的步骤，并说明预期和实际结果。[提交问题](https://github.com/adnxuic/ClipRope/issues)

## 致谢

感谢 **Tendedero** 带来的主界面视觉灵感，尤其是把复制的内容挂在绳上的呈现方式。[Tendedero](https://github.com/alejandrobujan/tendedero)

感谢 **CopyQ** 对 ClipRope 核心功能的启发，包括保存、查找和复用剪贴板历史。[CopyQ](https://github.com/hluk/CopyQ)

## 许可证

ClipRope 的原创代码及随附文档采用 MIT 许可证。随包分发的第三方组件遵循各自的授权条款。[MIT 许可证](LICENSE) · [第三方声明](packaging/windows/ThirdPartyNotices.txt)
