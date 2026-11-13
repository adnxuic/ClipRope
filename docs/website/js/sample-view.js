/* Project only the supplied fixtures into the UI language. Captured and edited text is literal. */
window.createClipRopeSampleView = function(catalogs) {
  "use strict";
  const {t} = window.ClipRopeI18n;
  function index(catalog) {
    const cards = new Map(), folders = new Map(), files = new Map(), sessions = new Map();
    for (const row of catalog.ropes)
      for (const card of row.cards) cards.set(card.id, card);
    for (const folder of catalog.folders) {
      folders.set(folder.id, folder);
      for (const file of folder.files) files.set(file.sampleFile, file);
    }
    for (const session of catalog.sessions) sessions.set(session.id, session);
    return {cards, folders, files, sessions};
  }
  const zh = index(catalogs["zh-CN"]), en = index(catalogs.en);
  const current = () => window.ClipRopeI18n.language === "en" ? en : zh;
  const oldFiles = new Map(), paths = new Map();
  for (const folder of catalogs["zh-CN"].folders) {
    paths.set(folder.path, folder.id);
    paths.set(en.folders.get(folder.id).path, folder.id);
    for (const file of folder.files) oldFiles.set(folder.id + "\n" + file.name, file.sampleFile);
  }
  function folderName(folder) {
    return zh.folders.get(folder.id)?.name === folder.name ? current().folders.get(folder.id).name : folder.name;
  }
  function folderPath(path) {
    return current().folders.get(paths.get(path))?.path || path;
  }
  function fileView(file, owner = file.folderId) {
    // Resolve pre-key samples only within their original folder.
    const key = file.sampleFile || oldFiles.get(owner + "\n" + file.name);
    const original = zh.files.get(key), other = en.files.get(key), target = current().files.get(key);
    const result = {...file, parent: folderPath(file.parent)};
    if (file.ext === "PNG" && (file.type === "PNG 图像" || file.type === "PNG image"))
      result.type = t("pngType");
    if (target && (file.name === original.name || file.name === other.name)) {
      for (const field of ["name", "type", "preview"]) {
        if (file[field] === original[field] || file[field] === other[field]) result[field] = target[field];
      }
    }
    return result;
  }
  function cardView(card) {
    const result = {...card};
    const key = card.sampleCard || card.id, original = zh.cards.get(key);
    if (original && !card.edited && card.kind === original.kind) {
      for (const field of ["text", "notice"])
        if (original[field] !== undefined && card[field] === original[field])
          result[field] = current().cards.get(key)[field];
    }
    if (card.members?.length) {
      result.members = card.members.map(file => fileView(file));
      const first = result.members[0];
      result.text = first.name;
      result.detail = result.members.length > 1 ? t("items", result.members.length) : first.type;
      result.location = first.parent;
    }
    return result;
  }
  function sessionView(session) {
    const translated = current().sessions.get(session.id);
    const authored = new Map(translated.messages.map(m => [m.id, m]));
    const messages = session.messages.map(m => {
      if (authored.has(m.id)) return authored.get(m.id);
      const result = {...m, author: t("me"), avatarText: t("me")};
      if (m.member) {
        result.member = fileView(m.member);
        result.fileName = result.member.name;
        result.meta = result.member.type;
      }
      return result;
    });
    return {...session, title: translated.title, avatarText: translated.avatarText, messages};
  }
  return {cardView, fileView, folderName, folderPath, sessionView};
};
