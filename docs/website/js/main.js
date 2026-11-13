// ClipRope Website — Full Main Workspace Controller
// Ported from src/platform_win/workspace.cpp & src/presentation/viewport.cpp
(function() {
"use strict";
const {t} = window.ClipRopeI18n;

const {WorkspacePainter} = window.ClipRopePaint;
const H = window.ClipRopeHistory, U = window.ClipRopeUI;
const screenshotPreviewEl = document.getElementById("screenshot-hover-preview");
const screenshotPreviewCanvas = document.getElementById("screenshot-hover-canvas");
const screenshotPreviewCtx = screenshotPreviewCanvas ? screenshotPreviewCanvas.getContext("2d") : null;
const trayBtn = document.getElementById("tray-cliprope-btn");
const canvas = document.getElementById("workspace-canvas");
if (!canvas)
  return;
const painter = new WorkspacePainter();
const BASE_SCALE = 1;
let workspace;

// ---------- Screenshot Hover Full-Size Preview (ported from src/platform_win/screenshot_hover.cpp)
// ----------

function isScreenshotPreviewEligible(card) {
  // Matches application::screenshot_preview_eligible in src/application/screenshot_preview.hpp (lines
  // 22-25)
  return Boolean(card && card.kind === "image" && card.status !== "damaged");
}

function intersectRect(a, b) {
  const left = Math.max(a.left, b.left);
  const top = Math.max(a.top, b.top);
  const right = Math.min(a.right, b.right);
  const bottom = Math.min(a.bottom, b.bottom);
  if (right <= left || bottom <= top)
    return null;
  return {left, top, right, bottom};
}

// Exact port of screenshot_placement in src/platform_win/screenshot_hover.cpp (lines 13-59)
function computeScreenshotPlacement(cardRect, workRect, sw, sh) {
  if (!sw || !sh || sw > 8192 || sh > 8192 || sw * sh > 8388608) {
    return null;
  }
  const gap = Math.round(12 * BASE_SCALE);
  const pad = Math.max(1, Math.round(3 * BASE_SCALE));
  const mw = Math.min(960 * BASE_SCALE, (workRect.right - workRect.left) * 0.8);
  const mh = Math.min(720 * BASE_SCALE, (workRect.bottom - workRect.top) * 0.8);
  const ideal = Math.min(1.0, mw / sw, mh / sh, 2048 / sw, 2048 / sh, Math.sqrt(2097152 / (sw * sh)));
  if (ideal <= 0)
    return null;

  const regions = [
    {left: cardRect.right + gap, top: workRect.top, right: workRect.right, bottom: workRect.bottom},
    {left: workRect.left, top: workRect.top, right: cardRect.left - gap, bottom: workRect.bottom},
    {left: workRect.left, top: cardRect.bottom + gap, right: workRect.right, bottom: workRect.bottom},
    {left: workRect.left, top: workRect.top, right: workRect.right, bottom: cardRect.top - gap},
  ];

  let best = null;
  let bestArea = 0;

  for (let i = 0; i < 4; ++i) {
    const r = intersectRect(regions[i], workRect);
    if (!r)
      continue;
    const ratio = Math.min(ideal, (r.right - r.left - pad * 2) / sw, (r.bottom - r.top - pad * 2) / sh);
    if (ratio <= 0)
      continue;
    const w = Math.floor(sw * ratio);
    const h = Math.floor(sh * ratio);
    if (!w || !h)
      continue;

    const x = i === 0 ? r.left :
        i === 1       ? r.right - w - pad * 2 :
                        Math.min(r.right - w - pad * 2, Math.max(r.left, cardRect.left));
    const y = i === 2 ? r.top :
        i === 3       ? r.bottom - h - pad * 2 :
                        Math.min(r.bottom - h - pad * 2, Math.max(r.top, cardRect.top));

    const candidate = {
      window: {left: x, top: y, right: x + w + pad * 2, bottom: y + h + pad * 2},
      width: w,
      height: h,
      padding: pad,
    };
    if (w * h > bestArea) {
      best = candidate;
      bestArea = w * h;
    }
    // Preserve right/left/below/above priority when the full preferred size fits.
    if (ratio >= ideal - 1e-9) {
      return candidate;
    }
  }
  return best;
}

const screenshotHover = {
  sourceKey: "",
  cardId: "",
  card: null,
  anchor: null,
  placement: null,
  liveCheck: null,
  enteredAt: 0,
  timerId: 0,
  displayed: false,

  present(card, placement) {
    if (!screenshotPreviewEl || !screenshotPreviewCanvas || !screenshotPreviewCtx)
      return false;
    const dpr = window.devicePixelRatio || 1;
    const pad = placement.padding;
    const totalW = placement.width + 2 * pad;
    const totalH = placement.height + 2 * pad;
    if (!totalW || !totalH || totalW > 2112 || totalH > 2112 || totalW * totalH > 2300000) {
      return false;
    }

    const targetW = Math.max(1, Math.round(totalW * dpr));
    const targetH = Math.max(1, Math.round(totalH * dpr));
    if (screenshotPreviewCanvas.width !== targetW || screenshotPreviewCanvas.height !== targetH) {
      screenshotPreviewCanvas.width = targetW;
      screenshotPreviewCanvas.height = targetH;
      screenshotPreviewCanvas.style.width = `${totalW}px`;
      screenshotPreviewCanvas.style.height = `${totalH}px`;
    }

    screenshotPreviewCtx.save();
    screenshotPreviewCtx.setTransform(1, 0, 0, 1, 0, 0);
    screenshotPreviewCtx.scale(dpr, dpr);

    // Matches ScreenshotHover::Impl::present in src/platform_win/screenshot_hover.cpp (lines 143-157):
    // fluent::system_palette() light: border = palette.line (#e7e3da), bg = palette.panel (#ffffff)
    screenshotPreviewCtx.fillStyle = "#e7e3da";
    screenshotPreviewCtx.fillRect(0, 0, totalW, totalH);
    screenshotPreviewCtx.fillStyle = "#ffffff";
    screenshotPreviewCtx.fillRect(pad, pad, placement.width, placement.height);

    const bmp = painter.ensurePreviewBitmap(card, placement.width, placement.height, dpr);
    if (bmp) {
      screenshotPreviewCtx.imageSmoothingEnabled = true;
      screenshotPreviewCtx.imageSmoothingQuality = "high";
      screenshotPreviewCtx.drawImage(bmp, pad, pad, placement.width, placement.height);
    }

    screenshotPreviewCtx.restore();

    screenshotPreviewEl.style.transform =
        `translate3d(${Math.round(placement.window.left)}px, ${Math.round(placement.window.top)}px, 0)`;
    screenshotPreviewEl.classList.add("is-visible");
    this.displayed = true;
    return true;
  },

  offer(sourceKey, card, anchor, liveCheck, immediate = false) {
    if (!isScreenshotPreviewEligible(card)) {
      this.leave(sourceKey);
      return false;
    }
    const same = this.sourceKey === sourceKey && this.cardId === card.id;
    if (same && this.anchor && this.anchor.left === anchor.left && this.anchor.top === anchor.top &&
        this.anchor.right === anchor.right && this.anchor.bottom === anchor.bottom) {
      return true;
    }

    const now = performance.now();
    const entered = same ? this.enteredAt : now;
    const workRect = {
      left: 0,
      top: 0,
      right: window.innerWidth || 1536,
      bottom: Math.max(100, (window.innerHeight || 864) - 48),
    };
    const sw = (card.source && card.source.width) || 1920;
    const sh = (card.source && card.source.height) || 1080;
    const layout = computeScreenshotPlacement(anchor, workRect, sw, sh);

    if (layout && same && this.placement && this.placement.width === layout.width &&
        this.placement.height === layout.height) {
      this.anchor = anchor;
      this.placement = layout;
      this.liveCheck = liveCheck;
      if (this.displayed && screenshotPreviewEl) {
        screenshotPreviewEl.style.transform =
            `translate3d(${Math.round(layout.window.left)}px, ${Math.round(layout.window.top)}px, 0)`;
      }
      return true;
    }

    this.cancel();
    if (!layout)
      return true;

    this.sourceKey = sourceKey;
    this.cardId = card.id;
    this.card = card;
    this.anchor = anchor;
    this.placement = layout;
    this.liveCheck = liveCheck;
    this.enteredAt = entered;

    if (immediate) {
      this.present(card, layout);
      return true;
    }

    // Matches SetTimer(p.window, 1, 10, nullptr) & 500ms dwell threshold in screenshot_hover.cpp (line 177)
    const delayMs = Math.max(0, 500 - (now - entered));
    this.timerId = window.setTimeout(() => {
      this.timerId = 0;
      if (!this.liveCheck || !this.liveCheck()) {
        this.cancel();
        return;
      }
      this.present(this.card, this.placement);
    }, delayMs);
    return true;
  },

  leave(sourceKey) {
    if (this.sourceKey === sourceKey) {
      this.cancel();
    }
  },

  cancel() {
    if (this.timerId) {
      clearTimeout(this.timerId);
      this.timerId = 0;
    }
    this.displayed = false;
    this.sourceKey = "";
    this.cardId = "";
    this.card = null;
    this.anchor = null;
    this.placement = null;
    this.liveCheck = null;
    if (screenshotPreviewEl) {
      screenshotPreviewEl.classList.remove("is-visible");
    }
  },
};

const samples = {"zh-CN": window.createClipRopeSamples("zh-CN"), en: window.createClipRopeSamples("en")};
const {ropes, folders} = samples["zh-CN"];
const wechatSessions = structuredClone(samples["zh-CN"].sessions);
const sampleView = window.createClipRopeSampleView(samples);

function updateTaskbarClock() {
  const timeEl = document.getElementById("clock-time");
  const dateEl = document.getElementById("clock-date");
  if (!timeEl || !dateEl)
    return;
  const now = new Date();
  const hh = String(now.getHours()).padStart(2, "0");
  const mm = String(now.getMinutes()).padStart(2, "0");
  timeEl.textContent = `${hh}:${mm}`;
  dateEl.textContent = now.toLocaleDateString(window.ClipRopeI18n.language);
}
updateTaskbarClock();
setInterval(updateTaskbarClock, 15000);

// ---------- Desktop Folders & Windows 11 File Explorer (文件资源管理器) ----------

const explorerBtn = document.getElementById("taskbar-explorer-btn");
const explorerWin = document.getElementById("win11-explorer");
const explorerTitlebar = document.getElementById("explorer-titlebar");
const explorerTabsEl = document.getElementById("explorer-tabs");
const explorerSidebarListEl = document.getElementById("explorer-sidebar-list");
const explorerCurrentCrumbEl = document.getElementById("explorer-current-folder-crumb");
const explorerFilesListEl = document.getElementById("explorer-files-list");
const explorerItemCountEl = document.getElementById("explorer-item-count");
const explorerFocusStatusEl = document.getElementById("explorer-focus-status");
const explorerMinBtn = document.getElementById("explorer-min-btn");
const explorerMaxBtn = document.getElementById("explorer-max-btn");
const explorerCloseBtn = document.getElementById("explorer-close-btn");
const desktopFolders = Array.from(document.querySelectorAll(".desktop-folder[data-folder-id]"));

let explorerFolders = folders;

let explorerOpen = false;
let explorerMaximized = false;
let activeFolderId = "specs";
let selectedFiles = new Set();
let explorerPos = {
  x: Math.max(140, Math.round(((window.innerWidth || 1536) - 880) / 2)),
  y: 292,
};
let explorerDrag = null;

function getActiveExplorerFolder() {
  return explorerFolders.find((f) => f.id === activeFolderId) || explorerFolders[0];
}

function syncExplorerPosition() {
  if (!explorerWin || explorerMaximized)
    return;
  explorerPos.x = Math.max(0, Math.min(explorerPos.x, innerWidth - explorerWin.offsetWidth));
  explorerPos.y = Math.max(0, Math.min(explorerPos.y, innerHeight - 48 - explorerWin.offsetHeight));
  explorerWin.style.setProperty("--exp-x", `${explorerPos.x}px`);
  explorerWin.style.setProperty("--exp-y", `${explorerPos.y}px`);
}

function syncMaximizeIcon() {
  if (!explorerMaxBtn)
    return;
  if (explorerMaximized) {
    explorerMaxBtn.title = t("restore");
    explorerMaxBtn.setAttribute("aria-label", t("restore"));
    explorerMaxBtn.innerHTML =
        `<svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" stroke-width="1.1"><rect x="2.5" y="0.8" width="6.7" height="6.7" rx="0.8"/><path d="M0.8 2.8v6.4h6.4" stroke-linecap="round"/></svg>`;
  } else {
    explorerMaxBtn.title = t("maximize");
    explorerMaxBtn.setAttribute("aria-label", t("maximize"));
    explorerMaxBtn.innerHTML =
        `<svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" stroke-width="1.1"><rect x="1" y="1" width="8" height="8" rx="1"/></svg>`;
  }
}

function renderExplorer() {
  const folder = getActiveExplorerFolder();

  // Sync desktop folder selection highlight
  for (const btn of desktopFolders) {
    const isSelected = explorerOpen && btn.getAttribute("data-folder-id") === folder.id;
    btn.classList.toggle("is-focused", isSelected);
  }

  // Sync taskbar button state
  if (explorerBtn) {
    explorerBtn.classList.toggle("active", explorerOpen);
    explorerBtn.setAttribute("aria-pressed", explorerOpen ? "true" : "false");
  }

  if (!explorerWin)
    return;
  explorerWin.classList.toggle("is-open", explorerOpen);
  explorerWin.classList.toggle("is-maximized", explorerMaximized);
  explorerWin.setAttribute("aria-hidden", explorerOpen ? "false" : "true");
  syncExplorerPosition();
  syncMaximizeIcon();

  // Render titlebar folder tabs (allows switching folder focus directly in File Explorer)
  if (explorerTabsEl) {
    explorerTabsEl.innerHTML = "";
    for (const f of explorerFolders) {
      const tab = document.createElement("button");
      tab.type = "button";
      tab.className = `explorer-tab${f.id === folder.id ? " is-active" : ""}`;
      tab.setAttribute("data-folder-id", f.id);
      tab.setAttribute("role", "tab");
      tab.setAttribute("aria-selected", f.id === folder.id ? "true" : "false");
      const dot = U.element("span", "explorer-tab-dot");
      dot.setAttribute("aria-hidden", "true");
      tab.append(dot, U.element("span", "", sampleView.folderName(f)));
      tab.addEventListener("click", () => {
        setActiveExplorerFolder(f.id);
      });
      explorerTabsEl.appendChild(tab);
    }
  }

  // Render left sidebar folder list (also switches folder focus)
  if (explorerSidebarListEl) {
    explorerSidebarListEl.innerHTML = "";
    for (const f of explorerFolders) {
      const item = document.createElement("button");
      item.type = "button";
      item.className = `sidebar-folder-item${f.id === folder.id ? " is-active" : ""}`;
      item.setAttribute("data-folder-id", f.id);
      const left = U.element("span", "sidebar-folder-left"), dot = U.element("span", "explorer-tab-dot");
      dot.setAttribute("aria-hidden", "true");
      left.append(dot, U.element("span", "", sampleView.folderName(f)));
      item.append(left, U.element("span", "sidebar-folder-count", f.files.length));
      item.addEventListener("click", () => {
        setActiveExplorerFolder(f.id);
      });
      explorerSidebarListEl.appendChild(item);
    }
  }

  // Render address bar current folder breadcrumb
  if (explorerCurrentCrumbEl) {
    explorerCurrentCrumbEl.textContent = sampleView.folderName(folder);
  }

  if (explorerFilesListEl) {
    explorerFilesListEl.replaceChildren();
    for (const file of folder.files) {
      const display = sampleView.fileView(file, folder.id);
      const row = U.element("div", "explorer-file-row");
      row.setAttribute("role", "option");
      row.dataset.filename = file.name;
      row.setAttribute("aria-label", display.name);
      row.title = display.name;
      const name = U.element("div", "file-row-name");
      const check = U.element("input");
      check.type = "checkbox";
      check.setAttribute("aria-label", t("selectFile", display.name));
      name.append(check, U.element("span", "file-ext-pill", file.ext), U.element("span", "", display.name));
      row.append(
          name, U.element("div", "file-row-meta", display.type), U.element("div", "file-row-meta", file.size));
      row.onclick = e => {
        const selected = selectedFiles.has(file.name);
        if (e.target !== check && !e.ctrlKey)
          selectedFiles.clear();
        if (selected && (e.target === check || e.ctrlKey))
          selectedFiles.delete(file.name);
        else
          selectedFiles.add(file.name);
        syncExplorerSelection();
      };
      if (file.preview) {
        const open = () => workspace.previewCard(H.asFiles([sourceFile(folder, file)]));
        row.ondblclick = e => { if (e.target !== check) open(); };
        row.onkeydown = e => { if (e.key === "Enter") { e.preventDefault(); open(); } };
      }
      explorerFilesListEl.append(row);
    }
  }
  explorerItemCountEl.textContent = t(folder.files.length === 1 ? "oneItem" : "items", folder.files.length);
  syncExplorerSelection();
}

function syncExplorerSelection() {
  for (const row of explorerFilesListEl.children) {
    const selected = selectedFiles.has(row.dataset.filename);
    row.setAttribute("aria-selected", String(selected));
    row.classList.toggle("is-selected", selected);
    row.querySelector("input").checked = selected;
  }
  explorerFocusStatusEl.textContent = t(selectedFiles.size === 1 ? "oneSelected" : "selectedItems", selectedFiles.size);
  document.querySelector("#files-copy").disabled = !selectedFiles.size;
}

let focusedDesktopWindow = "wechat";

function focusDesktopWindow(target) {
  focusedDesktopWindow = target;
  if (explorerWin) {
    explorerWin.classList.toggle("is-front", target === "explorer");
  }
  const wcWin = document.getElementById("win11-wechat");
  if (wcWin) {
    wcWin.classList.toggle("is-front", target === "wechat");
  }
}

function setActiveExplorerFolder(folderId) {
  if (!explorerFolders.some((f) => f.id === folderId))
    return;
  activeFolderId = folderId;
  selectedFiles.clear();
  renderExplorer();
}

function openExplorerFolder(folderId) {
  if (folderId && explorerFolders.some((f) => f.id === folderId)) {
    activeFolderId = folderId;
    selectedFiles.clear();
  }
  explorerOpen = true;
  focusDesktopWindow("explorer");
  renderExplorer();
}

function closeExplorerWindow() {
  explorerOpen = false;
  explorerMaximized = false;
  if (wechatOpen) {
    focusDesktopWindow("wechat");
  }
  renderExplorer();
}

function toggleExplorerWindow() {
  if (explorerOpen && (focusedDesktopWindow === "explorer" || !wechatOpen)) {
    explorerOpen = false;
    if (wechatOpen) {
      focusDesktopWindow("wechat");
    }
    renderExplorer();
  } else {
    explorerOpen = true;
    focusDesktopWindow("explorer");
    renderExplorer();
  }
}

function toggleExplorerMaximized() {
  if (!explorerOpen) {
    explorerOpen = true;
  }
  focusDesktopWindow("explorer");
  explorerMaximized = !explorerMaximized;
  renderExplorer();
}

// ADR 0005 (0005-explorer-folder-paste.md) & external_drag.cpp:
// Only real files/shortcuts, folders/multi-selections, and saved image pixels/screenshots
// produce filesystem items when pasted or Ctrl+dragged into an Explorer folder.
// "Text paths and web URLs do not become filesystem items."
function canCopyCardToFolder(card) {
  if (!card)
    return false;
  return card.kind === "files" || card.kind === "folder" || card.kind === "image";
}

function formatLocalScreenshotName(now = new Date()) {
  // Matches swprintf_s(file.cFileName, L"Screenshot-%04u%02u%02u-%02u%02u%02u-%03u.png", ...)
  // in src/platform_win/virtual_files.cpp (line 62)
  const yyyy = String(now.getFullYear()).padStart(4, "0");
  const mm = String(now.getMonth() + 1).padStart(2, "0");
  const dd = String(now.getDate()).padStart(2, "0");
  const hh = String(now.getHours()).padStart(2, "0");
  const min = String(now.getMinutes()).padStart(2, "0");
  const ss = String(now.getSeconds()).padStart(2, "0");
  const ms = String(now.getMilliseconds()).padStart(3, "0");
  return `Screenshot-${yyyy}${mm}${dd}-${hh}${min}${ss}-${ms}.png`;
}

async function copyCardToExplorerFolder(card, folderId, stillValid = () => true) {
  if (!canCopyCardToFolder(card))
    return false;
  const entries = card.kind === "image" ? [{
    name: formatLocalScreenshotName(),
    ext: "PNG",
    type: "PNG 图像",
    dataUrl: getCardImageDataUrl(card)
  }] :
                                          card.members || [];
  return workspace.command(s => {
    if (!stillValid())
      throw new Error(t("targetChanged"));
    const folder = s.folders.find(f => f.id === folderId);
    if (!folder || !entries.length)
      throw new Error(t("fileMissing"));
    const copies = [];
    const names = new Set(folder.files.flatMap(f => [f.name, sampleView.fileView(f, folderId).name])
        .map(name => name.toLowerCase()));
    for (const file of entries) {
      const dot = file.name.lastIndexOf("."), base = dot > 0 ? file.name.slice(0, dot) : file.name,
            suffix = dot > 0 ? file.name.slice(dot) : "";
      let name = file.name, n = 2;
      while (names.has(name.toLowerCase()))
        name = base + " (" + (n++) + ")" + suffix;
      names.add(name.toLowerCase());
      copies.push({
        ...file,
        name,
        parent: folder.path,
        folderId,
        size: file.size || "—",
        modified: new Date().toLocaleString()
      });
    }
    folder.files.unshift(...copies);
  }, false);
}

// Hit-tests folder drop targets for ExternalDragSession (Ctrl + Left-Button Drag):
// 1. Inside #win11-explorer (if open): checks specific folder tabs, sidebar folder items, or active folder
// pane
// 2. Desktop folder icons (#desktop-icons .desktop-folder[data-folder-id])
function findExternalDropFolder(clientX, clientY) {
  if (explorerOpen && explorerWin) {
    const eRect = explorerWin.getBoundingClientRect();
    if (clientX >= eRect.left && clientX <= eRect.right && clientY >= eRect.top &&
        clientY <= eRect.bottom) {
      if (explorerTabsEl) {
        const tabs = explorerTabsEl.querySelectorAll(".explorer-tab[data-folder-id]");
        for (const tab of tabs) {
          const r = tab.getBoundingClientRect();
          if (clientX >= r.left && clientX <= r.right && clientY >= r.top && clientY <= r.bottom) {
            return tab.getAttribute("data-folder-id");
          }
        }
      }
      if (explorerSidebarListEl) {
        const items = explorerSidebarListEl.querySelectorAll(".sidebar-folder-item[data-folder-id]");
        for (const item of items) {
          const r = item.getBoundingClientRect();
          if (clientX >= r.left && clientX <= r.right && clientY >= r.top && clientY <= r.bottom) {
            return item.getAttribute("data-folder-id");
          }
        }
      }
      return activeFolderId;
    }
  }

  for (const btn of desktopFolders) {
    const r = btn.getBoundingClientRect();
    if (clientX >= r.left && clientX <= r.right && clientY >= r.top && clientY <= r.bottom) {
      return btn.getAttribute("data-folder-id");
    }
  }

  return null;
}

function syncExternalDropHighlights(dropFolderId) {
  for (const btn of desktopFolders) {
    const fid = btn.getAttribute("data-folder-id");
    const isFocused = (explorerOpen && fid === activeFolderId) || (dropFolderId && fid === dropFolderId);
    btn.classList.toggle("is-focused", Boolean(isFocused));
  }
}

if (explorerWin) {
  explorerWin.addEventListener("pointerdown", () => {
    focusDesktopWindow("explorer");
  });
}

// Bind desktop folder icons (click opens File Explorer and focuses that folder)
for (const btn of desktopFolders) {
  btn.addEventListener("click", () => {
    const folderId = btn.getAttribute("data-folder-id");
    openExplorerFolder(folderId);
  });
}

// Bind taskbar File Explorer button
if (explorerBtn) {
  explorerBtn.addEventListener("click", () => {
    toggleExplorerWindow();
  });
}

if (explorerMinBtn) {
  explorerMinBtn.addEventListener("click", () => {
    explorerOpen = false;
    if (wechatOpen) {
      focusDesktopWindow("wechat");
    }
    renderExplorer();
  });
}

if (explorerMaxBtn) {
  explorerMaxBtn.addEventListener("click", () => {
    toggleExplorerMaximized();
  });
}

if (explorerCloseBtn) {
  explorerCloseBtn.addEventListener("click", () => {
    closeExplorerWindow();
  });
}

// Titlebar drag & double-click maximize
if (explorerTitlebar) {
  explorerTitlebar.addEventListener("dblclick", (e) => {
    if (e.target.closest("button"))
      return;
    toggleExplorerMaximized();
  });

  explorerTitlebar.addEventListener("pointerdown", (e) => {
    if (e.button !== 0 || explorerMaximized)
      return;
    if (e.target.closest("button"))
      return;
    explorerTitlebar.setPointerCapture(e.pointerId);
    explorerDrag = {
      pointerId: e.pointerId,
      startX: e.clientX,
      startY: e.clientY,
      origX: explorerPos.x,
      origY: explorerPos.y,
    };
  });

  explorerTitlebar.addEventListener("pointermove", (e) => {
    if (!explorerDrag || e.pointerId !== explorerDrag.pointerId)
      return;
    const maxX = Math.max(40, (window.innerWidth || 1536) - 240);
    const maxY = Math.max(40, (window.innerHeight || 864) - 120);
    explorerPos.x = Math.min(maxX, Math.max(0, explorerDrag.origX + (e.clientX - explorerDrag.startX)));
    explorerPos.y = Math.min(maxY, Math.max(0, explorerDrag.origY + (e.clientY - explorerDrag.startY)));
    syncExplorerPosition();
  });

  function finishExplorerDrag(e) {
    if (!explorerDrag || e.pointerId !== explorerDrag.pointerId)
      return;
    explorerDrag = null;
    if (explorerTitlebar.hasPointerCapture(e.pointerId)) {
      explorerTitlebar.releasePointerCapture(e.pointerId);
    }
  }

  explorerTitlebar.addEventListener("pointerup", finishExplorerDrag);
  explorerTitlebar.addEventListener("pointercancel", finishExplorerDrag);
}

renderExplorer();

// ---------- WeChat Desktop Chat Window (微信桌面版对话框模拟) ----------

const wechatBtn = document.getElementById("taskbar-wechat-btn");
const wechatWin = document.getElementById("win11-wechat");
const wechatTitlebar = document.getElementById("wechat-titlebar");
const wechatSessionsListEl = document.getElementById("wechat-sessions");
const wechatChatTitleEl = document.getElementById("wechat-chat-title");
const wechatMessagesEl = document.getElementById("wechat-messages");
const wechatDraftAttachmentsEl = document.getElementById("wechat-draft-attachments");
const wechatInputEl = document.getElementById("wechat-input");
const wechatSendBtn = document.getElementById("wechat-send-btn");
const wechatMinBtn = document.getElementById("wechat-min-btn");
const wechatMaxBtn = document.getElementById("wechat-max-btn");
const wechatCloseBtn = document.getElementById("wechat-close-btn");

let wechatOpen = false;
let wechatMaximized = false;
let activeWeChatSessionId = "dev-group";
let wechatDraftAttachments = [];
let wechatAttachmentSeq = 0;
let wechatMsgSeq = 10;
let wechatPos = {
  x: Math.max(180, Math.round(((window.innerWidth || 1536) - 820) / 2)),
  y: 284,
};
let wechatDrag = null;

function getActiveWeChatSession() {
  return wechatSessions.find((s) => s.id === activeWeChatSessionId) || wechatSessions[0];
}

function canPasteCardIntoWeChat(card) {
  if (!card)
    return false;
  return card.kind !== "unavailable";
}

function isExternalDropOverWeChat(clientX, clientY) {
  if (!wechatOpen || !wechatWin)
    return false;
  // If File Explorer is open and in front of WeChat, check if Explorer covers this point first
  if (explorerOpen && explorerWin && focusedDesktopWindow === "explorer") {
    const eRect = explorerWin.getBoundingClientRect();
    if (clientX >= eRect.left && clientX <= eRect.right && clientY >= eRect.top &&
        clientY <= eRect.bottom) {
      return false;
    }
  }
  const wRect = wechatWin.getBoundingClientRect();
  return (
      clientX >= wRect.left && clientX <= wRect.right && clientY >= wRect.top && clientY <= wRect.bottom);
}

function getCardImageDataUrl(card) {
  if (card?.dataUrl)
    return card.dataUrl;
  if (!card)
    return "";
  if (card.bitmapCanvas && typeof card.bitmapCanvas.toDataURL === "function") {
    return card.bitmapCanvas.toDataURL("image/png");
  }
  const previewCanvas =
      painter.ensurePreviewBitmap(card, card.source?.width || 960, card.source?.height || 540, 1);
  if (previewCanvas && typeof previewCanvas.toDataURL === "function") {
    return previewCanvas.toDataURL("image/png");
  }
  return "";
}

function summarizeWeChatMessage(msg) {
  if (!msg)
    return "";
  if (msg.type === "image")
    return t("imageMessage");
  if (msg.type === "file")
    return t("fileMessage", msg.fileName || "");
  return msg.text || "";
}

function syncWeChatPosition() {
  if (!wechatWin || wechatMaximized)
    return;
  wechatPos.x = Math.max(0, Math.min(wechatPos.x, innerWidth - wechatWin.offsetWidth));
  wechatPos.y = Math.max(0, Math.min(wechatPos.y, innerHeight - 48 - wechatWin.offsetHeight));
  wechatWin.style.setProperty("--wc-x", `${wechatPos.x}px`);
  wechatWin.style.setProperty("--wc-y", `${wechatPos.y}px`);
}

function syncWeChatMaximizeIcon() {
  if (!wechatMaxBtn)
    return;
  if (wechatMaximized) {
    wechatMaxBtn.title = t("restore");
    wechatMaxBtn.setAttribute("aria-label", t("restore"));
    wechatMaxBtn.innerHTML =
        `<svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" stroke-width="1.1"><rect x="2.5" y="0.8" width="6.7" height="6.7" rx="0.8"/><path d="M0.8 2.8v6.4h6.4" stroke-linecap="round"/></svg>`;
  } else {
    wechatMaxBtn.title = t("maximize");
    wechatMaxBtn.setAttribute("aria-label", t("maximize"));
    wechatMaxBtn.innerHTML =
        `<svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" stroke-width="1.1"><rect x="1" y="1" width="8" height="8" rx="1"/></svg>`;
  }
}

function renderWeChatDraft() {
  if (!wechatDraftAttachmentsEl)
    return;
  wechatDraftAttachmentsEl.innerHTML = "";
  if (wechatDraftAttachments.length === 0) {
    wechatDraftAttachmentsEl.hidden = true;
    return;
  }
  wechatDraftAttachmentsEl.hidden = false;
  for (const item of wechatDraftAttachments) {
    const chip = document.createElement("div");
    chip.className = "wechat-draft-chip";
    if (item.type === "image") {
      const img = document.createElement("img");
      img.className = "wechat-draft-thumb";
      img.src = item.dataUrl;
      img.alt = item.name || t("imagePreview");
      chip.appendChild(img);
      const label = document.createElement("span");
      label.className = "wechat-draft-chip-name";
      label.textContent = item.name || t("image");
      chip.appendChild(label);
    } else {
      const extBadge = document.createElement("span");
      extBadge.className = "file-ext-pill";
      extBadge.textContent = item.ext || "FILE";
      chip.appendChild(extBadge);
      const label = document.createElement("span");
      label.className = "wechat-draft-chip-name";
      label.textContent = (item.member && sampleView.fileView(item.member).name) || item.fileName || t("file");
      chip.appendChild(label);
    }

    const removeBtn = document.createElement("button");
    removeBtn.type = "button";
    removeBtn.className = "wechat-draft-remove";
    removeBtn.title = t("removeAttachment");
    removeBtn.setAttribute("aria-label", t("removeAttachment"));
    removeBtn.textContent = "×";
    removeBtn.addEventListener("click", () => {
      wechatDraftAttachments = wechatDraftAttachments.filter((a) => a.id !== item.id);
      renderWeChatDraft();
    });
    chip.appendChild(removeBtn);
    wechatDraftAttachmentsEl.appendChild(chip);
  }
}

function renderWeChatMessages(scrollToBottom = false) {
  if (!wechatMessagesEl)
    return;
  const session = sampleView.sessionView(getActiveWeChatSession());
  wechatMessagesEl.innerHTML = "";

  for (const msg of session.messages) {
    const row = document.createElement("div");
    row.className = `wechat-msg-row ${msg.sender === "self" ? "is-self" : "is-other"}`;

    const avatar = document.createElement("div");
    avatar.className = "wechat-msg-avatar";
    avatar.style.background = msg.avatarBg || (msg.sender === "self" ? "#07c160" : "#2b6cb0");
    avatar.textContent = msg.avatarText || (msg.sender === "self" ? t("me") : t("friend"));
    row.appendChild(avatar);

    const contentCol = document.createElement("div");
    contentCol.className = "wechat-msg-col";

    if (msg.sender === "other" && msg.author && session.id === "dev-group") {
      const senderEl = document.createElement("div");
      senderEl.className = "wechat-msg-sender";
      senderEl.textContent = msg.author;
      contentCol.appendChild(senderEl);
    }

    if (msg.type === "image") {
      const img = document.createElement("img");
      img.className = "wechat-bubble-image";
      img.src = msg.dataUrl;
      img.alt = msg.alt || t("chatImage");
      contentCol.appendChild(img);
    } else if (msg.type === "file") {
      const bubble = U.button("", () => {
        const member = messageFile(msg);
        if (member) workspace.previewCard(H.asFiles([member]));
      }, "wechat-bubble-file");
      bubble.setAttribute("aria-label", t("openFile", msg.fileName || t("file")));
      const info = document.createElement("span");
      info.className = "wechat-file-info";
      const nameEl = document.createElement("span");
      nameEl.className = "wechat-file-title";
      nameEl.textContent = msg.fileName || t("file");
      const metaEl = document.createElement("span");
      metaEl.className = "wechat-file-sub";
      metaEl.textContent = msg.meta || t("fileKind", msg.ext || "FILE");
      info.appendChild(nameEl);
      info.appendChild(metaEl);
      const iconEl = document.createElement("span");
      iconEl.className = "wechat-file-badge";
      iconEl.textContent = msg.ext || "FILE";
      bubble.appendChild(info);
      bubble.appendChild(iconEl);
      contentCol.appendChild(bubble);
    } else {
      const bubble = document.createElement("div");
      bubble.className = "wechat-bubble-text";
      bubble.textContent = msg.text || "";
      contentCol.appendChild(bubble);
    }

    const copy = U.button(t("copy"), () => copyMessage(msg, contentCol), "message-copy");
    copy.onpointerdown = e => e.preventDefault();
    contentCol.append(copy);
    row.appendChild(contentCol);
    wechatMessagesEl.appendChild(row);
  }

  if (scrollToBottom) {
    wechatMessagesEl.scrollTop = wechatMessagesEl.scrollHeight;
  }
}

function renderWeChat(scrollToBottom = false) {
  const session = sampleView.sessionView(getActiveWeChatSession());

  if (wechatBtn) {
    wechatBtn.classList.toggle("active", wechatOpen);
    wechatBtn.setAttribute("aria-pressed", wechatOpen ? "true" : "false");
  }

  if (!wechatWin)
    return;
  wechatWin.classList.toggle("is-open", wechatOpen);
  wechatWin.classList.toggle("is-maximized", wechatMaximized);
  wechatWin.classList.toggle("is-front", focusedDesktopWindow === "wechat");
  wechatWin.setAttribute("aria-hidden", wechatOpen ? "false" : "true");
  syncWeChatPosition();
  syncWeChatMaximizeIcon();

  if (wechatChatTitleEl) {
    wechatChatTitleEl.textContent = session.title;
  }

  if (wechatSessionsListEl) {
    wechatSessionsListEl.innerHTML = "";
    for (const original of wechatSessions) {
      const s = sampleView.sessionView(original);
      const lastMsg = s.messages[s.messages.length - 1];
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = `wechat-session-item${s.id === session.id ? " is-active" : ""}`;
      btn.setAttribute("role", "option");
      btn.setAttribute("aria-label", s.title);
      btn.setAttribute("aria-selected", s.id === session.id ? "true" : "false");

      const av = document.createElement("div");
      av.className = "wechat-session-avatar";
      av.style.background = s.avatarBg;
      av.textContent = s.avatarText;

      const body = document.createElement("div");
      body.className = "wechat-session-meta";

      const top = document.createElement("div");
      top.className = "wechat-session-top";
      const nameSpan = document.createElement("span");
      nameSpan.className = "wechat-session-name";
      nameSpan.textContent = s.title;
      const timeSpan = document.createElement("span");
      timeSpan.className = "wechat-session-time";
      timeSpan.textContent = s.time;
      top.appendChild(nameSpan);
      top.appendChild(timeSpan);

      const prev = document.createElement("div");
      prev.className = "wechat-session-preview";
      prev.textContent = summarizeWeChatMessage(lastMsg);

      body.appendChild(top);
      body.appendChild(prev);
      btn.appendChild(av);
      btn.appendChild(body);

      btn.addEventListener("click", () => {
        activeWeChatSessionId = s.id;
        renderWeChat(true);
      });
      wechatSessionsListEl.appendChild(btn);
    }
  }

  renderWeChatMessages(scrollToBottom);
  renderWeChatDraft();
}

function openWeChatWindow(sessionId) {
  if (sessionId && wechatSessions.some((s) => s.id === sessionId)) {
    activeWeChatSessionId = sessionId;
  }
  wechatOpen = true;
  focusDesktopWindow("wechat");
  renderWeChat(true);
}

function closeWeChatWindow() {
  wechatOpen = false;
  wechatMaximized = false;
  if (explorerOpen) {
    focusDesktopWindow("explorer");
  }
  renderWeChat();
}

function toggleWeChatWindow() {
  if (wechatOpen && (focusedDesktopWindow === "wechat" || !explorerOpen)) {
    wechatOpen = false;
    if (explorerOpen) {
      focusDesktopWindow("explorer");
    }
    renderWeChat();
  } else {
    wechatOpen = true;
    focusDesktopWindow("wechat");
    renderWeChat(true);
    if (wechatInputEl) {
      wechatInputEl.focus();
    }
  }
}

function toggleWeChatMaximized() {
  if (!wechatOpen) {
    wechatOpen = true;
  }
  focusDesktopWindow("wechat");
  wechatMaximized = !wechatMaximized;
  renderWeChat(true);
}

function pasteCardIntoWeChat(card) {
  if (!canPasteCardIntoWeChat(card))
    return false;
  if (!wechatOpen) {
    wechatOpen = true;
  }
  focusDesktopWindow("wechat");

  if (card.kind === "image") {
    const dataUrl = getCardImageDataUrl(card);
    if (dataUrl) {
      const w = (card.source && card.source.width) || 1920;
      const h = (card.source && card.source.height) || 1080;
      wechatDraftAttachments.push({
        id: `wc-att-${++wechatAttachmentSeq}`,
        type: "image",
        dataUrl,
        name: t("screenshotSize", w, h),
      });
    }
  } else if (card.kind === "files" || card.kind === "folder") {
    for (const member of card.members || [])
      wechatDraftAttachments.push(
          {id: H.id(), type: "file", fileName: member.name, ext: member.ext, meta: member.type, member});
  } else {
    const content = card.text || (card.kind === "rich" ? card.notice : "");
    if (wechatInputEl && content) {
      wechatInputEl.setRangeText(content, wechatInputEl.selectionStart, wechatInputEl.selectionEnd, "end");
    }
  }

  renderWeChat(false);
  if (wechatInputEl) {
    wechatInputEl.focus();
  }
  return true;
}

function sendWeChatMessage() {
  const session = getActiveWeChatSession();
  if (!session)
    return false;
  const textVal = wechatInputEl ? wechatInputEl.value.trim() : "";
  if (wechatDraftAttachments.length === 0 && !textVal) {
    return false;
  }

  const now = new Date();
  const timeStr = `${String(now.getHours()).padStart(2, "0")}:${String(now.getMinutes()).padStart(2, "0")}`;

  for (const att of wechatDraftAttachments) {
    if (att.type === "image") {
      session.messages.push({
        id: `msg-${++wechatMsgSeq}`,
        sender: "self",
        author: t("me"),
        avatarText: t("me"),
        avatarBg: "#07c160",
        type: "image",
        dataUrl: att.dataUrl,
        alt: att.name || t("sentImage"),
      });
    } else {
      session.messages.push({
        id: `msg-${++wechatMsgSeq}`,
        sender: "self",
        author: t("me"),
        avatarText: t("me"),
        avatarBg: "#07c160",
        type: "file",
        fileName: att.fileName,
        ext: att.ext,
        meta: att.meta,
        member: att.member,
      });
    }
  }

  if (textVal) {
    session.messages.push({
      id: `msg-${++wechatMsgSeq}`,
      sender: "self",
      author: t("me"),
      avatarText: t("me"),
      avatarBg: "#07c160",
      type: "text",
      text: textVal,
    });
  }

  session.time = timeStr;
  wechatDraftAttachments = [];
  if (wechatInputEl) {
    wechatInputEl.value = "";
  }
  renderWeChat(true);
  return true;
}

if (wechatWin) {
  wechatWin.addEventListener("pointerdown", () => {
    focusDesktopWindow("wechat");
  });
}

if (wechatBtn) {
  wechatBtn.addEventListener("click", () => {
    toggleWeChatWindow();
  });
}

if (wechatMinBtn) {
  wechatMinBtn.addEventListener("click", () => {
    wechatOpen = false;
    if (explorerOpen) {
      focusDesktopWindow("explorer");
    }
    renderWeChat();
  });
}

if (wechatMaxBtn) {
  wechatMaxBtn.addEventListener("click", () => {
    toggleWeChatMaximized();
  });
}

if (wechatCloseBtn) {
  wechatCloseBtn.addEventListener("click", () => {
    closeWeChatWindow();
  });
}

if (wechatSendBtn) {
  wechatSendBtn.addEventListener("click", () => {
    sendWeChatMessage();
  });
}

if (wechatInputEl) {
  wechatInputEl.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      sendWeChatMessage();
    }
  });
}

