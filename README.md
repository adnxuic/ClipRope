<p align="center">
  <img src="resources/icons/cliprope.png" width="88" height="88" alt="ClipRope icon">
</p>
<h1 align="center">ClipRope</h1>
<p align="center">A clipboard history manager for Windows. Hang clips on ropes or pull them onto your desktop.</p>
<p align="center"><strong>English</strong> · <a href="README.zh-CN.md">简体中文</a></p>
<p align="center"><a href="https://github.com/adnxuic/ClipRope/releases/tag/v0.1.0">Download for Windows x64</a> · <a href="https://adnxuic.github.io/ClipRope/website/?lang=en">Try the demo</a> · <a href="https://adnxuic.github.io/ClipRope/user/en/ClipRope-Docs.html">Read the user guide</a> · <a href="https://github.com/adnxuic/ClipRope/issues">Report an issue</a></p>

<p align="center">
  <img src="resources/cliprope-demo.gif" width="960" alt="On the Field notes rope, a copied sentence becomes a card. Double-clicking it pastes the sentence into the editor. A lighthouse image and a narrow snip hang beside it. The snip is dragged onto the desktop, then returned to the rope before switching back to main.">
</p>

Copy a paragraph, a screenshot, or a group of files. Each copy becomes a **Hanging card** on the active rope. Keep one rope for research and another for writing, or pull a card off the rope to keep it beside the document you’re working on. Your ropes and **Floating cards** are saved locally and return when you restart ClipRope.

## Install on Windows x64

Under **Assets** on the v0.1.0 release page, download `ClipRope-0.1.0-windows-x64.exe` and its companion `.exe.sha256` file. Run the command below in PowerShell from your download folder. Compare the **Hash** with the 64 hexadecimal digits at the start of the checksum file. If they match, run the installer and open **ClipRope** from the Start menu.

```powershell
Get-FileHash -Algorithm SHA256 .\ClipRope-0.1.0-windows-x64.exe
```

## Quick start

1. In a text editor, copy two different lines, one at a time. Leave the caret where you want to paste.
2. Press **Ctrl+Shift+V** or left-click the tray icon. Find the card showing the first line you copied and double-click its paper with the left button. The card moves to the front of its rope, the workspace hides, and ClipRope sends **Ctrl+V** if the original editor is still a valid destination. You can also paste the restored text manually.
3. Open the rope again. Hold the left button on the center of a card, drag down past the row of cards, and release on the desktop. The card now floats beside your editor. Click **Return** at its upper left to hang it at the front of the currently active rope.

Prefer to paste by hand? In **Global settings → Cards**, set **Hanging double-click** to **Copy only**.

## Everyday gestures

| To | Gesture |
| --- | --- |
| Reorder a card | Drag its center into a gap between cards on the same rope. |
| Move a card to another rope | Press **Ctrl+Tab** to open the overview, then drag the card into a gap on the destination rope. |
| Edit saved text | Double-click a Floating text card and press **Ctrl+S** to save. For a Hanging text card, right-click and choose **Edit text…**. |

For cross-app drag and drop, ReBin, and scrolling, see the [user guide](https://adnxuic.github.io/ClipRope/user/en/ClipRope-Docs.html).

## Shortcuts

| Default shortcut | Action | Where it works |
| --- | --- | --- |
| **Ctrl+Shift+V** | Show or hide the workspace | System-wide |
| **Ctrl+Shift+Space** | Start a Floating quick note | System-wide |
| **Tab** | Switch to the next collecting rope | Over a rope or with the workspace focused |
| **Ctrl+Tab** | Toggle the rope overview | Over a rope or with the workspace focused |
| **Ctrl+F** | Find in Cards | Over a rope or with the workspace focused |

By default, switching to a nonempty rope also puts its first card on the clipboard. You can change this in **Global settings → Ropes**.

With **Find in Cards** open, type text, a link, or part of a file path. **Enter** selects the next match and **Shift+Enter** the previous one. Single-rope view searches the current rope. Overview searches across ropes.

## History and privacy

The active rope collects copies even while hidden. History lives in `%LOCALAPPDATA%\ClipRope` by default. Open **Global settings → Storage & Privacy** to exclude an application from capture, check disk usage, or move the history folder.

ClipRope keeps the clipboard formats it can restore. The receiving app chooses what to paste. For example, an Excel range can retain its values and formatting, while a chart may paste as a picture.

## Build from source

On Windows x64, install Git, PowerShell 7, Python 3.11 or newer, and Visual Studio 2022 with **Desktop development with C++**. The Release build requires MSVC 19.40.33811 (v143), CMake 3.28.3-msvc11, and Windows SDK 10.0.22621.0. In a Git clone of the repository, run `git switch --detach v0.1.0` to build this release.

From the repository root, build and run a development copy with its own history. If the installed copy is running, choose **Exit** from its tray menu first.

```powershell
pwsh -File tools/build.ps1
pwsh -File tools/build.ps1 -Action RunDev
```

The build writes `build/app/ClipRope.exe`. `RunDev` opens it with a separate history in `build/test-data/manual`. To make an installer, exit the development copy, run the core regression suite, and package the verified Release build.

```powershell
pwsh -File tools/build.ps1 -Action Test
pwsh -File tools/build.ps1 -Action Package
```

The test and packaging steps use NSIS 3.13. The build verifies its archive before use. For offline builds, place `nsis-3.13.zip` in `.local-artifacts/` first. The installer is written to `build/packages/ClipRope-0.1.0-windows-x64.exe`.

## Report a problem

Include your Windows version, ClipRope version, and steps to reproduce the problem using sample text or files. Describe what you expected and what happened. [Open an issue](https://github.com/adnxuic/ClipRope/issues)

## Acknowledgments

Thank you to **Tendedero** for inspiring the look of ClipRope’s main workspace, especially the idea of hanging copied items on a line. [Tendedero](https://github.com/alejandrobujan/tendedero)

Thank you to **CopyQ** for inspiring the way ClipRope saves, searches, and reuses clipboard history. [CopyQ](https://github.com/hluk/CopyQ)

## License

ClipRope’s original code and accompanying documentation are available under the MIT License. Bundled third-party components retain their respective terms. [MIT License](LICENSE) · [Third-party notices](packaging/windows/ThirdPartyNotices.txt)
