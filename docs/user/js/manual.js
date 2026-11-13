(() => {
  "use strict";

  const root = document.documentElement;
  const english = root.lang === "en";
  const labels = english ? {
    light: "Switch to light theme", dark: "Switch to dark theme",
    openMenu: "Open table of contents", closeMenu: "Close table of contents",
    search: "Search the guide", close: "Close", query: "Search terms",
    placeholder: "Try “Copy only” or “ReBin”", results: "Matching sections",
    browse: "Browse the pages or type a term to find a specific action.",
    empty: "No matching sections. Try a shorter search term.",
    count: n => n === 1 ? "1 result" : `${n} results`
  } : {
    light: "切换为浅色手册", dark: "切换为深色手册",
    openMenu: "打开目录", closeMenu: "关闭目录",
    search: "搜索文档", close: "关闭", query: "搜索关键词",
    placeholder: "试试「仅复制」或「回收绳」", results: "匹配的章节",
    browse: "浏览页面，或输入关键词定位具体操作。",
    empty: "没有找到匹配的章节。可以试试更短的关键词。",
    count: n => `找到 ${n} 项结果`
  };
  // Without a local Windows icon font, keep the chapter labels and the SVG toolbar controls.
  document.fonts?.load('18px "ClipRope UI Icons"').then(faces => {
    if (faces.length) root.classList.add("has-fluent-icons");
  }).catch(() => {});
  const pageName = location.pathname.split("/").pop();
  const oldSections = new Map(Object.entries({
    start: "getting-started.html#start",
    concepts: "cards.html#forms",
    collect: "cards.html#preview",
    organize: "ropes.html#collector",
    paste: "getting-started.html#paste",
    floating: "cards.html#floating",
    editing: "cards.html#editing",
    settings: "settings.html#paste-mode",
    shortcuts: "reference.html#shortcuts",
    faq: "reference.html#faq"
  }));

  function redirectOldSection() {
    const destination = oldSections.get(location.hash.slice(1));
    if (pageName === "ClipRope-Docs.html" && destination) {
      location.replace(destination);
      return true;
    }
    return false;
  }
  if (redirectOldSection()) return;
  window.addEventListener("hashchange", redirectOldSection);

  const languageLink = document.querySelector(".language-link");
  const otherPage = new URL(languageLink.href);
  function syncLanguageLink() {
    otherPage.hash = location.hash;
    languageLink.href = otherPage.href;
  }
  syncLanguageLink();
  window.addEventListener("hashchange", syncLanguageLink);

  const themeButton = document.querySelector(".theme-toggle");
  const systemDark = window.matchMedia("(prefers-color-scheme: dark)");
  function updateThemeLabel() {
    const dark = root.dataset.theme ? root.dataset.theme === "dark" : systemDark.matches;
    const label = dark ? labels.light : labels.dark;
    themeButton.setAttribute("aria-label", label);
    themeButton.title = label;
  }
  function readTheme() {
    let theme;
    try { theme = localStorage.getItem("cliprope-docs-theme"); } catch { /* Storage is optional for local files. */ }
    if (theme === "light" || theme === "dark") root.dataset.theme = theme;
    else delete root.dataset.theme;
    updateThemeLabel();
  }
  readTheme();
  themeButton.hidden = false;
  themeButton.addEventListener("click", () => {
    const dark = root.dataset.theme ? root.dataset.theme === "dark" : systemDark.matches;
    root.dataset.theme = dark ? "light" : "dark";
    try { localStorage.setItem("cliprope-docs-theme", root.dataset.theme); } catch { /* Keep this page's choice. */ }
    updateThemeLabel();
  });
  systemDark.addEventListener("change", updateThemeLabel);
  window.addEventListener("pageshow", event => { if (event.persisted) readTheme(); });

  const nav = document.querySelector(".site-nav");
  const menu = document.querySelector(".menu-toggle");
  const narrow = window.matchMedia("(max-width: 720px)");
  function setMenu(open) {
    menu.hidden = !narrow.matches;
    nav.hidden = narrow.matches && !open;
    menu.setAttribute("aria-expanded", String(open && narrow.matches));
    menu.setAttribute("aria-label", open ? labels.closeMenu : labels.openMenu);
  }
  root.classList.add("has-menu");
  setMenu(false);
  narrow.addEventListener("change", () => setMenu(false));
  window.addEventListener("pageshow", () => setMenu(false));
  menu.addEventListener("click", () => {
    const open = nav.hidden;
    setMenu(open);
    if (open) nav.querySelector('[aria-current="page"]').focus();
  });
  nav.addEventListener("click", event => {
    const link = event.target.closest("a");
    if (!link || !narrow.matches || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
    event.preventDefault();
    setMenu(false);
    const destination = new URL(link.href);
    destination.hash = "main";
    if (destination.pathname === location.pathname) document.getElementById("main").focus();
    location.assign(destination.href);
  });
  document.addEventListener("click", event => {
    if (narrow.matches && !nav.hidden && !nav.contains(event.target) && !menu.contains(event.target)) setMenu(false);
  });
  document.addEventListener("focusin", event => {
    if (narrow.matches && !nav.hidden && !nav.contains(event.target) && event.target !== menu) setMenu(false);
  });
  document.addEventListener("keydown", event => {
    if (event.key === "Escape" && narrow.matches && !nav.hidden) {
      event.preventDefault();
      setMenu(false);
      menu.focus();
    }
  });

  const index = window.clipRopeSearchIndex;
  if (!Array.isArray(index) || !window.HTMLDialogElement) return;
  const trigger = document.querySelector(".search-trigger");
  const dialog = document.createElement("dialog");
  dialog.id = "manual-search";
  dialog.className = "search-dialog";
  dialog.setAttribute("aria-labelledby", "search-heading");
  dialog.innerHTML = `
    <div class="search-box">
      <header><h2 id="search-heading">${labels.search}</h2><button class="search-close" type="button">${labels.close}</button></header>
      <input type="search" role="combobox" aria-label="${labels.query}" aria-autocomplete="list" aria-expanded="true"
             aria-controls="search-results" placeholder="${labels.placeholder}" autocomplete="off">
    </div>
    <p class="search-status" role="status" aria-live="polite"></p>
    <div class="results" id="search-results" role="listbox" aria-label="${labels.results}"></div>`;
  document.body.append(dialog);
  const input = dialog.querySelector("input");
  const status = dialog.querySelector(".search-status");
  const results = dialog.querySelector(".results");
  let matches = [];
  let selected = -1;
  let opener;

  const normalize = value => value.normalize("NFKC").toLowerCase().trim().replace(/\s*\+\s*/g, "+");
  const searchable = index.map(item => ({
    item, title: normalize(item.title), page: normalize(item.page), body: normalize(item.body)
  }));
  function score(entry, terms) {
    let weight = 0;
    for (const term of terms) {
      if (entry.title === term) weight += 12;
      else if (entry.title.includes(term)) weight += 8;
      else if (entry.page.includes(term)) weight += 4;
      else if (entry.body.includes(term)) weight += 1;
      else return 0;
    }
    return weight;
  }
  function snippet(body, terms) {
    const text = body.toLowerCase();
    const offset = terms.map(term => text.indexOf(term)).find(i => i >= 0) ?? 0;
    const stops = english ? [". ", "! ", "? "] : ["。", "；", "！", "？"];
    const boundary = Math.max(...stops.map(stop => body.lastIndexOf(stop, Math.max(0, offset - 1))));
    let start = offset - boundary <= 40 ? boundary + 1 : Math.max(0, offset - 16);
    if (english && start && body[start] !== " ") start = body.lastIndexOf(" ", start) + 1;
    let end = start + (english ? 128 : 72);
    if (english && end < body.length) end = Math.max(body.lastIndexOf(" ", end), offset + 1);
    return (start ? "…" : "") + body.slice(start, end).trim() + (body.length > end ? "…" : "");
  }
  function selectResult(position, scroll = false) {
    if (selected >= 0) results.children[selected]?.setAttribute("aria-selected", "false");
    selected = position;
    const option = results.children[position];
    if (!option) {
      input.removeAttribute("aria-activedescendant");
      return;
    }
    option.setAttribute("aria-selected", "true");
    input.setAttribute("aria-activedescendant", option.id);
    if (scroll) option.scrollIntoView({ block: "nearest" });
  }
  function drawResults() {
    const query = normalize(input.value);
    const terms = query ? query.split(/\s+/) : [];
    matches = terms.length ? searchable.map(entry => ({ item: entry.item, weight: score(entry, terms) }))
      .filter(match => match.weight > 0).sort((a, b) => b.weight - a.weight).map(match => match.item)
      : index.filter(item => !item.url.includes("#"));
    results.replaceChildren();
    selected = -1;
    status.textContent = terms.length
      ? (matches.length ? labels.count(matches.length) : labels.empty)
      : labels.browse;
    matches.forEach((item, i) => {
      const option = document.createElement("div");
      option.className = "result";
      option.setAttribute("role", "option");
      option.setAttribute("aria-selected", "false");
      option.id = `search-result-${i}`;
      const chapter = document.createElement("small");
      chapter.textContent = item.page;
      const title = document.createElement("strong");
      title.textContent = item.title;
      const excerpt = document.createElement("span");
      excerpt.textContent = snippet(item.body, terms);
      if (item.page !== item.title) option.append(chapter);
      option.append(title, excerpt);
      option.addEventListener("pointermove", event => {
        if ((event.movementX || event.movementY) && selected !== i) selectResult(i);
      });
      option.addEventListener("click", () => follow(item));
      results.append(option);
    });
    selectResult(matches.length ? 0 : -1);
    results.scrollTop = 0;
  }
  function openSearch() {
    if (dialog.open) return;
    opener = document.activeElement;
    setMenu(false);
    input.value = "";
    drawResults();
    dialog.showModal();
    input.focus();
  }
  function follow(item) {
    const destination = new URL(item.url, location.href);
    opener = null;
    dialog.close();
    if (destination.pathname === location.pathname && destination.hash) {
      location.hash = destination.hash;
      const section = document.getElementById(destination.hash.slice(1));
      section.focus({ preventScroll: true });
      section.scrollIntoView({ block: "start" });
    } else {
      location.assign(destination.href);
    }
  }
  trigger.hidden = false;
  trigger.setAttribute("aria-controls", dialog.id);
  trigger.addEventListener("click", openSearch);
  dialog.querySelector(".search-close").addEventListener("click", () => dialog.close());
  dialog.addEventListener("click", event => { if (event.target === dialog) dialog.close(); });
  dialog.addEventListener("close", () => {
    if (opener && opener !== document.body && opener.checkVisibility()) opener.focus();
    else if (opener) trigger.focus();
    opener = null;
  });
  input.addEventListener("input", drawResults);
  input.addEventListener("keydown", event => {
    if (event.isComposing) return;
    if (event.key === "Escape") {
      event.preventDefault();
      dialog.close();
      return;
    }
    if (!matches.length) return;
    if (event.key === "ArrowDown" || event.key === "ArrowUp") {
      event.preventDefault();
      const delta = event.key === "ArrowDown" ? 1 : -1;
      selectResult((selected + delta + matches.length) % matches.length, true);
    } else if (event.key === "Enter") {
      event.preventDefault();
      follow(matches[selected]);
    }
  });
  document.addEventListener("keydown", event => {
    if (!event.isComposing && (event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k") {
      event.preventDefault();
      openSearch();
    }
  });
})();
