/* Browser UI strings. Literal visitor content stays outside this catalog. */
window.ClipRopeI18n = (() => {
  "use strict";
  const messages = {
    pageTitle: ["ClipRope · Windows 11 晾衣绳式剪贴板历史管理器", "ClipRope · A clothesline for your Windows clipboard"],
    description: ["ClipRope 是一款面向 Windows 11 的高性能持久化剪贴板历史管理器，采用温暖拟物的麻绳、木夹与纸张卡片工作区，并支持桌面独立悬浮卡片。", "ClipRope keeps your Windows 11 clipboard history on a rope with wooden pegs and paper cards. Pull a card down to keep it on your desktop."],
    intro: ["ClipRope 主工作区将每一次复制的文本、文件与截图挂在屏幕顶部的麻绳上。双击卡片即可粘贴回原窗口，拖下晾衣绳即可变为独立置顶悬浮便签。", "Copied text, files and screenshots hang from the rope at the top of the screen. Double-click a card to paste it back, or drag it off the rope to keep it as a floating note."],
    desktopEnvironment: ["Windows 11 桌面环境", "Windows 11 desktop"],
    logo: ["ClipRope 项目图标", "ClipRope logo"],
    desktopFolders: ["桌面文件夹", "Desktop folders"],
    explorerWindow: ["Windows 11 文件资源管理器", "Windows 11 File Explorer"],
    folderTabs: ["文件夹标签页", "Folder tabs"],
    minimizeToTray: ["最小化到任务栏", "Minimize to taskbar"],
    minimize: ["最小化", "Minimize"],
    maximizeOrRestore: ["最大化或还原窗口", "Maximize or restore window"],
    maximize: ["最大化", "Maximize"],
    restore: ["向下还原", "Restore down"],
    closeExplorer: ["关闭文件资源管理器", "Close File Explorer"],
    close: ["关闭", "Close"],
    copySelection: ["复制所选", "Copy selected"],
    paste: ["粘贴", "Paste"],
    thisPC: ["此电脑", "This PC"],
    desktop: ["桌面", "Desktop"],
    navigationPane: ["导航窗格", "Navigation pane"],
    name: ["名称", "Name"],
    type: ["类型", "Type"],
    size: ["大小", "Size"],
    folderContents: ["当前文件夹内容", "Folder contents"],
    wechatWindow: ["微信聊天窗口", "WeChat chat window"],
    chatList: ["聊天会话列表", "Chats"],
    search: ["搜索", "Search"],
    conversationList: ["会话列表", "Conversations"],
    closeChat: ["关闭聊天窗口", "Close chat"],
    messages: ["聊天消息记录", "Chat messages"],
    messagePlaceholder: ["输入消息…", "Type a message…"],
    composer: ["聊天消息输入框", "Message composer"],
    copySelectedText: ["复制所选文字", "Copy selected text"],
    send: ["发送(S)", "Send"],
    workspace: ["ClipRope 主工作区", "ClipRope workspace"],
    ropeWorkspace: ["晾衣绳工作区", "Rope workspace"],
    floatingLayer: ["桌面置顶悬浮卡片层", "Floating desktop cards"],
    taskbar: ["Windows 11 任务栏", "Windows 11 taskbar"],
    taskbarApps: ["任务栏应用", "Taskbar apps"],
    explorerLauncher: ["文件资源管理器（点击打开或切换文件夹焦点）", "Open or switch to File Explorer"],
    explorer: ["文件资源管理器", "File Explorer"],
    snipLauncher: ["截图工具（点击框选网页截图并自动挂上晾衣绳）", "Capture part of the page and hang the screenshot on the rope"],
    snippingTool: ["截图工具", "Snipping Tool"],
    wechatLauncher: ["微信（点击打开聊天对话框，支持粘贴与拖入文本、图片、文件并发送）", "Open WeChat to paste, drop or send text, images and files"],
    wechat: ["微信", "WeChat"],
    tray: ["系统托盘", "System tray"],
    hiddenIcons: ["显示隐藏的图标", "Show hidden icons"],
    toggleWorkspace: ["点击打开或最小化 ClipRope 主工作区", "Show or hide the ClipRope workspace"],
    workspaceTip: ["ClipRope · 点击隐藏/打开主工作区", "ClipRope · Show or hide the workspace"],
    clock: ["系统时间与日期", "Time and date"],
    snipToolbar: ["Windows 11 截图工具栏", "Snipping toolbar"],
    rectangle: ["矩形截图模式", "Rectangular snip"],
    rectangleTip: ["矩形模式（拖拽框选截图区域）", "Rectangular snip (drag to select an area)"],
    fullScreen: ["全屏截图", "Full-screen snip"],
    fullScreenTip: ["全屏模式（立即截取整个网页并挂入绳子）", "Capture the entire page and add it to the rope"],
    closeSnip: ["关闭截图工具", "Close Snipping Tool"],
    closeEsc: ["关闭 (Esc)", "Close (Esc)"],
    openFolder: ["打开{0}文件夹", "Open folder: {0}"],
    selectFile: ["选择 {0}", "Select {0}"],
    openFile: ["打开 {0}", "Open {0}"],
    items: ["{0} 个项目", "{0} items"],
    oneItem: ["{0} 个项目", "{0} item"],
    selectedItems: ["已选择 {0} 个项目", "{0} items selected"],
    oneSelected: ["已选择 {0} 个项目", "{0} item selected"],
    fileMessage: ["[文件] {0}", "[File] {0}"],
    imageMessage: ["[图片]", "[Image]"],
    imagePreview: ["截图预览", "Screenshot preview"],
    image: ["图像", "Image"],
    file: ["文件", "File"],
    folder: ["文件夹", "Folder"],
    removeAttachment: ["移除附件", "Remove attachment"],
    me: ["我", "Me"],
    friend: ["友", "F"],
    chatImage: ["聊天图片", "Chat image"],
    fileKind: ["{0} 文件", "{0} file"],
    copy: ["复制", "Copy"],
    sentImage: ["发送的图片", "Sent image"],
    screenshot: ["截图", "Screenshot"],
    screenshotSize: ["截图 ({0}×{1})", "Screenshot ({0}×{1})"],
    missingAttachment: ["此附件暂无可用的演示文件", "This attachment has no demo file."],
    pasteHere: ["请在此粘贴文字或图片", "Paste text or an image here"],
    importClipboard: ["从系统粘入", "Import from system clipboard"],
    collect: ["收集", "Collect"],
    noExport: ["此内容没有可导出的格式", "This content has no exportable format."],
    pathsExported: ["文件路径已复制到系统", "File paths copied to the system clipboard."],
    exported: ["已复制到系统剪贴板", "Copied to the system clipboard."],
    manualCopyField: ["选中后手动复制", "Select and copy manually"],
    copyImage: ["右键复制图片", "Right-click to copy the image"],
    textDenied: ["系统未允许直接写入。请选中文字后复制。", "Clipboard access was denied. Select the text and copy it manually."],
    imageDenied: ["系统未允许直接写入。请右键复制图片。", "Clipboard access was denied. Right-click the image to copy it."],
    manualCopy: ["手动复制", "Copy manually"],
    containingFolder: ["文件所在文件夹", "Containing folder"],
    chooseFolder: ["选择所在文件夹", "Choose a containing folder"],
    reveal: ["定位", "Show in folder"],
    locationMissing: ["文件位置不可用", "File location unavailable."],
    noFiles: ["演示剪贴板里暂无可粘贴的文件或截图", "The demo clipboard has no files or screenshots to paste."],
    selectTextFirst: ["请先在输入框中选中文字", "Select some text in the composer first."],
    clipboardEmpty: ["演示剪贴板为空", "The demo clipboard is empty."],
    storageUnavailable: ["浏览器存储不可用，本次操作仅在当前页面保留", "Browser storage is unavailable. Changes will last only for this session."],
    targetChanged: ["粘贴目标已改变，内容仍在演示剪贴板", "The paste destination changed. The content is still on the demo clipboard."],
    fileMissing: ["文件夹或文件不可用", "The folder or file is unavailable."],
    scrollRopes: ["上下浏览绳子", "Scroll through ropes"],
    headUnavailable: ["绳首内容无法复制，已保留剪贴板", "The first card could not be copied. The clipboard was preserved."],
    ropeMenu: ["{0} 绳子菜单", "{0} rope menu"],
    deleteCard: ["删除卡片", "Delete card"],
    editText: ["编辑文字", "Edit text"],
    returnCard: ["回绳", "Return"],
    recycleFloating: ["回收悬浮卡片", "Recycle floating card"],
    collected: ["已收集到 {0}", "Added to {0}"],
    cardMoved: ["卡片已移动", "The card has moved."],
    textChanged: ["文字已变更，请重新打开编辑器", "The text has changed. Reopen the editor."],
    cardText: ["卡片文字", "Card text"],
    cancel: ["取消", "Cancel"],
    save: ["保存", "Save"],
    noMatch: ["无匹配", "No matches"],
    enterKeywords: ["输入关键词", "Enter a search term"],
    findCards: ["查找卡片", "Find cards"],
    previousMatch: ["上一处", "Previous match"],
    nextMatch: ["下一处", "Next match"],
    ropeName: ["绳名", "Rope name"],
    capacity: ["容量", "Capacity"],
    unlimited: ["无限", "Unlimited"],
    editRope: ["修改绳子", "Edit rope"],
    newRope: ["新建绳子", "New rope"],
    invalidRope: ["请输入绳名和有效的正整数容量", "Enter a rope name and a positive whole number for its capacity."],
    reduceCapacity: ["缩小容量", "Reduce capacity"],
    removeTail: ["将移除绳尾的 {0} 张卡片：\n{1}", "Cards to remove from the end ({0}):\n{1}"],
    removeAndSave: ["移除并保存", "Remove and save"],
    ropeChanged: ["绳子内容已变化，请重新确认", "The rope has changed. Review it again."],
    switchCollector: ["切换收集绳？", "Switch collecting rope?"],
    collectorPrompt: ["之后复制的内容将挂到 {0}。", "New copies will go to {0}."],
    switch: ["切换", "Switch"],
    singleRope: ["仅看这根绳", "Show only this rope"],
    overview: ["显示全部绳子", "Show all ropes"],
    previousRope: ["上一根绳", "Previous rope"],
    nextRope: ["下一根绳", "Next rope"],
    firstCard: ["回到首卡", "Go to first card"],
    regularView: ["常规显示", "Regular view"],
    compactView: ["紧凑显示", "Compact view"],
    hideRebin: ["隐藏 ReBin", "Hide ReBin"],
    showRebin: ["显示 ReBin", "Show ReBin"],
    newRopeAction: ["新建绳子…", "New rope…"],
    editRopeAction: ["修改绳子…", "Edit rope…"],
    clearRopeAction: ["清空绳子…", "Clear rope…"],
    clearRopeTitle: ["清空 {0}？", "Clear {0}?"],
    clearRopeBody: ["移除这根绳上的 {0} 张挂卡。", "Remove all hanging cards from this rope ({0})."],
    clear: ["清空", "Clear"],
    deleteRopeAction: ["删除绳子…", "Delete rope…"],
    deleteRopeTitle: ["删除 {0}？", "Delete {0}?"],
    deleteRopeBody: ["这根绳子和它的挂卡将一起删除。", "This rope and its hanging cards will be deleted."],
    delete: ["删除", "Delete"],
    outOfRoom: ["容量不足", "Not enough room"],
    requiredCapacity: ["此位置至少需要 {0} 个位置才能保留整组。", "Keeping the entire group here requires a capacity of at least {0}."],
    gotIt: ["知道了", "Got it"],
    expandGroup: ["展开文件组", "Expand file group"],
    groupTail: ["将移除 {0} 的尾卡：\n{1}", "These cards will be removed from the end of {0}:\n{1}"],
    expand: ["展开", "Expand"],
    groupMoved: ["文件组位置已变化，请重试", "The file group has moved. Try again."],
    openPreview: ["打开预览", "Open preview"],
    showInExplorer: ["在资源管理器中定位", "Show in File Explorer"],
    copyToDemo: ["复制到演示剪贴板", "Copy to demo clipboard"],
    copiedToDemo: ["已复制到演示剪贴板", "Copied to demo clipboard."],
    copyToSystem: ["复制到系统", "Copy to system clipboard"],
    returnCurrent: ["回到当前绳", "Return to current rope"],
    moveTo: ["移到 {0}", "Move to {0}"],
    copyTo: ["复制到 {0}", "Copy to {0}"],
    moveRebin: ["移入 ReBin", "Move to ReBin"],
    quickNote: ["快速笔记", "Quick note"],
    recent: ["{0} · 最近使用", "{0} · Recent"],
    card: ["卡片", "Card"],
    cardMissing: ["卡片已被移走或删除", "The card has been moved or deleted."],
    ropeMissing: ["目标绳已被删除", "The destination rope was deleted."],
    nothingToCopy: ["这张卡片没有可复制的内容", "This card has nothing to copy."],
    closeOtherTabs: ["请关闭其他演示标签页后重试", "Close other demo tabs and try again."],
    historyConflict: ["另一个标签页已更新历史，请刷新后重试", "Another tab updated the history. Refresh and try again."],
    saveFailed: ["保存失败，原有历史已保留", "Could not save. Your previous history was preserved."],
    ok: ["确定", "OK"],
    chartTitle: ["ClipRope 延迟测试（毫秒）", "ClipRope latency (ms)"],
    ropeCount: ["共 {0} 根", "Ropes: {0}"],
    richContent: ["富文本", "Formatted content"],
    unavailable: ["内容不可用", "Content unavailable"],
    capText: ["文本", "Text"],
    capImage: ["图像", "Image"],
    capFiles: ["文件", "Files"],
    capPartial: ["部分", "Partial"],
    switchLanguage: ["切换到 English", "Switch to 简体中文"],
    guideLabel: ["用户手册", "User guide"],
    openGuide: ["用户手册（在新标签页中打开）", "User guide (opens in a new tab)"],
    pngType: ["PNG 图像", "PNG image"],
  };
  const preference = "ClipRope-Web-Language";
  let language = "zh-CN";
  try {
    if (localStorage.getItem(preference) === "en") language = "en";
  } catch { /* File origins and private sessions can restrict local storage. */ }
  const entryUrl = new URL(location.href);
  const entryLanguage = entryUrl.searchParams.get("lang");
  if (entryLanguage === "en" || entryLanguage === "zh-CN") language = entryLanguage;
  function t(key, ...args) {
    return messages[key][language === "en" ? 1 : 0].replace(/\{(\d+)\}/g, (_, i) => args[i]);
  }
  function refreshPage() {
    document.documentElement.lang = language;
    for (const el of document.querySelectorAll("[data-i18n]")) {
      for (const binding of el.dataset.i18n.split(" ")) {
        const [key, attr] = binding.split(":");
        if (attr) el.setAttribute(attr, t(key));
        else el.textContent = t(key);
      }
    }
    const button = document.getElementById("language-switch");
    button.querySelector(".language-full").textContent = language === "en" ? "简体中文" : "English";
    button.querySelector(".language-short").textContent = language === "en" ? "中" : "EN";
    button.lang = language === "en" ? "zh-CN" : "en";
    button.setAttribute("aria-label", t("switchLanguage"));
    button.title = t("switchLanguage");
    const guide = document.getElementById("guide-link");
    guide.href = language === "en" ? "../user/en/ClipRope-Docs.html" : "../user/ClipRope-Docs.html";
    guide.hreflang = language;
  }
  function toggle() {
    language = language === "en" ? "zh-CN" : "en";
    try { localStorage.setItem(preference, language); } catch { /* Keep the in-page choice. */ }
    const url = new URL(location.href);
    if (url.searchParams.has("lang")) {
      url.searchParams.set("lang", language);
      try { history.replaceState(history.state, "", url); } catch { /* Some file origins restrict URL changes. */ }
    }
    refreshPage();
    window.dispatchEvent(new Event("cliprope-language"));
  }
  refreshPage();
  document.getElementById("language-switch").addEventListener("click", toggle);
  return {t, get language() { return language; }};
})();