if (wechatTitlebar) {
  wechatTitlebar.addEventListener("dblclick", (e) => {
    if (e.target.closest("button"))
      return;
    toggleWeChatMaximized();
  });

  wechatTitlebar.addEventListener("pointerdown", (e) => {
    if (e.button !== 0 || wechatMaximized)
      return;
    if (e.target.closest("button"))
      return;
    wechatTitlebar.setPointerCapture(e.pointerId);
    wechatDrag = {
      pointerId: e.pointerId,
      startX: e.clientX,
      startY: e.clientY,
      origX: wechatPos.x,
      origY: wechatPos.y,
    };
  });

  wechatTitlebar.addEventListener("pointermove", (e) => {
    if (!wechatDrag || e.pointerId !== wechatDrag.pointerId)
      return;
    const maxX = Math.max(40, (window.innerWidth || 1536) - 240);
    const maxY = Math.max(40, (window.innerHeight || 864) - 120);
    wechatPos.x = Math.min(maxX, Math.max(0, wechatDrag.origX + (e.clientX - wechatDrag.startX)));
    wechatPos.y = Math.min(maxY, Math.max(0, wechatDrag.origY + (e.clientY - wechatDrag.startY)));
    syncWeChatPosition();
  });

  function finishWeChatDrag(e) {
    if (!wechatDrag || e.pointerId !== wechatDrag.pointerId)
      return;
    wechatDrag = null;
    if (wechatTitlebar.hasPointerCapture(e.pointerId)) {
      wechatTitlebar.releasePointerCapture(e.pointerId);
    }
  }

  wechatTitlebar.addEventListener("pointerup", finishWeChatDrag);
  wechatTitlebar.addEventListener("pointercancel", finishWeChatDrag);
}

