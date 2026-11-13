/* The durable demo model contains only structured-cloneable browser data. */
(() => {
  "use strict";
  const {t} = window.ClipRopeI18n;
  function id() {
    if (crypto.randomUUID)
      return crypto.randomUUID();
    return Array
        .from(crypto.getRandomValues(new Uint8Array(16)), byte => byte.toString(16).padStart(2, "0"))
        .join("");
  }
  const rope = (state, key) => state.ropes.find(row => row.id === key);
  const label = card => card.text || card.notice || (card.kind === "image" ? t("screenshot") : t("card"));
  const editable = card => card && (card.kind === "text" || card.kind === "link");
  function locate(state, key) {
    for (const row of state.ropes) {
      const index = row.cards.findIndex(card => card.id === key);
      if (index >= 0)
        return {card: row.cards[index], row, index};
    }
    const index = state.floats.findIndex(item => item.card.id === key);
    if (index >= 0)
      return {card: state.floats[index].card, floating: state.floats[index], index};
    throw new Error(t("cardMissing"));
  }
  function remove(state, item) {
    (item.row ? item.row.cards : state.floats).splice(item.index, 1);
  }
  function insert(row, card, index = 0) {
    if (!row)
      throw new Error(t("ropeMissing"));
    const last = row.capacity ? row.capacity - 1 : row.cards.length;
    row.cards.splice(Math.max(0, Math.min(index, last)), 0, card);
    if (row.capacity && row.cards.length > row.capacity)
      row.cards.length = row.capacity;
  }
  function transfer(state, key, destination, index, copy = false) {
    const item = locate(state, key);
    const card = copy ? {...structuredClone(item.card), id: id(), sampleCard: item.card.sampleCard || item.card.id} : item.card;
    if (!copy)
      remove(state, item);
    insert(rope(state, destination), card, index);
  }
  function select(state, key) {
    if (!rope(state, key))
      throw new Error(t("ropeMissing"));
    state.active = key;
    if (key !== "rebin")
      state.ordinary = key;
  }
  function order(state) {
    const rows = state.ropes.filter(row => row.id !== "rebin" || state.rebinVisible);
    const start = rows.findIndex(row => row.id === state.active);
    return rows.slice(start).concat(rows.slice(0, start));
  }
  function head(state) {
    return rope(state, state.active)?.cards[0];
  }
  function publish(card) {
    if (!card || card.kind === "unavailable" || card.status === "damaged") {
      throw new Error(t("nothingToCopy"));
    }
    return structuredClone(card);
  }
  function asText(text) {
    return {
      id: id(),
      kind: /^https?:\/\/\S+$/i.test(text) ? "link" : "text",
      text,
      status: "complete",
      capabilities: "Text"
    };
  }
  function asFiles(files) {
    const first = files[0];
    return {
      id: id(),
      kind: files.length === 1 && first.ext === "DIR" ? "folder" : "files",
      text: first.name,
      ext: first.ext,
      location: first.parent,
      detail: files.length > 1 ? t("items", files.length) : first.type,
      members: structuredClone(files),
      status: "complete",
      capabilities: "Files"
    };
  }
  function matches(state, overview, query, view = card => card) {
    const result = [];
    if (!query)
      return result;
    const needle = query.replace(/[A-Z]/g, c => c.toLowerCase());
    for (const row of overview ? order(state) : [rope(state, state.active)]) {
      for (const original of row.cards) {
        const card = view(original);
        const fields = [card.text, ...(card.members || []).map(f => f.parent + "\\" + f.name)];
        for (const value of new Set(fields.filter(Boolean))) {
          const hay = value.replace(/[A-Z]/g, c => c.toLowerCase());
          let pos = hay.indexOf(needle);
          while (pos >= 0) {
            result.push({row: row.id, id: card.id, value, pos, length: query.length});
            pos = hay.indexOf(needle, pos + needle.length);
          }
        }
      }
    }
    return result;
  }
  function openDB() {
    return new Promise((resolve, reject) => {
      const req = indexedDB.open("ClipRope-Web-History", 1);
      req.onupgradeneeded = () => req.result.createObjectStore("history");
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
      req.onblocked = () => reject(new Error(t("closeOtherTabs")));
    });
  }
  function access(db, next, revision) {
    return new Promise((resolve, reject) => {
      const tx = db.transaction("history", next ? "readwrite" : "readonly");
      const store = tx.objectStore("history");
      const request = store.get("desktop");
      let conflict = false;
      request.onsuccess = () => {
        if (!next)
          return;
        if ((request.result?.revision || 0) !== revision) {
          conflict = true;
          tx.abort();
        } else
          store.put(next, "desktop");
      };
      tx.oncomplete = () => resolve(request.result);
      tx.onabort = () =>
          reject(new Error(conflict ? t("historyConflict") : t("saveFailed")));
      tx.onerror = () => {};
    });
  }
  class History {
    constructor(seed, temporary) {
      this.state = seed;
      this.temporary = temporary;
      this.queue = Promise.resolve();
      this.ready = this.open();
    }
    async open() {
      if (this.temporary)
        return;
      this.db = await openDB();
      this.db.onversionchange = () => this.db.close();
      const saved = await access(this.db);
      if (saved) {
        this.state = saved;
        this.state.rebinVisible = false;
        select(this.state, this.state.ordinary);
      }
    }
    change(edit) {
      const run = async () => {
        await this.ready;
        const before = this.state;
        const next = structuredClone(before);
        edit(next);
        next.revision++;
        if (!this.temporary)
          await access(this.db, next, before.revision);
        this.state = next;
        return {before, next};
      };
      const result = this.queue.then(run);
      this.queue = result.catch(() => {});
      return result;
    }
  }
  window.ClipRopeHistory = {
    History,
    id,
    rope,
    label,
    editable,
    locate,
    remove,
    insert,
    transfer,
    select,
    order,
    head,
    publish,
    asText,
    asFiles,
    matches
  };
})();
