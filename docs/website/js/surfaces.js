(() => {
  "use strict";
  const {t} = window.ClipRopeI18n;
  function element(tag, cls, text) {
    const node = document.createElement(tag);
    if (cls)
      node.className = cls;
    if (text != null)
      node.textContent = text;
    return node;
  }
  function button(label, action, cls) {
    const node = element("button", cls, label);
    node.type = "button";
    node.addEventListener("click", action);
    return node;
  }
  const status = element("div", "demo-status");
  status.setAttribute("role", "status");
  document.body.append(status);
  window.addEventListener("cliprope-language", () => status.classList.remove("visible"));
  let timer;
  function notice(message) {
    clearTimeout(timer);
    status.textContent = message;
    status.classList.add("visible");
    timer = setTimeout(() => status.classList.remove("visible"), 4200);
  }
  let popup;
  function closeMenu() {
    popup?.remove();
    popup = null;
  }
  function menu(x, y, title, items) {
    closeMenu();
    popup = element("div", "demo-menu");
    popup.setAttribute("role", "menu");
    popup.setAttribute("aria-label", title);
    popup.append(element("div", "menu-caption", title));
    for (const [name, action] of items) {
      if (!name)
        continue;
      const item = button(name, () => {
        closeMenu();
        Promise.resolve().then(action).catch(e => notice(e.message));
      });
      item.setAttribute("role", "menuitem");
      item.disabled = !action;
      popup.append(item);
    }
    document.body.append(popup);
    const r = popup.getBoundingClientRect();
    popup.style.left = Math.max(5, Math.min(innerWidth - r.width - 5, x)) + "px";
    popup.style.top = Math.max(5, Math.min(innerHeight - r.height - 5, y)) + "px";
    popup.querySelector("button:enabled")?.focus({preventScroll: true});
  }
  document.addEventListener("pointerdown", e => {
    if (popup && !popup.contains(e.target))
      closeMenu();
  }, true);
  document.addEventListener("keydown", e => {
    if (!popup)
      return;
    if (e.key === "Escape")
      closeMenu();
    if (["ArrowUp", "ArrowDown"].includes(e.key)) {
      e.preventDefault();
      const buttons = [...popup.querySelectorAll("button:enabled")];
      buttons[(buttons.indexOf(document.activeElement) + (e.key === "ArrowUp" ? -1 : 1) + buttons.length) % buttons.length]?.focus();
    }
  });
  function dialog(title, body, accept = t("ok"), cancellable = true) {
    return new Promise(resolve => {
      const panel = element("dialog", "demo-dialog");
      panel.setAttribute("aria-label", title);
      const form = element("form");
      form.method = "dialog";
      const heading = element("h2", "", title);
      form.append(heading, typeof body === "string" ? element("p", "", body) : body);
      const footer = element("div", "dialog-actions");
      if (cancellable)
        footer.append(button(t("cancel"), () => panel.close("cancel")));
      const ok = element("button", "demo-primary", accept);
      ok.value = "accept";
      footer.append(ok);
      form.append(footer);
      panel.append(form);
      document.body.append(panel);
      panel.addEventListener("close", () => {
        panel.remove();
        resolve(panel.returnValue === "accept");
      }, {once: true});
      panel.showModal();
      (form.querySelector("input:not([readonly]),textarea") || footer.firstElementChild).focus();
    });
  }
  window.ClipRopeUI = {element, button, menu, closeMenu, notice, dialog};
})();