renderWeChat();

// ---------- Windows 11 Snipping Tool (截图工具) ----------

const snipBtn = document.getElementById("taskbar-snipping-btn");
const snipOverlay = document.getElementById("win11-snip-overlay");
const snipCanvas = document.getElementById("snip-overlay-canvas");
const snipCtx = snipCanvas ? snipCanvas.getContext("2d") : null;
const snipModeRectBtn = document.getElementById("snip-mode-rect");
const snipModeFullBtn = document.getElementById("snip-mode-fullscreen");
const snipCloseBtn = document.getElementById("snip-close-btn");
const projectLogoImg = document.getElementById("project-logo");
const snapshotLogo = location.protocol === "file:" ? trayBtn.querySelector("img") : projectLogoImg;

let snipActive = false;
let snipSnapshotCanvas = null;
let snipDrag = null;
let snipCurrentRect = null;
let snipCounter = 0;

// Composites the live webpage (desktop background, centered logo, hanging rope workspace,
// floating cards, and bottom taskbar) into a high-DPI offscreen canvas snapshot.
function captureWebpageSnapshot() {
  const dpr = window.devicePixelRatio || 1;
  const w = window.innerWidth || 1536;
  const h = window.innerHeight || 864;
  const off = document.createElement("canvas");
  off.width = Math.max(1, Math.round(w * dpr));
  off.height = Math.max(1, Math.round(h * dpr));
  const sCtx = off.getContext("2d");
  sCtx.scale(dpr, dpr);

  // 1. Desktop backdrop radial gradient & warm architectural bloom
  const cx = w * 0.5;
  const cy = h * 0.48;
  const maxR = Math.hypot(w * 0.68, h * 0.58);
  const bgGrad = sCtx.createRadialGradient(cx, cy, 0, cx, cy, maxR);
  bgGrad.addColorStop(0, "#fffcf5");
  bgGrad.addColorStop(0.48, "#f7f3ea");
  bgGrad.addColorStop(1, "#ece6da");
  sCtx.fillStyle = bgGrad;
  sCtx.fillRect(0, 0, w, h);

  const haloGrad = sCtx.createRadialGradient(cx, cy, 0, cx, cy, 340);
  haloGrad.addColorStop(0, "rgba(218, 178, 124, 0.18)");
  haloGrad.addColorStop(0.52, "rgba(218, 178, 124, 0.06)");
  haloGrad.addColorStop(1, "rgba(218, 178, 124, 0)");
  sCtx.fillStyle = haloGrad;
  sCtx.fillRect(0, 0, w, h);

  // Subtle dot grid
  sCtx.fillStyle = "rgba(140, 108, 72, 0.045)";
  for (let gx = 14; gx < w; gx += 28) {
    for (let gy = 14; gy < h - 48; gy += 28) {
      sCtx.beginPath();
      sCtx.arc(gx, gy, 0.75, 0, Math.PI * 2);
      sCtx.fill();
    }
  }

  // 2. Centered ClipRope Project Icon
  if (snapshotLogo?.complete && snapshotLogo.naturalWidth > 0) {
    const lRect = projectLogoImg.getBoundingClientRect();
    sCtx.save();
    sCtx.shadowColor = "rgba(78, 54, 28, 0.16)";
    sCtx.shadowBlur = 26;
    sCtx.shadowOffsetY = 12;
    sCtx.drawImage(snapshotLogo, lRect.left, lRect.top, lRect.width, lRect.height);
    sCtx.restore();
  }

  // 2b. Desktop Folder Icons (#desktop-icons)
  for (const btn of desktopFolders) {
    const bRect = btn.getBoundingClientRect();
    if (bRect.width <= 0 || bRect.height <= 0)
      continue;
    const fx = bRect.left + bRect.width / 2;
    const fy = bRect.top + 28;
    // Folder back tab
    sCtx.fillStyle = "#e09f1f";
    sCtx.beginPath();
    sCtx.roundRect(fx - 20, fy - 16, 18, 8, 3);
    sCtx.fill();
    // Folder front body
    sCtx.fillStyle = "#f6be3b";
    sCtx.beginPath();
    sCtx.roundRect(fx - 20, fy - 11, 40, 27, 4);
    sCtx.fill();
    // Label
    const labelEl = btn.querySelector(".desktop-folder-name");
    if (labelEl) {
      sCtx.fillStyle = "#33261a";
      sCtx.font = '500 11.5px "Segoe UI", system-ui, sans-serif';
      sCtx.textAlign = "center";
      sCtx.textBaseline = "top";
      sCtx.fillText(labelEl.textContent || "", fx, bRect.top + 54);
    }
  }

  // 2c. Open Windows 11 File Explorer Window (#win11-explorer, below ClipRope workspace)
  if (explorerOpen && explorerWin) {
    const eRect = explorerWin.getBoundingClientRect();
    if (eRect.width > 0 && eRect.height > 0) {
      const folder = getActiveExplorerFolder();
      sCtx.save();
      sCtx.shadowColor = "rgba(38, 28, 16, 0.22)";
      sCtx.shadowBlur = 28;
      sCtx.shadowOffsetY = 12;
      sCtx.fillStyle = "rgba(250, 248, 244, 0.98)";
      sCtx.beginPath();
      sCtx.roundRect(eRect.left, eRect.top, eRect.width, eRect.height, explorerMaximized ? 0 : 10);
      sCtx.fill();
      sCtx.restore();

      // Titlebar & toolbar header
      sCtx.fillStyle = "rgba(238, 233, 224, 0.95)";
      sCtx.fillRect(eRect.left, eRect.top, eRect.width, 40);
      sCtx.fillStyle = "rgba(247, 244, 238, 0.96)";
      sCtx.fillRect(eRect.left, eRect.top + 40, eRect.width, 44);

      // Active tab pill
      sCtx.fillStyle = "#fcfaf6";
      sCtx.beginPath();
      sCtx.roundRect(eRect.left + 12, eRect.top + 8, 136, 32, [7, 7, 0, 0]);
      sCtx.fill();
      sCtx.fillStyle = "#2b2219";
      sCtx.font = '600 12px "Segoe UI", system-ui, sans-serif';
      sCtx.textAlign = "left";
      sCtx.textBaseline = "middle";
      sCtx.fillText(sampleView.folderName(folder), eRect.left + 36, eRect.top + 24);

      // Address bar
      sCtx.fillStyle = "#ffffff";
      sCtx.strokeStyle = "rgba(92, 72, 48, 0.16)";
      sCtx.lineWidth = 1;
      sCtx.beginPath();
      sCtx.roundRect(eRect.left + 12, eRect.top + 47, Math.max(120, eRect.width - 24), 30, 6);
      sCtx.fill();
      sCtx.stroke();
      sCtx.fillStyle = "#3c3024";
      sCtx.font = '400 12px "Segoe UI", system-ui, sans-serif';
      sCtx.fillText(`${t("thisPC")} › ${t("desktop")} › ${sampleView.folderName(folder)}`, eRect.left + 24, eRect.top + 62);

      // File rows preview
      let ry = eRect.top + 122;
      for (const file of folder.files) {
        if (ry + 30 > eRect.bottom - 30)
          break;
        sCtx.fillStyle = "#2f2419";
        sCtx.font = '400 12.5px "Segoe UI", system-ui, sans-serif';
        const display = sampleView.fileView(file, folder.id);
        sCtx.fillText(display.name, eRect.left + 216, ry + 14);
        sCtx.fillStyle = "#6e5d4b";
        sCtx.font = '400 11.5px "Segoe UI", system-ui, sans-serif';
        sCtx.fillText(display.type, eRect.left + 560, ry + 14);
        ry += 34;
      }
    }
  }

  // 2d. Open WeChat Window (#win11-wechat, below ClipRope workspace)
  if (wechatOpen && wechatWin) {
    const wRect = wechatWin.getBoundingClientRect();
    if (wRect.width > 0 && wRect.height > 0) {
      const session = sampleView.sessionView(getActiveWeChatSession());
      sCtx.save();
      sCtx.shadowColor = "rgba(32, 24, 14, 0.22)";
      sCtx.shadowBlur = 28;
      sCtx.shadowOffsetY = 12;
      sCtx.fillStyle = "#f5f5f5";
      sCtx.beginPath();
      sCtx.roundRect(wRect.left, wRect.top, wRect.width, wRect.height, wechatMaximized ? 0 : 10);
      sCtx.fill();
      sCtx.restore();

      // Left dark nav rail (54px)
      sCtx.fillStyle = "#2e2e2e";
      sCtx.fillRect(wRect.left, wRect.top, 54, wRect.height);

      // Middle session list (220px)
      sCtx.fillStyle = "#eae8e7";
      sCtx.fillRect(wRect.left + 54, wRect.top, 220, wRect.height);

      // Chat header
      sCtx.fillStyle = "#f5f5f5";
      sCtx.fillRect(wRect.left + 274, wRect.top, Math.max(0, wRect.width - 274), 48);
      sCtx.fillStyle = "#191919";
      sCtx.font = '600 13.5px "Segoe UI", system-ui, sans-serif';
      sCtx.textAlign = "left";
      sCtx.textBaseline = "middle";
      sCtx.fillText(session.title, wRect.left + 292, wRect.top + 24);

      // Composer area at bottom
      const compTop = wRect.bottom - 146;
      sCtx.fillStyle = "#ffffff";
      sCtx.fillRect(wRect.left + 274, compTop, Math.max(0, wRect.width - 274), 146);
    }
  }

  // 3. Top Hanging Rope Workspace Canvas (if open — ALWAYS ON TOP of File Explorer & WeChat!)
  if (workspace.shown && canvas) {
    const cRect = canvas.getBoundingClientRect();
    if (cRect.width > 0 && cRect.height > 0) {
      sCtx.drawImage(canvas, cRect.left, cRect.top, cRect.width, cRect.height);
    }
  }

  // 4. Desktop Floating Cards
  for (const {canvas: paper, rect} of workspace.floatingFrames) {
    sCtx.drawImage(paper, rect.left, rect.top, rect.width, rect.height);
  }

  // 5. Bottom Windows 11 Taskbar
  const tbY = h - 48;
  sCtx.fillStyle = "rgba(243, 240, 234, 0.92)";
  sCtx.fillRect(0, tbY, w, 48);
  sCtx.strokeStyle = "rgba(90, 70, 48, 0.14)";
  sCtx.lineWidth = 1;
  sCtx.beginPath();
  sCtx.moveTo(0, tbY + 0.5);
  sCtx.lineTo(w, tbY + 0.5);
  sCtx.stroke();

  // Taskbar center app icons (File Explorer, Snipping Tool, WeChat)
  const feX = w / 2 - 46;
  const stX = w / 2;
  const wcX = w / 2 + 46;
  const iconCY = tbY + 24;
  // File Explorer mini icon
  sCtx.fillStyle = "#f4b92c";
  sCtx.beginPath();
  sCtx.roundRect(feX - 10, iconCY - 8, 20, 15, 2.5);
  sCtx.fill();
  sCtx.fillStyle = "#1883d7";
  sCtx.fillRect(feX - 4, iconCY + 2, 8, 5);
  // Snipping Tool mini icon
  sCtx.fillStyle = "#ea4c27";
  sCtx.beginPath();
  sCtx.roundRect(stX - 10, iconCY - 8, 20, 15, 4);
  sCtx.fill();
  sCtx.strokeStyle = "#ffffff";
  sCtx.lineWidth = 1.5;
  sCtx.beginPath();
  sCtx.moveTo(stX - 4, iconCY + 3);
  sCtx.lineTo(stX + 5, iconCY - 3);
  sCtx.moveTo(stX - 4, iconCY - 3);
  sCtx.lineTo(stX + 5, iconCY + 3);
  sCtx.stroke();
  // WeChat mini icon
  sCtx.fillStyle = "#07c160";
  sCtx.beginPath();
  sCtx.arc(wcX - 2, iconCY - 1, 7.5, 0, Math.PI * 2);
  sCtx.fill();
  sCtx.fillStyle = "#38df7f";
  sCtx.beginPath();
  sCtx.arc(wcX + 4, iconCY + 3, 6, 0, Math.PI * 2);
  sCtx.fill();

  // Taskbar right clock & tray icon
  if (snapshotLogo?.complete && snapshotLogo.naturalWidth > 0 && trayBtn) {
    const tRect = trayBtn.getBoundingClientRect();
    sCtx.drawImage(snapshotLogo, tRect.left + 5, tRect.top + 9, 18, 18);
  }
  const timeEl = document.getElementById("clock-time");
  const dateEl = document.getElementById("clock-date");
  sCtx.fillStyle = "#2c2824";
  sCtx.font = '400 11.5px "Segoe UI", system-ui, sans-serif';
  sCtx.textAlign = "right";
  sCtx.textBaseline = "alphabetic";
  if (timeEl)
    sCtx.fillText(timeEl.textContent || "", w - 16, tbY + 20);
  if (dateEl)
    sCtx.fillText(dateEl.textContent || "", w - 16, tbY + 34);

  return off;
}

