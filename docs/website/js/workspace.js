/* Canvas painting and pointer commands share the same row and paper geometry. */
window.createClipRopeWorkspace = function(model, host) {
  "use strict";
  const {t} = window.ClipRopeI18n;
  const H = window.ClipRopeHistory, U = window.ClipRopeUI;
  const {Viewport, RopeCurve, RopePlaque, Hanging, Swing, floatingSizeForCard} = window.ClipRopePaint;
  const stage = document.querySelector("#workspace-stage"),
        canvas = document.querySelector("#workspace-canvas");
  const layer = document.querySelector("#floating-layer"),
        overlay = document.querySelector("#drag-overlay-canvas");
  const tray = document.querySelector("#tray-cliprope-btn"), painter = host.painter;
  const paperHits = U.element("div", "paper-hits");
  const ns = "http://www.w3.org/2000/svg";
  const lines = document.createElementNS(ns, "svg");
  lines.classList.add("rope-lines");
  lines.setAttribute("aria-hidden", "true");
  const scroll = U.element("input", "overview-scroll");
  scroll.type = "range";
  scroll.min = "0";
  scroll.hidden = true;
  scroll.setAttribute("aria-label", t("scrollRopes"));
  scroll.oninput = () => {
    vertical = Number(scroll.value);
    draw();
  };
  stage.append(lines, paperHits, scroll);
  const targets = new Map(), floats = new Map(), views = new Map(), boxes = new Map();
  const swing = new Swing();
  let shown = true, overview = false, vertical = 0, rows = [], gesture, hover, editor, find, selection,
      clipboard;
  let frame = 0, lastTime = 0, targetAtPress, menuTimer, edgeTimer, busyEditor;
  const state = () => model.state;
  const label = card => H.label(host.cardView(card));
  const publish = card => H.publish(host.cardView(card));
  const active = () => H.rope(state(), state().active);
  const clamp = (v, low, high) => Math.max(low, Math.min(v, Math.max(low, high)));
  function context(c, w, h) {
    const d = devicePixelRatio || 1;
    const width = Math.max(1, Math.round(w * d)), height = Math.max(1, Math.round(h * d));
    if (c.width !== width || c.height !== height) {
      c.width = width;
      c.height = height;
    }
    const ctx = c.getContext("2d");
    ctx.setTransform(d, 0, 0, d, 0, 0);
    ctx.clearRect(0, 0, w, h);
    return ctx;
  }
  function queueDraw() {
    if (!frame)
      frame = requestAnimationFrame(now => {
        frame = 0;
        const moving = swing.advance(clamp((now - lastTime) / 1000, 0.001, 0.05));
        lastTime = now;
        draw();
        if (moving)
          queueDraw();
      });
  }
  async function command(edit, follow = true) {
    try {
      const {before, next} = await model.change(edit);
      const a = H.head(before), b = H.head(next);
      if (follow && b && (before.active !== next.active || a?.id !== b.id || a?.text !== b.text)) {
        try {
          clipboard = publish(b);
        } catch {
          U.notice(t("headUnavailable"));
        }
      }
      host.updated(next, before);
      if (find)
        updateMatches(false);
      draw();
      return true;
    } catch (err) {
      U.notice(err.message);
      draw();
      return false;
    }
  }
  function view(row) {
    let v = views.get(row.id);
    if (!v) {
      v = new Viewport(row.cards, innerWidth);
      views.set(row.id, v);
    }
    v.width = innerWidth;
    v.compact = !!row.compact;
    v.setCards(row.cards, true);
    return v;
  }
  function rowHeight(row) {
    const v = view(row);
    return row.cards.reduce((height, card) => Math.max(height, v.measure(card).bottom + 98), 275);
  }
  function layout() {
    const all = overview ? H.order(state()) : [active()];
    const heights = all.map(rowHeight);
    const available = Math.max(100, innerHeight - 48);
    const total = heights.reduce((a, b) => a + b, 0);
    vertical = overview ? clamp(vertical, 0, total - available) : 0;
    const top = overview ?
        0 :
        clamp(
            gesture?.type === "rope" && gesture.moved ? gesture.top : active().topPx || 0, 0,
            available - heights[0]);
    stage.style.setProperty("--rope-top", top + "px");
    const height = Math.min(available, overview ? total : heights[0]);
    canvas.style.height = height + "px";
    scroll.hidden = !overview || total <= available;
    scroll.max = String(Math.max(0, total - available));
    scroll.value = String(vertical);
    scroll.style.height = Math.max(50, height - 24) + "px";
    let y = -vertical;
    rows = all.map((row, i) => {
      const result = {row, y, height: heights[i], v: view(row), curve: new RopeCurve(innerWidth)};
      y += heights[i];
      return result;
    });
    return height;
  }
  function hit(key, parent, cls, title, data, x, y, w, h) {
    let node = targets.get(key);
    if (!node) {
      node = U.element("button", "paper-hit " + cls);
      node.type = "button";
      targets.set(key, node);
      parent.append(node);
    }
    node.dataset.used = "1";
    Object.assign(node.dataset, data);
    node.setAttribute("aria-label", title);
    if (data.part === "rope")
      node.title = title;
    Object.assign(node.style, {left: x + "px", top: y + "px", width: w + "px", height: h + "px"});
    return node;
  }
  function renderRows(ctx, height) {
    const visibleLines = new Set();
    for (const lane of rows) {
      const {row, curve, v, y} = lane;
      if (y + lane.height < 0 || y >= height)
        continue;
      ctx.save();
      ctx.translate(0, y);
      let path = Array.from(lines.children).find(p => p.dataset.rope === row.id);
      if (!path) {
        path = document.createElementNS(ns, "path");
        path.dataset.rope = row.id;
        path.dataset.part = "rope";
        lines.append(path);
      }
      visibleLines.add(path);
      const signature = [innerWidth, y].join(",");
      if (path.dataset.geometry !== signature) {
        const points = [];
        for (let i = 0; i <= 40; i++) {
          const x = innerWidth * i / 40;
          points.push((i ? "L" : "M") + x + " " + (curve.y(x) + y));
        }
        path.setAttribute("d", points.join(""));
        path.dataset.geometry = signature;
      }
      const px = (innerWidth - RopePlaque.width) / 2;
      const py = curve.lowest() + 0.4 - RopePlaque.gap - RopePlaque.height;
      const preview = new window.ClipRopePaint.InsertionPreview();
      if (gesture?.moved && gesture.type === "card" && !gesture.external) {
        const idx = row.cards.findIndex(c => c.id === gesture.id);
        if (idx >= 0 && !gesture.copy) {
          preview.removed = idx;
          preview.removed_advance = v.advance(idx);
        }
        if (gesture.drop?.row === row.id) {
          const g = v.measure(gesture.card);
          Object.assign(
              preview,
              {gap: gesture.drop.index, gap_width: g.width, gap_inset: g.inset, gap_advance: g.advance});
        }
      }
      const visible = [];
      for (let index = Math.max(0, v.first - 2); index < row.cards.length; index++) {
        const original = row.cards[index];
        const slot = preview.position(index, row.capacity || Infinity);
        if (slot === null)
          continue;
        const c = gesture?.type === "size" && gesture.id === original.id ? {...original, ...gesture.scale} :
                                                                           original;
        const g = v.measure(c), left = v.projectedLeft(slot, preview), x = left + g.width / 2;
        if (left > innerWidth + 32)
          break;
        if (left + g.width < -32)
          continue;
        const cy = curve.contact(x), angle = swing.angle(c.id);
        visible.push({c, g, x, cy, angle, slot});
        painter.drawHookBehind(ctx, x, cy, angle);
      }
      painter.drawRope(ctx, curve, 0);
      painter.drawPlaque(ctx, curve, 0, row.name, state().ropes.length);
      hit("rope-" + row.id, paperHits, "plaque-hit", t("ropeMenu", row.name), {rope: row.id, part: "rope"},
          px, y + py, RopePlaque.width, RopePlaque.height);
      if (preview.gap !== null)
        painter.drawDropGap(ctx, v.projectedLeft(preview.gap, preview) + preview.gap_width / 2, 0, curve);
      if (gesture?.moved && gesture.type === "rope" && overview && gesture.over === row.id) {
        ctx.fillStyle = "#996a32";
        ctx.fillRect(16, 4, innerWidth - 32, 3);
      }
      for (const {c, g, x, cy, angle, slot} of visible) {
        const isHover = hover?.id === c.id;
        painter.drawHangingCard(
            ctx, host.cardView(c), slot, row.capacity, g, x, cy, angle, isHover, isHover && hover.part === "close",
            devicePixelRatio);
        const left = x - g.body_width / 2, top = cy + y + g.top, h = g.bottom - g.top;
        boxes.set(
            c.id, {left, top: stage.getBoundingClientRect().top + top, w: g.body_width, h, row: row.id});
        const attrs = {card: c.id, rope: row.id};
        hit("card-" + c.id, paperHits, "body-hit", label(c), {...attrs, part: "body"}, left,
            cy + y + Hanging.bar_top, g.body_width, g.bottom - Hanging.bar_top);
        if (isHover || document.activeElement?.dataset.card === c.id) {
          const closeX = x + (g.adaptive ? g.width / 2 - 38 : g.width / 2 - Hanging.close_size / 2);
          const closeY = cy + y + (g.adaptive ? g.bar_bottom - 12 : Hanging.paper_top + 15);
          hit("close-" + c.id, paperHits, "control-hit", t("deleteCard"), {...attrs, part: "close"}, closeX - 10,
              closeY - 10, 20, 20);
          if (H.editable(c))
            hit("edit-" + c.id, paperHits, "control-hit", t("editText"), {...attrs, part: "edit"},
                x + g.hanger_width / 2 - 28, cy + y + 22, 24, 24);
        }
        if (selection?.id === c.id && find) {
          const {pos, value, length} = selection;
          const prefix = value.slice(Math.max(0, pos - 10), pos), match = value.slice(pos, pos + length);
          ctx.save();
          ctx.beginPath();
          ctx.rect(left + 3, cy + g.top + 3, g.body_width - 6, h - 6);
          ctx.clip();
          ctx.fillStyle = "#fff5de";
          ctx.fillRect(left + 5, cy + g.top + 10, g.body_width - 10, 25);
          ctx.font = '14px "Segoe UI", sans-serif';
          ctx.textBaseline = "middle";
          const start = left + 9, offset = Math.min(ctx.measureText(prefix).width, g.body_width / 3);
          ctx.fillStyle = "#e5a85e";
          ctx.fillRect(start + offset, cy + g.top + 13, ctx.measureText(match).width, 19);
          ctx.fillStyle = "#382b21";
          if (prefix)
            ctx.fillText(prefix, start, cy + g.top + 23, offset);
          ctx.fillText(value.slice(pos), start + offset, cy + g.top + 23);
          ctx.restore();
        }
      }
      ctx.restore();
    }
    for (const path of [...lines.children])
      if (!visibleLines.has(path))
        path.remove();
  }
  function floatRect(item) {
    const moving = gesture?.id === item.card.id ? gesture : null;
    const card = moving?.type === "size" ? {...item.card, ...moving.scale} : item.card;
    const size = floatingSizeForCard(card);
    const x = moving?.moved && moving.type === "card" ? moving.x - moving.gripX : moving?.resizeX ?? item.x;
    const y = moving?.moved && moving.type === "card" ? moving.y - moving.gripY : moving?.resizeY ?? item.y;
    return {
      card,
      left: clamp(x, 0, innerWidth - size.width),
      top: clamp(y, 0, innerHeight - 48 - size.height),
      w: size.width,
      h: size.height
    };
  }
  let frontWindow = 0;
  function drawFloats() {
    const ids = new Set();
    for (const item of state().floats) {
      const b = floatRect(item), c = b.card;
      ids.add(c.id);
      let node = floats.get(c.id);
      if (!node) {
        node = U.element("div", "floating-card-window");
        node.append(U.element("canvas", "floating-card-canvas"));
        node.style.zIndex = String(++frontWindow);
        node.addEventListener("pointerdown", () => { node.style.zIndex = String(++frontWindow); });
        layer.append(node);
        floats.set(c.id, node);
      }
      node.classList.toggle(
          "is-hanging-preview",
          !!(gesture?.type === "card" && gesture.id === c.id && gesture.moved && gesture.drop));
      Object.assign(
          node.style, {left: b.left + "px", top: b.top + "px", width: b.w + "px", height: b.h + "px"});
      const ctx = context(node.firstChild, b.w, b.h);
      painter.drawFloatingCard(
          ctx, host.cardView(c), b.w, b.h, hover?.id === c.id ? hover.part : "none", devicePixelRatio,
          editor?.id === c.id);
      boxes.set(c.id, {...b, floating: true});
      const attrs = {card: c.id, rope: ""};
      hit("float-" + c.id, node, "body-hit", label(c), {...attrs, part: "body"}, 0, 0, b.w, b.h);
      hit("return-" + c.id, node, "control-hit", t("returnCard"), {...attrs, part: "return"}, 8, 6,
          b.w >= 200 ? 76 : 34, 26);
      hit("fclose-" + c.id, node, "control-hit", t("recycleFloating"), {...attrs, part: "close"}, b.w - 32, 6, 26,
          26);
      if (H.editable(c))
        hit("fedit-" + c.id, node, "control-hit", t("editText"), {...attrs, part: "edit"}, b.w - 59, 6, 27,
            26);
    }
    for (const [key, node] of floats)
      if (!ids.has(key)) {
        node.remove();
        floats.delete(key);
      }
  }
  function draw() {
    for (const node of targets.values())
      delete node.dataset.used;
    boxes.clear();
    const h = layout();
    const ctx = context(canvas, innerWidth, h);
    stage.classList.toggle("is-minimized", !shown);
    stage.inert = !shown;
    tray.classList.toggle("is-open", shown);
    tray.setAttribute("aria-pressed", String(shown));
    if (shown)
      renderRows(ctx, h);
    drawFloats();
    for (const [k, node] of targets)
      if (!node.dataset.used) {
        node.remove();
        targets.delete(k);
      }
    positionEditor();
    overlay.hidden =
        !(gesture?.moved && gesture.type === "card" &&
          (!gesture.floating || gesture.drop || gesture.external));
    if (!overlay.hidden) {
      const dc = context(overlay, innerWidth, innerHeight),
            g = window.ClipRopePaint.measureCardLayout(gesture.card);
      painter.drawHangingCard(
          dc, host.cardView(gesture.card), gesture.drop?.index || 0,
          gesture.drop ? H.rope(state(), gesture.drop.row).capacity : null, g, gesture.x,
          gesture.y - g.top - 24, 0, false, false, devicePixelRatio);
    }
  }
  function rowAt(x, y) {
    if (!shown || x < 0 || x >= innerWidth || y >= innerHeight - 48)
      return;
    const local = y - stage.getBoundingClientRect().top;
    return rows.find(r => local >= r.y && local < r.y + r.height);
  }
  function dropAt(x, y) {
    const lane = rowAt(x, y);
    if (!lane)
      return null;
    const local = y - stage.getBoundingClientRect().top - lane.y;
    if (local < lane.curve.y(x) - 34 || local > lane.height - 18)
      return null;
    const p = new window.ClipRopePaint.InsertionPreview();
    const src = lane.row.cards.findIndex(c => c.id === gesture.id);
    if (src >= 0 && !gesture.copy) {
      p.removed = src;
      p.removed_advance = lane.v.advance(src);
    }
    return {row: lane.row.id, index: lane.v.insertion(x, p, lane.row.capacity || Infinity)};
  }
  function setHover(info, part) {
    if (hover?.id === info?.card && hover?.part === part)
      return;
    host.hover.cancel();
    hover = info?.card ? {id: info.card, part} : null;
    if (hover && part === "body") {
      const c = H.locate(state(), hover.id).card, b = boxes.get(c.id);
      if (b && c.kind === "image")
        host.hover.offer(
            "paper", c, {left: b.left, top: b.top, right: b.left + b.w, bottom: b.top + b.h},
            () => !gesture && hover?.id === c.id);
    }
    queueDraw();
  }
  function sizeEdge(b, x, y) {
    if (!b || x < b.left - 3 || x > b.left + b.w + 3 || y < b.top - 3 || y > b.top + b.h + 3)
      return "";
    const h = x - b.left <= 6 ? "w" : b.left + b.w - x <= 6 ? "e" : "";
    const v = b.top + b.h - y <= 6 ? "s" : b.floating && y - b.top <= 6 ? "n" : "";
    return v + h;
  }
  function blockGestures(target) {
    return target.closest(
        ".demo-menu,.demo-dialog,.paper-editor,.find-bar,#win11-snip-overlay,.demo-status");
  }
  function dataset(e) {
    return e.target.closest?.(".paper-hit,.rope-lines path")?.dataset || null;
  }
  function handleMove(e) {
    if (!gesture) {
      const d = dataset(e);
      const edge = d?.part === "body" && sizeEdge(boxes.get(d.card), e.clientX, e.clientY);
      if (e.target.closest(".paper-hit"))
        e.target.style.cursor = edge ? edge + "-resize" : "";
      const b = d?.card && boxes.get(d.card);
      const abovePaper = b && e.clientY < b.top + (b.floating ? 38 : 0);
      setHover(d, d?.part === "body" && abovePaper ? "hanger" : d?.part);
      return;
    }
    if (gesture.pointer !== e.pointerId)
      return;
    const g = gesture;
    g.x = e.clientX;
    g.y = e.clientY;
    const dx = g.x - g.startX, dy = g.y - g.startY;
    if (!g.moved && Math.hypot(dx, dy) < 4)
      return;
    if (!g.moved) {
      g.capture = g.floating ? layer : stage;
      g.capture.setPointerCapture(e.pointerId);
    }
    g.moved = true;
    targetAtPress = null;
    host.hover.cancel();
    if (g.type === "rope") {
      g.top = g.baseTop + dy;
      g.over = rowAt(g.x, g.y)?.row.id;
    } else if (g.type === "size") {
      const horizontal = g.edge.includes("w") ? -dx : dx, verticalDelta = g.edge.includes("n") ? -dy : dy;
      let sx = g.sx, sy = g.sy;
      if (/[we]/.test(g.edge))
        sx = clamp(g.sx * (1 + horizontal * (g.floating ? 1 : 2) / g.b.w), 0.75, 2);
      if (/[ns]/.test(g.edge))
        sy = clamp(g.sy * (1 + verticalDelta / g.b.h), 0.75, 2);
      if (g.card.kind === "image")
        sx = sy = /[we]/.test(g.edge) ? sx : sy;
      g.scale = {scaleX: sx, scaleY: sy};
      if (g.floating && g.edge.includes("w"))
        g.resizeX = g.b.left + g.b.w - g.b.w * sx / g.sx;
      if (g.floating && g.edge.includes("n"))
        g.resizeY = g.b.top + g.b.h - g.b.h * sy / g.sy;
    } else {
      g.drop = g.external ? null : dropAt(g.x, g.y);
      if (g.external)
        host.dragOver(g.x, g.y);
    }
    if (!edgeTimer)
      edgeTimer = setInterval(edgeScroll, 180);
    queueDraw();
  }
  function edgeScroll() {
    if (!gesture?.moved || gesture.external || gesture.type === "size")
      return;
    const g = gesture, lane = rowAt(g.x, g.y);
    if (overview && (g.y < 28 || g.y > innerHeight - 84))
      vertical += g.y < 28 ? -48 : 48;
    if (g.type === "card" && lane && (g.x < 36 || g.x > innerWidth - 36))
      lane.v.wheel(g.x < 36 ? -0.5 : 0.5);
    draw();
    if (g.type === "card")
      g.drop = dropAt(g.x, g.y);
    if (g.type === "rope")
      g.over = rowAt(g.x, g.y)?.row.id;
  }
  function begin(e) {
    if (e.button !== 0 || gesture || blockGestures(e.target))
      return;
    const d = dataset(e);
    if (!d)
      return;
    if (d.part === "body" &&
        (!targetAtPress || targetAtPress.card !== d.card || performance.now() - targetAtPress.time > 550)) {
      targetAtPress = {card: d.card, time: performance.now(), target: host.bindTarget()};
    }
    if (!["rope", "body"].includes(d.part))
      return;
    const x = e.clientX, y = e.clientY;
    const base = {pointer: e.pointerId, startX: x, startY: y, x, y, moved: false};
    if (d.part === "rope") {
      gesture = {...base, type: "rope", row: d.rope, baseTop: H.rope(state(), d.rope).topPx || 0};
    } else {
      const item = H.locate(state(), d.card), b = boxes.get(d.card);
      if (!b)
        return;
      const edge = sizeEdge(b, x, y);
      gesture = {
        ...base,
        type: edge ? "size" : "card",
        edge,
        card: item.card,
        id: d.card,
        row: d.rope,
        floating: !!item.floating,
        b,
        gripX: x - b.left,
        gripY: y - b.top,
        sx: item.card.scaleX || 1,
        sy: item.card.scaleY || 1,
        external: e.ctrlKey && !overview,
        copy: e.ctrlKey && overview
      };
    }
    host.hover.cancel();
  }
  function resetGesture() {
    clearInterval(edgeTimer);
    edgeTimer = 0;
    const previous = gesture;
    gesture = null;
    if (previous?.capture?.hasPointerCapture(previous.pointer))
      previous.capture.releasePointerCapture(previous.pointer);
    host.dragClear();
    draw();
  }
  async function end(e, cancelled) {
    const g = gesture;
    if (!g || e.pointerId !== g.pointer)
      return;
    if (g.type === "card" && g.moved && !g.external && !cancelled)
      g.drop = dropAt(e.clientX, e.clientY);
    if (cancelled || !g.moved) {
      resetGesture();
      return;
    }
    suppressClick = true;
    setTimeout(() => {suppressClick = false}, 0);
    resetGesture();
    if (g.type === "rope") {
      if (!overview)
        await command(s => {
          H.rope(s, g.row).topPx = clamp(g.top, 0, innerHeight - 48 - rowHeight(H.rope(s, g.row)));
        });
      else if (g.over && g.over !== g.row)
        await command(s => {
          const ordered = H.order(s), from = ordered.findIndex(r => r.id === g.row),
                to = ordered.findIndex(r => r.id === g.over);
          ordered.splice(to, 0, ...ordered.splice(from, 1));
          s.ropes = ordered.concat(s.ropes.filter(r => !ordered.includes(r)));
          H.select(s, ordered[0].id);
        });
    } else if (g.type === "size") {
      await command(s => {
        const item = H.locate(s, g.id);
        Object.assign(item.card, g.scale);
        if (item.floating) {
          item.floating.x = g.resizeX ?? item.floating.x;
          item.floating.y = g.resizeY ?? item.floating.y;
        }
      });
    } else if (g.external) {
      host.drop(host.cardView(g.card), e.clientX, e.clientY);
    } else if (g.drop) {
      await withSavedEditor(() => command(s => H.transfer(s, g.id, g.drop.row, g.drop.index, g.copy)));
    } else if (!g.copy) {
      await withSavedEditor(
          () => command(s => {
            const item = H.locate(s, g.id), size = floatingSizeForCard(item.card);
            const x = clamp(g.x - g.gripX * size.width / g.b.w, 0, innerWidth - size.width);
            const y = clamp(g.y - g.gripY * size.height / g.b.h, 0, innerHeight - 48 - size.height);
            H.remove(s, item);
            s.floats.push({card: item.card, x, y});
          }));
    }
  }
  let suppressClick = false;
  stage.addEventListener("focusin", queueDraw);
  window.addEventListener("pointermove", handleMove);
  window.addEventListener("pointerdown", begin);
  window.addEventListener("pointerup", e => end(e, false));
  window.addEventListener("pointercancel", e => end(e, true));
  window.addEventListener("lostpointercapture", e => {
    if (gesture?.pointer === e.pointerId && gesture.capture === e.target)
      resetGesture();
  });
  window.addEventListener("blur", () => {
    if (gesture)
      resetGesture();
  });
  document.addEventListener("keydown", e => {
    if (e.key === "Escape" && gesture)
      resetGesture();
  });
  window.addEventListener("wheel", e => {
    if (gesture || blockGestures(e.target) || e.target.closest("#floating-layer"))
      return;
    const lane = rowAt(e.clientX, e.clientY);
    if (!lane)
      return;
    e.preventDefault();
    if (editor && !boxes.get(editor.id)?.floating) {
      withSavedEditor(() => browse());
      return;
    }
    browse();
    function browse() {
      host.hover.cancel();
      const delta = e.deltaY || e.deltaX;
      if (e.ctrlKey && !overview) {
        cycle(lane.row.id, Math.sign(delta));
        return;
      }
      if (e.ctrlKey)
        vertical += delta;
      else {
        lane.v.wheel(delta / (e.deltaMode === 1 ? 3 : 100));
        for (const c of lane.row.cards)
          swing.kick(c.id, Math.sign(delta) * 14);
      }
      draw();
      queueDraw();
    }
  }, {passive: false});

  function show(value) {
    shown = value;
    host.hover.cancel();
    if (!shown)
      closeFind();
    else {
      for (const c of active().cards.slice(0, 12))
        swing.kick(c.id, 15);
      queueDraw();
    }
    draw();
  }
  async function toggle() {
    if (shown && editor && !H.locate(state(), editor.id).floating && !await saveEditor())
      return;
    show(!shown);
  }
  tray.addEventListener("click", toggle);
  async function capture(card, destination = state().active) {
    const saved = await command(s => H.insert(H.rope(s, destination), card), false);
    if (saved) {
      clipboard = publish(card);
      view(H.rope(state(), destination)).latest();
      swing.kick(card.id, 18);
      queueDraw();
      U.notice(t("collected", H.rope(state(), destination).name));
    }
    return saved;
  }
  async function activate(key, target) {
    await withSavedEditor(async () => {
      const item = H.locate(state(), key);
      if (item.floating) {
        await openCard(key);
        return;
      }
      try {
        clipboard = publish(item.card);
      } catch (err) {
        U.notice(err.message);
        return;
      }
      const published = clipboard, rowId = item.row.id;
      const ok = await command(s => {
        const source = H.locate(s, key);
        if (source.row?.id !== rowId)
          throw new Error(t("cardMoved"));
        H.transfer(s, key, rowId, 0);
      }, false);
      if (ok) {
        view(H.rope(state(), rowId)).latest();
        show(false);
      }
      if (clipboard === published && host.validTarget(target))
        await host.deliver(published, target);
    });
  }
  function positionEditor() {
    if (!editor)
      return;
    const b = boxes.get(editor.id);
    if (!b) {
      try {
        H.locate(state(), editor.id);
      } catch {
        discardEditor();
      }
      return;
    }
    const top = b.top + (b.floating ? 40 : 0);
    Object.assign(editor.panel.style, {
      left: clamp(b.left + 3, 0, innerWidth - 80) + "px",
      top: top + "px",
      width: Math.max(72, b.w - 6) + "px",
      height: Math.max(66, b.h - (b.floating ? 43 : 4)) + "px"
    });
  }
  function discardEditor() {
    editor?.panel.remove();
    editor = null;
    queueDraw();
  }
  async function saveEditor() {
    if (!editor)
      return true;
    if (busyEditor)
      return busyEditor;
    const draft = editor, value = draft.input.value;
    if (draft.original === value) {
      discardEditor();
      return true;
    }
    draft.input.readOnly = true;
    busyEditor = command(s => {
                   const {card} = H.locate(s, draft.id);
                   if (card.text !== draft.stored)
                     throw new Error(t("textChanged"));
                   Object.assign(card, H.asText(value), {id: draft.id, edited: true});
                   delete card.host;
                 })
                     .then(ok => {
                       if (ok && editor === draft)
                         discardEditor();
                       else
                         draft.input.readOnly = false;
                       return ok;
                     })
                     .finally(() => {
                       busyEditor = null;
                     });
    return busyEditor;
  }
  async function withSavedEditor(fn) {
    if (await saveEditor())
      return fn();
  }
  async function editText(key) {
    if (editor?.id === key) {
      editor.input.focus();
      return;
    }
    if (!await saveEditor())
      return;
    const {card} = H.locate(state(), key);
    swing.cards.delete(key);
    if (!H.editable(card))
      return;
    const panel = U.element("section", "paper-editor"), input = U.element("textarea");
    input.setAttribute("aria-label", t("cardText"));
    input.value = host.cardView(card).text;
    const foot = U.element("div", "paper-actions");
    foot.append(U.button(t("cancel"), discardEditor), U.button(t("save"), saveEditor, "demo-primary"));
    panel.append(input, foot);
    document.body.append(panel);
    editor = {id: key, original: input.value, stored: card.text, input, panel};
    draw();
    input.focus();
    input.setSelectionRange(input.value.length, input.value.length);
  }
  function closeFind() {
    find?.panel.remove();
    find = null;
    selection = null;
    queueDraw();
  }
  function updateMatches(reveal, step = 0) {
    if (!find)
      return;
    const hits = H.matches(state(), overview, find.input.value, host.cardView);
    let i = hits.findIndex(
        m => m.id === selection?.id && m.value === selection.value && m.pos === selection.pos);
    if (hits.length)
      i = i < 0 ? (step < 0 ? hits.length - 1 : 0) : (i + step + hits.length) % hits.length;
    selection = hits[i] || null;
    find.count.textContent = hits.length ? (i + 1) + " / " + hits.length :
        find.input.value                 ? t("noMatch") :
                                           t("enterKeywords");
    find.prev.disabled = find.next.disabled = !hits.length;
    if (reveal && selection) {
      const row = H.rope(state(), selection.row), v = view(row);
      v.first = row.cards.findIndex(c => c.id === selection.id);
      v.offset = 0;
      v.realize(true);
      if (overview) {
        const all = H.order(state());
        vertical = all.slice(0, all.findIndex(r => r.id === row.id)).reduce((n, r) => n + rowHeight(r), 0);
      }
    }
    queueDraw();
  }
  function openFind() {
    if (find) {
      find.input.focus();
      return;
    }
    const panel = U.element("section", "find-bar"), input = U.element("input"), count = U.element("output");
    panel.setAttribute("aria-label", t("findCards"));
    input.placeholder = t("findCards");
    input.setAttribute("aria-label", t("findCards"));
    const prev = U.button(t("previousMatch"), () => updateMatches(true, -1)),
          next = U.button(t("nextMatch"), () => updateMatches(true, 1));
    panel.append(input, count, prev, next, U.button(t("close"), closeFind));
    document.body.append(panel);
    find = {panel, input, count, prev, next};
    input.oninput = () => {
      selection = null;
      updateMatches(true);
    };
    updateMatches(false);
    input.focus();
  }
  function cycle(key, dir) {
    const all = H.order(state());
    const next = key === "rebin" ?
        state().ordinary :
        all[(all.findIndex(r => r.id === key) + dir + all.length) % all.length].id;
    return withSavedEditor(() => command(s => H.select(s, next)));
  }
  async function ropeProperties(key) {
    const original = key ? H.rope(state(), key) : null;
    const form = U.element("div", "property-fields");
    function field(label, type, value) {
      const wrapper = U.element("label", "", label), input = U.element("input");
      input.type = type;
      input.value = value;
      input.required = true;
      wrapper.append(input);
      form.append(wrapper);
      return input;
    }
    const name = field(t("ropeName"), "text", original?.name || "");
    name.maxLength = 64;
    name.readOnly = key === "main" || key === "rebin";
    const capacity = field(t("capacity"), "number", original?.capacity || 50);
    capacity.min = 1;
    capacity.step = 1;
    if (key === "rebin") {
      capacity.type = "text";
      capacity.value = t("unlimited");
      capacity.readOnly = true;
    }
    if (!await U.dialog(key ? t("editRope") : t("newRope"), form, t("save")))
      return;
    const limit = key === "rebin" ? null : Number(capacity.value), title = name.value.trim();
    if (!title || (limit !== null && (!Number.isSafeInteger(limit) || limit < 1))) {
      U.notice(t("invalidRope"));
      return;
    }
    const tail = original && limit ? original.cards.slice(limit) : [];
    if (tail.length &&
        !await U.dialog(
            t("reduceCapacity"), t("removeTail", tail.length, tail.map(label).join("\n")),
            t("removeAndSave")))
      return;
    const newKey = key || H.id();
    if (!await command(s => {
          if (key) {
            const row = H.rope(s, key);
            if ((row.capacity !== original.capacity) ||
                (limit && row.cards.slice(limit).map(c => c.id).join() !== tail.map(c => c.id).join()))
              throw new Error(t("ropeChanged"));
            row.name = title;
            row.capacity = limit;
            if (limit && row.cards.length > limit)
              row.cards.length = limit;
          } else
            s.ropes.push({id: newKey, name: title, capacity: limit, cards: []});
        }))
      return;
    if (!key && await U.dialog(t("switchCollector"), t("collectorPrompt", title), t("switch")))
      await command(s => H.select(s, newKey));
  }
  function ropeMenu(key, x, y) {
    const row = H.rope(state(), key);
    U.menu(x, y, row.name, [
      [t("findCards"), openFind],
      [
        overview ? t("singleRope") : t("overview"), () => withSavedEditor(async () => {
                                                    if (overview && !await command(s => H.select(s, key)))
                                                      return;
                                                    overview = !overview;
                                                    vertical = 0;
                                                    updateMatches(false);
                                                    draw();
                                                  })
      ],
      [t("previousRope"), () => cycle(key, -1)], [t("nextRope"), () => cycle(key, 1)],
      [
        t("firstCard"), row.cards.length && (() => {
                      view(row).latest();
                      draw();
                    })
      ],
      [
        row.compact ? t("regularView") : t("compactView"), () => command(s => {
                                                 const r = H.rope(s, key);
                                                 r.compact = !r.compact;
                                               })
      ],
      [
        state().rebinVisible ? t("hideRebin") : t("showRebin"),
        () => withSavedEditor(() => command(s => {
                                if (s.rebinVisible) {
                                  s.rebinVisible = false;
                                  if (s.active === "rebin")
                                    H.select(s, s.ordinary);
                                } else {
                                  const bin = H.rope(s, "rebin");
                                  s.ropes = s.ropes.filter(r => r !== bin);
                                  s.ropes.splice(s.ropes.indexOf(H.rope(s, s.active)), 0, bin);
                                  s.rebinVisible = true;
                                  H.select(s, "rebin");
                                }
                              }))
      ],
      [t("newRopeAction"), () => ropeProperties(null)], [t("editRopeAction"), () => ropeProperties(key)],
      [
        t("clearRopeAction"), row.cards.length && (async () => {
                       if (await U.dialog(
                               t("clearRopeTitle", row.name),
                               t("clearRopeBody", row.cards.length), t("clear")))
                         await command(s => {
                           H.rope(s, key).cards = [];
                         });
                     })
      ],
      [
        t("deleteRopeAction"), !["main", "rebin"].includes(key) && (async () => {
          if (!await U.dialog(t("deleteRopeTitle", row.name), t("deleteRopeBody"), t("delete")))
            return;
          await command(s => {
            s.ropes = s.ropes.filter(r => r.id !== key);
            if (s.ordinary === key)
              s.ordinary = "main";
            if (s.active === key)
              H.select(s, "main");
          });
        })
      ]
    ]);
  }
  function expandedPositions(existing, count) {
    const width = 240, height = 160, gap = 12;
    const right = Math.max(0, innerWidth - width);
    const bottom = Math.max(0, innerHeight - 48 - height);
    const occupied = existing.map(floatRect);
    const positions = [];
    for (let y = Math.min(gap,bottom); y <= bottom && positions.length < count; y += height + gap) {
      for (let x = Math.min(gap,right); x <= right && positions.length < count; x += width + gap) {
        const overlaps = occupied.some(b => x < b.left + b.w && x + width > b.left &&
                                            y < b.top + b.h && y + height > b.top);
        if (!overlaps) positions.push({x,y});
      }
    }
    for (let i = 1; positions.length < count; i++) {
      positions.push({x: (i * 24) % (right + 1), y: (i * 24) % (bottom + 1)});
    }
    return positions;
  }
  async function expand(key) {
    const {card, row, index} = H.locate(state(), key), members = card.members;
    if (!members || members.length < 2)
      return;
    if (row?.capacity && index + 1 + members.length > row.capacity) {
      await U.dialog(
          t("outOfRoom"), t("requiredCapacity", index + 1 + members.length), t("gotIt"),
          false);
      return;
    }
    const tail = row?.capacity ? row.cards.slice(Math.max(index + 1, row.capacity - members.length)) : [];
    if (tail.length &&
        !await U.dialog(
            t("expandGroup"), t("groupTail", row.name, tail.map(label).join("\n")), t("expand")))
      return;
    await command(s => {
      const current = H.locate(s, key);
      if (current.row?.id !== row?.id || current.index !== index ||
          (row && current.row.cards.map(c => c.id).join() !== row.cards.map(c => c.id).join()))
        throw new Error(t("groupMoved"));
      const positions = current.row ? [] : expandedPositions(s.floats, members.length);
      members.forEach((file, i) => {
        const child = H.asFiles([file]);
        if (current.row)
          H.insert(current.row, child, index + i + 1);
        else
          s.floats.push({card: child, ...positions[i]});
      });
    });
  }
  async function openCard(key) {
    const {card} = H.locate(state(), key);
    if (H.editable(card))
      return editText(key);
    return previewCard(card);
  }
  async function previewCard(source) {
    const card = host.cardView(source);
    if (card.kind === "folder" || card.members?.length > 1)
      return host.reveal(card);
    const body = U.element("div", "file-preview");
    const file = card.members?.[0];
    if (card.kind === "image" || file?.dataUrl?.startsWith("data:image/") || file?.sampleImage) {
      const img = U.element("img");
      img.alt = H.label(card);
      img.src = file?.dataUrl || host.imageURL(card);
      body.append(img);
    } else if (card.kind === "rich" && card.text) {
      body.append(U.element("p", "", card.text));
    } else {
      body.append(
          U.element("strong", "", card.ext || card.notice || t("file")),
          U.element("p", "", card.members?.[0]?.preview || card.detail || card.notice || H.label(card)));
      if (card.members?.length)
        body.append(U.element("small", "", card.members[0].parent + "\\" + card.members[0].name));
    }
    const title = card.kind === "image" ? t("imagePreview") : card.notice || H.label(card);
    return U.dialog(title, body, t("close"), false);
  }
  async function cardAction(key, part) {
    if (part === "edit")
      return editText(key);
    if (part === "return" || part === "close") {
      const destination = state().active;
      await withSavedEditor(() => command(s => {
                              const item = H.locate(s, key);
                              if (part === "return")
                                H.transfer(s, key, destination, 0);
                              else if (item.floating)
                                H.transfer(s, key, "rebin", 0);
                              else
                                H.remove(s, item);
                            }));
    }
  }
  function cardMenu(key, x, y) {
    const {card, row} = H.locate(state(), key), items = [];
    if (H.editable(card))
      items.push([t("editText"), () => editText(key)]);
    else if (card.kind !== "folder" && !(card.members?.length > 1))
      items.push([t("openPreview"), () => openCard(key)]);
    if (card.members?.length)
      items.push([t("showInExplorer"), () => host.reveal(card)]);
    if (card.members?.length > 1)
      items.push([t("expandGroup"), () => expand(key)]);
    items.push(
        [
          t("copyToDemo"),
          () => {
            clipboard = publish(card);
            U.notice(t("copiedToDemo"));
          }
        ],
        [t("copyToSystem"), () => host.exportCard(host.cardView(card))]);
    if (!row)
      items.push([t("returnCurrent"), () => cardAction(key, "return")]);
    for (const target of H.order(state())) {
      if (target.id !== row?.id)
        items.push([
          t("moveTo", target.name), () => withSavedEditor(() => command(s => H.transfer(s, key, target.id, 0)))
        ]);
      items.push([
        t("copyTo", target.name),
        () => withSavedEditor(() => command(s => H.transfer(s, key, target.id, 0, true)))
      ]);
    }
    items.push([row ? t("deleteCard") : t("moveRebin"), () => cardAction(key, "close")]);
    U.menu(x, y, label(card), items);
  }
  async function quickNote() {
    if (!await saveEditor())
      return;
    const card = H.asText("");
    if (await command(s => s.floats.push({card, x: (innerWidth - 240) / 2, y: (innerHeight - 208) / 2})))
      await editText(card.id);
  }
  tray.addEventListener("contextmenu", e => {
    e.preventDefault();
    const row = active();
    const items = row.cards.slice(0, 5).map(card => [label(card), () => {
      const source = H.locate(state(), card.id);
      if (source.row?.id !== row.id) throw new Error(t("cardMoved"));
      clipboard = publish(source.card);
      return command(s => H.transfer(s, card.id, row.id, 0), false);
    }]);
    items.push([t("quickNote"), quickNote], [t("importClipboard"), () => host.importCard(state().active)]);
    U.menu(e.clientX, e.clientY, t("recent", row.name), items);
  });
  window.addEventListener("contextmenu", e => {
    const d = dataset(e);
    if (!d)
      return;
    e.preventDefault();
    clearTimeout(menuTimer);
    if (d.card)
      cardMenu(d.card, e.clientX, e.clientY);
    else
      ropeMenu(d.rope, e.clientX, e.clientY);
  });
  window.addEventListener("click", e => {
    if (suppressClick)
      return;
    const d = dataset(e);
    if (!d)
      return;
    if (d.part === "rope") {
      clearTimeout(menuTimer);
      if (e.detail < 2)
        menuTimer = setTimeout(() => ropeMenu(d.rope, e.clientX, e.clientY), overview ? 320 : 0);
    } else if (d.part !== "body")
      cardAction(d.card, d.part);
    else if (e.detail === 0)
      activate(d.card, host.bindTarget());
  });
  window.addEventListener("dblclick", e => {
    const d = dataset(e);
    if (!d)
      return;
    if (d.part === "rope") {
      clearTimeout(menuTimer);
      U.closeMenu();
      if (overview)
        withSavedEditor(async () => {
          if (await command(s => H.select(s, d.rope))) {
            overview = false;
            vertical = 0;
            draw();
          }
        });
    } else if (d.part === "body") {
      const target = targetAtPress?.target;
      targetAtPress = null;
      activate(d.card, target);
    }
  });
  window.addEventListener("resize", draw);
  return {
    draw,
    refreshLanguage() {
      U.closeMenu();
      scroll.setAttribute("aria-label", t("scrollRopes"));
      if (editor) {
        const {card} = H.locate(state(), editor.id);
        if (editor.input.value === editor.original) {
          editor.input.value = host.cardView(card).text;
          editor.original = editor.input.value;
        }
        editor.input.setAttribute("aria-label", t("cardText"));
        const buttons = editor.panel.querySelectorAll("button");
        buttons[0].textContent = t("cancel");
        buttons[1].textContent = t("save");
      }
      if (find) {
        find.panel.setAttribute("aria-label", t("findCards"));
        find.input.setAttribute("aria-label", t("findCards"));
        find.input.placeholder = t("findCards");
        find.prev.textContent = t("previousMatch");
        find.next.textContent = t("nextMatch");
        find.panel.lastElementChild.textContent = t("close");
        updateMatches(false);
      }
      draw();
    },
    command,
    capture,
    quickNote,
    active,
    show,
    openCard,
    previewCard,
    openFind,
    get shown() {
      return shown;
    },
    get clipboard() {
      return clipboard;
    },
    imageReady: queueDraw,
    previewHover(card) {
      const b = boxes.get(card.id);
      if (b)
        host.hover.offer(
            "demo", card, {left: b.left, top: b.top, right: b.left + b.w, bottom: b.top + b.h}, () => true,
            true);
    },
    previewDrag(card, x, y) {
      gesture = {type: "card", card, id: card.id, moved: true, external: true, x, y};
      draw();
    },
    get floatingFrames() {
      return [...floats.values()].map(n => ({canvas: n.firstChild, rect: n.getBoundingClientRect()}));
    }
  };
};