function renderSnipOverlay(selRect) {
  snipCurrentRect = selRect || null;
  if (!snipCanvas || !snipCtx)
    return;
  const dpr = window.devicePixelRatio || 1;
  const cssW = window.innerWidth || 1536;
  const cssH = window.innerHeight || 864;
  const targetW = Math.max(1, Math.round(cssW * dpr));
  const targetH = Math.max(1, Math.round(cssH * dpr));
  if (snipCanvas.width !== targetW || snipCanvas.height !== targetH) {
    snipCanvas.width = targetW;
    snipCanvas.height = targetH;
  }

  snipCtx.save();
  snipCtx.setTransform(1, 0, 0, 1, 0, 0);
  snipCtx.clearRect(0, 0, targetW, targetH);
  snipCtx.scale(dpr, dpr);

  // Dimmed screen backdrop
  snipCtx.fillStyle = "rgba(0, 0, 0, 0.42)";
  snipCtx.fillRect(0, 0, cssW, cssH);

  if (selRect && selRect.w > 1 && selRect.h > 1) {
    const {x, y, w, h} = selRect;
    // Clear selected rectangle so the live webpage beneath is 100% bright
    snipCtx.clearRect(x, y, w, h);

    // Outer subtle dark contrast border
    snipCtx.strokeStyle = "rgba(0, 0, 0, 0.45)";
    snipCtx.lineWidth = 2.5;
    snipCtx.strokeRect(x - 0.5, y - 0.5, w + 1, h + 1);

    // Crisp white Windows 11 selection border
    snipCtx.strokeStyle = "#ffffff";
    snipCtx.lineWidth = 1.5;
    snipCtx.strokeRect(x, y, w, h);

    // Corner accent handles (#60cdff)
    const cornerLen = Math.min(12, w / 2, h / 2);
    snipCtx.strokeStyle = "#60cdff";
    snipCtx.lineWidth = 2.5;
    snipCtx.lineCap = "round";
    snipCtx.beginPath();
    // Top-left
    snipCtx.moveTo(x, y + cornerLen);
    snipCtx.lineTo(x, y);
    snipCtx.lineTo(x + cornerLen, y);
    // Top-right
    snipCtx.moveTo(x + w - cornerLen, y);
    snipCtx.lineTo(x + w, y);
    snipCtx.lineTo(x + w, y + cornerLen);
    // Bottom-right
    snipCtx.moveTo(x + w, y + h - cornerLen);
    snipCtx.lineTo(x + w, y + h);
    snipCtx.lineTo(x + w - cornerLen, y + h);
    // Bottom-left
    snipCtx.moveTo(x + cornerLen, y + h);
    snipCtx.lineTo(x, y + h);
    snipCtx.lineTo(x, y + h - cornerLen);
    snipCtx.stroke();

    // Live pixel dimension badge (Windows 11 style)
    const pxW = Math.max(1, Math.round(w * dpr));
    const pxH = Math.max(1, Math.round(h * dpr));
    const dimLabel = `${pxW} \u00d7 ${pxH} px`;
    snipCtx.font = '600 11.5px "Segoe UI", system-ui, sans-serif';
    const tw = snipCtx.measureText(dimLabel).width;
    const bw = tw + 14;
    const bh = 22;
    const bx = Math.min(cssW - bw - 6, Math.max(6, x + w - bw));
    const by = y + h + 8 + bh < cssH ? y + h + 8 : Math.max(6, y - bh - 8);

    snipCtx.fillStyle = "rgba(32, 32, 32, 0.9)";
    snipCtx.beginPath();
    snipCtx.roundRect(bx, by, bw, bh, 5);
    snipCtx.fill();
    snipCtx.strokeStyle = "rgba(255, 255, 255, 0.18)";
    snipCtx.lineWidth = 1;
    snipCtx.stroke();

    snipCtx.fillStyle = "#ffffff";
    snipCtx.textAlign = "center";
    snipCtx.textBaseline = "middle";
    snipCtx.fillText(dimLabel, bx + bw / 2, by + bh / 2 + 0.5);
  }

  snipCtx.restore();
}

function openSnippingTool() {
  screenshotHover.cancel();
  if (snipActive) {
    closeSnippingTool();
    return;
  }
  snipSnapshotCanvas = captureWebpageSnapshot();
  snipActive = true;
  snipDrag = null;
  if (snipBtn) {
    snipBtn.classList.add("active");
  }
  if (snipOverlay) {
    snipOverlay.classList.add("is-active");
    snipOverlay.setAttribute("aria-hidden", "false");
  }
  renderSnipOverlay(null);
}

function closeSnippingTool() {
  snipActive = false;
  snipDrag = null;
  if (snipBtn) {
    snipBtn.classList.remove("active");
  }
  if (snipOverlay) {
    snipOverlay.classList.remove("is-active");
    snipOverlay.setAttribute("aria-hidden", "true");
  }
}

// Crops the selected rectangle from the webpage snapshot and hangs the new screenshot
// card at the head (#1/50) of the current rope!
function completeSnipCapture(rect) {
  if (!snipSnapshotCanvas) {
    snipSnapshotCanvas = captureWebpageSnapshot();
  }
  const dpr = window.devicePixelRatio || 1;
  const sx = Math.max(0, Math.round(rect.x * dpr));
  const sy = Math.max(0, Math.round(rect.y * dpr));
  const sw = Math.max(1, Math.min(snipSnapshotCanvas.width - sx, Math.round(rect.w * dpr)));
  const sh = Math.max(1, Math.min(snipSnapshotCanvas.height - sy, Math.round(rect.h * dpr)));

  const cropped = document.createElement("canvas");
  cropped.width = sw;
  cropped.height = sh;
  const cCtx = cropped.getContext("2d");
  cCtx.drawImage(snipSnapshotCanvas, sx, sy, sw, sh, 0, 0, sw, sh);

  closeSnippingTool();

  const newCard = {
    id: `snip-${Date.now()}-${++snipCounter}`,
    kind: "image",
    image_primary: true,
    source: {
      width: sw,
      height: sh,
    },
    dataUrl: cropped.toDataURL('image/png'),
    status: "complete",
    capabilities: "Image",
  };

  return workspace.capture(newCard);
}

if (snipBtn) {
  snipBtn.addEventListener("click", () => {
    openSnippingTool();
  });
}

if (snipCloseBtn) {
  snipCloseBtn.addEventListener("click", () => {
    closeSnippingTool();
  });
}

if (snipModeRectBtn) {
  snipModeRectBtn.addEventListener("click", () => {
    renderSnipOverlay(null);
  });
}

if (snipModeFullBtn) {
  snipModeFullBtn.addEventListener("click", () => {
    completeSnipCapture({
      x: 0,
      y: 0,
      w: window.innerWidth || 1536,
      h: window.innerHeight || 864,
    });
  });
}

if (snipCanvas) {
  snipCanvas.addEventListener("pointerdown", (e) => {
    if (!snipActive || e.button !== 0)
      return;
    snipCanvas.setPointerCapture(e.pointerId);
    snipDrag = {
      pointerId: e.pointerId,
      startX: e.clientX,
      startY: e.clientY,
      currX: e.clientX,
      currY: e.clientY,
    };
    renderSnipOverlay({x: e.clientX, y: e.clientY, w: 0, h: 0});
  });

  snipCanvas.addEventListener("pointermove", (e) => {
    if (!snipActive || !snipDrag || e.pointerId !== snipDrag.pointerId)
      return;
    snipDrag.currX = e.clientX;
    snipDrag.currY = e.clientY;
    const x = Math.min(snipDrag.startX, snipDrag.currX);
    const y = Math.min(snipDrag.startY, snipDrag.currY);
    const w = Math.abs(snipDrag.currX - snipDrag.startX);
    const h = Math.abs(snipDrag.currY - snipDrag.startY);
    renderSnipOverlay({x, y, w, h});
  });

  function finishSnipGesture(e) {
    if (!snipDrag || e.pointerId !== snipDrag.pointerId)
      return;
    const state = snipDrag;
    snipDrag = null;
    if (snipCanvas.hasPointerCapture(e.pointerId)) {
      snipCanvas.releasePointerCapture(e.pointerId);
    }
    const x = Math.min(state.startX, e.clientX);
    const y = Math.min(state.startY, e.clientY);
    const w = Math.abs(e.clientX - state.startX);
    const h = Math.abs(e.clientY - state.startY);
    if (w >= 8 && h >= 8) {
      completeSnipCapture({x, y, w, h});
    } else {
      renderSnipOverlay(null);
    }
  }

  snipCanvas.addEventListener("pointerup", finishSnipGesture);
  snipCanvas.addEventListener("pointercancel", finishSnipGesture);
}

window.addEventListener("keydown", (e) => {
  if (e.key === "Escape") {
    screenshotHover.cancel();
    if (snipActive) {
      e.preventDefault();
      closeSnippingTool();
    }
  }
});

window.addEventListener("resize", () => {
  syncExplorerPosition();
  syncWeChatPosition();
  if (snipActive)
    renderSnipOverlay(snipCurrentRect);
});
async function runDemoHooks() {
  if (window.location.search.includes("demo=hover-preview")) {
    const rope = workspace.active();
    const imgCard = rope.cards.find((c) => c.kind === "image");
    if (imgCard) {
      workspace.previewHover(imgCard);
    }
  } else if (window.location.search.includes("demo=floating")) {
    const rope = workspace.active();
    if (rope.cards.length >= 3) {
      await workspace.command(s => {
        const [c1, c2] = H.rope(s, s.active).cards.splice(0, 2);
        s.floats.push({card: c1, x: 220, y: 460}, {card: c2, x: 1360, y: 470});
      });
    }
  } else if (window.location.search.includes("demo=wechat")) {
    openWeChatWindow("dev-group");
  } else if (window.location.search.includes("demo=ctrl-drag")) {
    openExplorerFolder("specs");
    const rope = workspace.active();
    const fileCard = rope.cards.find((c) => c.kind === "files");
    if (fileCard) {
      await copyCardToExplorerFolder(fileCard, "specs");
      workspace.previewDrag(fileCard, 780, 560);
    }
  } else if (window.location.search.includes("demo=explorer-max")) {
    openExplorerFolder("screenshots");
    explorerMaximized = true;
    renderExplorer();
  } else if (window.location.search.includes("demo=explorer")) {
    openExplorerFolder("specs");
  } else if (window.location.search.includes("demo=snipped")) {
    await completeSnipCapture({x: innerWidth / 4, y: 60, w: innerWidth / 2, h: innerHeight / 2});
  } else if (window.location.search.includes("demo=snip")) {
    openSnippingTool();
    renderSnipOverlay({x: 640, y: 120, w: 640, h: 520});
  }
}

function sourceFile(folder, file) {
  return {...file, folderId: folder.id, parent: folder.path};
}
function bindTarget() {
  if (wechatOpen && focusedDesktopWindow === "wechat")
    return {
      kind: "wechat",
      key: activeWeChatSessionId,
      start: wechatInputEl.selectionStart,
      end: wechatInputEl.selectionEnd,
      text: wechatInputEl.value
    };
  if (explorerOpen && focusedDesktopWindow === "explorer")
    return {kind: "explorer", key: activeFolderId};
  return null;
}
function validTarget(target) {
  const now = bindTarget();
  return now && target && now.kind === target.kind && now.key === target.key &&
      (now.kind !== "wechat" ||
       (now.text === target.text && now.start === target.start && now.end === target.end));
}
function deliver(card, target) {
  if (!validTarget(target))
    return false;
  if (target.kind === "wechat")
    return pasteCardIntoWeChat(card);
  return copyCardToExplorerFolder(card, target.key, () => validTarget(target));
}
function messageFile(msg) {
  if (msg.member) return msg.member;
  for (const folder of explorerFolders) {
    const file = folder.files.find(f => f.name === msg.fileName);
    if (file) return sourceFile(folder, file);
  }
  U.notice(t("missingAttachment"));
}
function copyMessage(msg, column) {
  const selection = window.getSelection(), bubble = column.querySelector(".wechat-bubble-text");
  if (msg.type === "text")
    return workspace.capture(H.asText(
        bubble?.contains(selection?.anchorNode) && bubble?.contains(selection.focusNode) &&
            selection.toString() ||
        msg.text));
  if (msg.type === "image")
    return captureImage(msg.dataUrl, workspace.active().id);
  const member = messageFile(msg);
  if (member)
    return workspace.capture(H.asFiles([member]));
}
function blobURL(blob) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(reader.result);
    reader.onerror = () => reject(reader.error);
    reader.readAsDataURL(blob);
  });
}
async function captureImage(url, destination) {
  const image = new Image();
  image.src = url;
  await image.decode();
  return workspace.capture(
      {
        id: H.id(),
        kind: "image",
        image_primary: true,
        dataUrl: url,
        source: {width: image.naturalWidth, height: image.naturalHeight},
        status: "complete",
        capabilities: "Image"
      },
      destination);
}
async function importCard(destination) {
  try {
    if (navigator.clipboard?.read) {
      const items = await navigator.clipboard.read();
      for (const item of items) {
        const image = item.types.find(t => t.startsWith("image/"));
        if (image)
          return await captureImage(await blobURL(await item.getType(image)), destination);
        if (item.types.includes("text/plain")) {
          const text = await (await item.getType("text/plain")).text();
          if (text)
            return workspace.capture(H.asText(text), destination);
        }
      }
    } else if (navigator.clipboard?.readText) {
      const text = await navigator.clipboard.readText();
      if (text)
        return workspace.capture(H.asText(text), destination);
    }
  } catch { /* Browser permissions can change between explicit requests. */
  }
  const wrap = U.element("div", "property-fields"), label = U.element("label", "", t("pasteHere")),
        field = U.element("textarea");
  let image;
  field.onpaste = e => {
    const f = [...e.clipboardData.files].find(f => f.type.startsWith("image/"));
    if (f) {
      e.preventDefault();
      image = f;
      field.value = f.name || t("image");
    }
  };
  label.append(field);
  wrap.append(label);
  if (await U.dialog(t("importClipboard"), wrap, t("collect"))) {
    if (image)
      await captureImage(await blobURL(image), destination);
    else if (field.value)
      await workspace.capture(H.asText(field.value), destination);
  }
}
async function exportCard(card) {
  const text = card.members?.map(f => f.parent + "\\" + f.name).join("\n") || card.text;
  try {
    if (card.kind === "image") {
      const image = new Image();
      image.src = getCardImageDataUrl(card);
      await image.decode();
      const buffer = document.createElement("canvas");
      buffer.width = image.naturalWidth;
      buffer.height = image.naturalHeight;
      buffer.getContext("2d").drawImage(image, 0, 0);
      const blob = await new Promise(resolve => buffer.toBlob(resolve, "image/png"));
      await navigator.clipboard.write([new ClipboardItem({"image/png": blob})]);
    } else if (text)
      await navigator.clipboard.writeText(text);
    else {
      U.notice(t("noExport"));
      return;
    }
    U.notice(card.members ? t("pathsExported") : t("exported"));
  } catch {
    const field = U.element(text ? "textarea" : "img");
    if (text) {
      field.value = text;
      field.readOnly = true;
      field.setAttribute("aria-label", t("manualCopyField"));
    } else {
      field.src = getCardImageDataUrl(card);
      field.alt = t("copyImage");
    }
    const body = U.element("div", "file-preview");
    body.append(
        U.element(
            "p", "",
            text ? t("textDenied") : t("imageDenied")),
        field);
    const done = U.dialog(t("manualCopy"), body, t("close"), false);
    field.select?.();
    await done;
  }
}
async function reveal(card) {
  const members = card.members || [], ids = [...new Set(members.map(f => f.folderId))];
  let key = ids[0];
  if (ids.length > 1) {
    const list = U.element("select");
    list.setAttribute("aria-label", t("containingFolder"));
    for (const id of ids) {
      const folder = explorerFolders.find(f => f.id === id);
      const o = U.element("option", "", folder && sampleView.folderName(folder));
      o.value = id;
      list.append(o);
    }
    if (!await U.dialog(t("chooseFolder"), list, t("reveal")))
      return;
    key = list.value;
  }
  if (!explorerFolders.some(f => f.id === key)) {
    U.notice(t("locationMissing"));
    return;
  }
  openExplorerFolder(key);
  const names = new Set(members.filter(f => f.folderId === key).map(f => sampleView.fileView(f).name));
  selectedFiles = new Set(getActiveExplorerFolder().files.filter(f => names.has(sampleView.fileView(f, key).name)).map(f => f.name));
  renderExplorer();
  explorerFilesListEl.querySelector('[aria-selected="true"]')?.scrollIntoView({block: "nearest"});
}
function attachAppActions() {
  document.querySelector("#files-copy").onclick = () => {
    const folder = getActiveExplorerFolder(), files = folder.files.filter(f => selectedFiles.has(f.name));
    if (files.length)
      workspace.capture(H.asFiles(files.map(f => sourceFile(folder, f))));
  };
  document.querySelector("#files-paste").onclick = () => {
    const c = workspace.clipboard;
    if (c && canCopyCardToFolder(c))
      copyCardToExplorerFolder(c, activeFolderId);
    else
      U.notice(t("noFiles"));
  };
  const copy = document.querySelector("#chat-copy");
  copy.onpointerdown = e => e.preventDefault();
  copy.onclick = () => {
    const text = wechatInputEl.value.slice(wechatInputEl.selectionStart, wechatInputEl.selectionEnd);
    if (text)
      workspace.capture(H.asText(text));
    else
      U.notice(t("selectTextFirst"));
  };
  const paste = document.querySelector("#chat-paste");
  paste.onpointerdown = e => e.preventDefault();
  paste.onclick = () => {
    if (workspace.clipboard)
      pasteCardIntoWeChat(workspace.clipboard);
    else
      U.notice(t("clipboardEmpty"));
  };
}
async function boot() {
  let model = new H.History(
      {
        revision: 0,
        active: "main",
        ordinary: "main",
        rebinVisible: false,
        ropes,
        floats: [],
        folders: explorerFolders
      },
      new URLSearchParams(location.search).has("demo"));
  try {
    await model.ready;
  } catch {
    model = new H.History(model.state, true);
    U.notice(t("storageUnavailable"));
  }
  explorerFolders = model.state.folders;
  function refreshDesktop() {
    for (const button of desktopFolders) {
      const folder = explorerFolders.find(f => f.id === button.dataset.folderId);
      if (folder) {
        const name = sampleView.folderName(folder);
        button.querySelector(".desktop-folder-name").textContent = name;
        button.setAttribute("aria-label", t("openFolder", name));
      }
    }
  }
  refreshDesktop();
  workspace = window.createClipRopeWorkspace(model, {
    painter,
    cardView: sampleView.cardView,
    hover: screenshotHover,
    bindTarget,
    validTarget,
    deliver,
    importCard,
    exportCard,
    reveal,
    imageURL: getCardImageDataUrl,
    updated(next) {
      explorerFolders = next.folders;
      renderExplorer();
    },
    dragOver(x, y) {
      syncExternalDropHighlights(isExternalDropOverWeChat(x, y) ? null : findExternalDropFolder(x, y));
    },
    dragClear() {
      syncExternalDropHighlights(null);
    },
    drop(card, x, y) {
      if (isExternalDropOverWeChat(x, y))
        pasteCardIntoWeChat(card);
      else {
        const folder = findExternalDropFolder(x, y);
        if (folder)
          copyCardToExplorerFolder(card, folder);
      }
    }
  });
  painter.onImageReady = workspace.imageReady;
  attachAppActions();
  window.addEventListener("cliprope-language", () => {
    const chatScroll = wechatMessagesEl.scrollTop;
    const atBottom = wechatMessagesEl.scrollHeight - wechatMessagesEl.clientHeight - chatScroll < 2;
    refreshDesktop();
    renderExplorer();
    renderWeChat(atBottom);
    if (!atBottom) wechatMessagesEl.scrollTop = chatScroll;
    workspace.refreshLanguage();
    updateTaskbarClock();
  });
  renderExplorer();
  workspace.draw();
  if (document.readyState !== "complete")
    await new Promise(resolve => window.addEventListener("load", resolve, {once: true}));
  await runDemoHooks();
}
boot().catch(err => U.notice(err.message));
})();
