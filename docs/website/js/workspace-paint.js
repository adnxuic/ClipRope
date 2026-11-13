// ClipRope Workspace Renderer & Presentation Port
// Ported from:
// - src/rendering/workspace_paint.cpp
// - src/presentation/hanging_layout.hpp & hanging_layout.cpp
// - src/presentation/image_card_layout.hpp
// - src/presentation/viewport.hpp & viewport.cpp
// - src/presentation/swing.hpp & swing.cpp
// - src/presentation/card_content.hpp
(function (global) {
  "use strict";
  const {t} = window.ClipRopeI18n;

  const PI = Math.PI;
  const f32 = Math.fround;

  // Vertical geometry of one hanging card in DIPs (src/presentation/hanging_layout.hpp)
  const Hanging = Object.freeze({
    row: 290,
    rope_end: 60,
    rope_thickness: 9,
    max_sag: 24,
    hook_top: -9,
    pivot: -4.5, // -rope_thickness / 2
    bar_top: 18,
    bar_end: 25,
    bar_bottom: 42,
    bar_overhang: 4,
    paper_top: 42,
    paper_bottom: 190,
    paper_tuck: 4,
    close_size: 26,
    swing_limit: 4.5,
  });

  const TextInset = Object.freeze({
    side: 12,
    hanging_top: 11,
    floating_top: 44,
    hanging_bottom: 12,
    floating_bottom: 10,
  });

  const RopePlaque = Object.freeze({
    width: 144,
    height: 48,
    radius: 5,
    gap: 10,
  });

  // Skin parameters (0xRRGGBB from src/rendering/workspace_paint.cpp)
  const skin = Object.freeze({
    label: 0x5e5144,
    icon: 0x6c5a4c,
    muted: 0x8a7b6a,
    faint: 0xa89a88,
    steel_edge: 0x4c5157,
    steel: 0x9ea4ab,
    steel_light: 0xe4e8ec,
    steel_dark: 0x5e646b,
    wood_edge: 0x8a5a2b,
    wood_grain: 0x9c6a38,
    wood_light: 0xf7ddb0,
    wood_bottom: 0x8f5e2e,
    engraving: 0xfff1d6,
    warm_paper: 0xfbf7ef,
    paper_edge: 0x5a4630,
    drop: 0x2c7972,
    partial: 0xe0a030,
    damaged: 0xc8473b,
    link: 0x3a6ea5,
    warning: 0xb5533c,
    hemp: 0xc9a26b,
  });

  const ropeDefaultTones = Object.freeze([
    0x6f5230, // deep
    0x9a7447, // dark
    0xc9a26b, // mid
    0xddbe8a, // light
    0xf0ddb4, // high
  ]);

  function splitRgb(c) {
    return {
      r: ((c >>> 16) & 255) / 255,
      g: ((c >>> 8) & 255) / 255,
      b: (c & 255) / 255,
    };
  }

  function mixColor(a, b, t) {
    const x = splitRgb(a);
    const y = splitRgb(b);
    const q = (u, v) => Math.round(Math.min(1, Math.max(0, u + (v - u) * t)) * 255);
    return (q(x.r, y.r) << 16) | (q(x.g, y.g) << 8) | q(x.b, y.b);
  }

  function rgbaCss(rgb, alpha = 1) {
    const r = (rgb >>> 16) & 255;
    const g = (rgb >>> 8) & 255;
    const b = rgb & 255;
    if (alpha >= 0.999) {
      return `rgb(${r}, ${g}, ${b})`;
    }
    return `rgba(${r}, ${g}, ${b}, ${alpha})`;
  }

  // FNV-1a hash matching seed_of(std::string_view) in workspace_paint.cpp and mix(const Id&) in swing.cpp
  function seedOf(id) {
    const s = String(id || "");
    let hash = 2166136261 >>> 0;
    for (let i = 0; i < s.length; ++i) {
      hash ^= s.charCodeAt(i) & 0xff;
      hash = Math.imul(hash, 16777619) >>> 0;
    }
    return hash >>> 0;
  }

  function cardSeed(card) {
    return typeof card.seed === "number" ? card.seed >>> 0 : seedOf(card.id);
  }

  // Deterministic PRNG matching struct Noise in workspace_paint.cpp
  class Noise {
    constructor(seed) {
      this.state = seed >>> 0;
    }
    next() {
      this.state = (Math.imul(this.state, 1664525) + 1013904223) >>> 0;
      return f32((this.state >>> 8) / 16777216.0);
    }
    range(a, b) {
      return f32(a + (b - a) * this.next());
    }
  }

  // Catenary curve matching RopeCurve in src/presentation/hanging_layout.cpp
  class RopeCurve {
    constructor(width = 1536) {
      this.width = Math.max(1, width);
      this.sag = Math.min(Hanging.max_sag, this.width / 64);
      this.a = 0;
      if (this.sag >= 0.01) {
        const half = this.width / 2.0;
        const sag = this.sag;
        let a = (half * half) / (2 * sag);
        for (let i = 0; i < 32; ++i) {
          const u = half / a;
          const f = a * (Math.cosh(u) - 1) - sag;
          const slope = Math.cosh(u) - 1 - u * Math.sinh(u);
          if (slope === 0) break;
          const next = a - f / slope;
          if (!(next > 0) || !Number.isFinite(next)) break;
          const done = Math.abs(next - a) < 1e-9 * a;
          a = next;
          if (done) break;
        }
        this.a = a;
      }
    }
    lowest() {
      return Hanging.rope_end + this.sag;
    }
    y(x) {
      if (!this.a) return Hanging.rope_end + this.sag;
      const half = this.width / 2.0;
      const t = Math.min(this.width, Math.max(0, x));
      return Hanging.rope_end + this.a * (Math.cosh(half / this.a) - Math.cosh((t - half) / this.a));
    }
    contact(hookX) {
      return Math.round(this.y(hookX));
    }
  }

  // Hook geometry constants (lines 83-102 of workspace_paint.cpp)
  const WIRE = 2.8;
  const LOOP_RADIUS = 7.4;
  const LOOP_X = -0.6;
  const LOOP_Y = Hanging.pivot + LOOP_RADIUS - WIRE / 2; // 1.5
  const NECK_TOP = 11.4;
  const CROSSING_DEG = 180 - (Math.asin((LOOP_Y - Hanging.pivot) / LOOP_RADIUS) * 180) / PI;
  const SHOULDER = Hanging.bar_end - 0.8; // 24.2
  const END_RADIUS = 5;

  function loopPoint(degrees) {
    const a = (degrees * PI) / 180;
    return {
      x: LOOP_X + LOOP_RADIUS * Math.cos(a),
      y: LOOP_Y - LOOP_RADIUS * Math.sin(a),
    };
  }

  function barTopAt(x, width) {
    const half = width / 2;
    const ax = Math.abs(x);
    const hump = 0.21 * width;
    const corner = half - END_RADIUS;
    if (ax <= hump) {
      return Hanging.bar_top + ((SHOULDER - Hanging.bar_top) * (1 - Math.cos((PI * ax) / hump))) / 2;
    }
    if (ax <= corner) {
      return SHOULDER + (0.8 * (ax - hump)) / Math.max(1, corner - hump);
    }
    const d = Math.min(END_RADIUS, ax - corner);
    return Hanging.bar_end + END_RADIUS - Math.sqrt(END_RADIUS * END_RADIUS - d * d);
  }

  function buildGrainLines(width, base, noise, count, spread, topAt) {
    const h = width / 2;
    const polylines = [];
    for (let i = 0; i < count; ++i) {
      const t = (i + noise.range(0.25, 0.75)) / count;
      const span = width * noise.range(0.35, 0.95);
      const start = noise.range(-h + 3, Math.max(-h + 3, h - 3 - span));
      const f = noise.range(0.03, 0.08);
      const phase = noise.range(0, 2 * PI);
      const points = [];
      const limit = Math.min(h - 3, start + span);
      for (let x = start; x <= limit + 1e-4; x += 3) {
        const top = topAt(x) + 2.2;
        const bottom = base - 1.8;
        const rawY = top + (bottom - top) * t + spread * Math.sin(x * f + phase);
        const y = Math.min(bottom, Math.max(top, rawY));
        points.push({ x, y });
      }
      if (points.length >= 2) {
        polylines.push(points);
      }
    }
    return polylines;
  }

  function buildBarCache(width, base) {
    const h = width / 2;
    const hump = 0.21 * width;
    const highlight = [];
    for (let x = -h + 4; x <= h - 4; x += 2) {
      highlight.push({ x, y: barTopAt(x, width) + 1.1 });
    }
    const grain = [];
    const streaks = [];
    const topAt = (x) => barTopAt(x, width);
    for (let variant = 0; variant < 4; ++variant) {
      const noise = new Noise(Math.imul(0x9e3779b9, variant + 1) >>> 0);
      grain.push(buildGrainLines(width, base, noise, 6, 0.45, topAt));
      streaks.push(buildGrainLines(width, base, noise, 2, 0.3, topAt));
    }
    return { width, base, h, hump, highlight, grain, streaks };
  }

  // Pendulum physics matching src/presentation/swing.cpp
  // Finite pendulum feedback for hanging cards, in degrees with clockwise positive.
  class Swing {
    constructor() {
      this.cards = new Map();
      this.pinned = "";
      this.startAngle = 0;
      this.settleTime = 0;
    }
    kick(id, degreesPerSecond) {
      if (id === this.pinned || !Number.isFinite(degreesPerSecond)) return;
      let m = this.cards.get(id);
      if (!m) {
        // Neighbors differ slightly so a scrolled rope does not sway in lockstep.
        const h = seedOf(id);
        const unit = (bits) => (bits & 0xff) / 255;
        m = {
          angle: 0,
          velocity: 0,
          omega: 7.5 + unit(h),
          damping: 0.25 + 0.05 * unit(h >>> 8),
          gain: 0.85 + 0.3 * unit(h >>> 16),
        };
        this.cards.set(id, m);
      }
      const fastest = Hanging.swing_limit * m.omega;
      m.velocity = Math.min(fastest, Math.max(-fastest, m.velocity + degreesPerSecond * m.gain));
    }
    release(id, degrees, degreesPerSecond) {
      if (id === this.pinned || !Number.isFinite(degrees) || !Number.isFinite(degreesPerSecond)) return;
      this.cards.delete(id);
      this.kick(id, degreesPerSecond);
      const m = this.cards.get(id);
      if (m) {
        m.angle = Math.min(Hanging.swing_limit, Math.max(-Hanging.swing_limit, degrees));
      }
    }
    pin(id) {
      this.unpin();
      this.pinned = id;
      this.startAngle = this.angle(id);
      this.settleTime = 0;
      if (this.startAngle === 0) {
        this.cards.delete(id);
      }
    }
    unpin() {
      if (this.pinned) {
        this.cards.delete(this.pinned);
      }
      this.pinned = "";
    }
    settling() {
      return Boolean(this.pinned) && this.cards.has(this.pinned);
    }
    advance(seconds) {
      if (!Number.isFinite(seconds)) return this.moving();
      this.settleTime += this.settling() ? Math.max(0, seconds) : 0;
      seconds = Math.min(0.1, Math.max(0, seconds));
      const step = 0.004;
      for (const [id, m] of this.cards.entries()) {
        if (id === this.pinned) {
          const t = Math.min(1, this.settleTime / 0.18);
          m.angle = this.startAngle * (1 - t * t * (3 - 2 * t));
          if (t === 1) this.cards.delete(id);
          continue;
        }
        for (let left = seconds; left > 1e-9; left -= step) {
          const dt = Math.min(left, step);
          m.velocity -= (m.omega * m.omega * m.angle + 2 * m.damping * m.omega * m.velocity) * dt;
          m.angle += m.velocity * dt;
          if (Math.abs(m.angle) > Hanging.swing_limit) {
            m.angle = Math.sign(m.angle) * Hanging.swing_limit;
            m.velocity *= -0.3;
          }
        }
        // Below this the paper's lower edge moves less than a quarter DIP.
        if (Math.abs(m.angle) < 0.06 && Math.abs(m.velocity) < 0.6) {
          this.cards.delete(id);
        }
      }
      return this.moving();
    }
    angle(id) {
      const m = this.cards.get(id);
      return m ? m.angle : 0;
    }
    moving() {
      return this.cards.size > 0;
    }
    clear() {
      this.cards.clear();
    }
  }

  // InsertionPreview & Viewport matching src/presentation/viewport.hpp & image_card_layout.hpp
  class InsertionPreview {
    constructor() {
      this.removed = null; // number | null
      this.gap = null; // number | null
      this.gap_width = 0;
      this.gap_advance = 0;
      this.gap_inset = 0;
      this.removed_advance = 0;
    }
    position(index, capacity) {
      if (this.removed === index) return null;
      let pos = index;
      if (this.removed !== null && pos > this.removed) --pos;
      if (this.gap !== null && pos >= this.gap) ++pos;
      return this.gap !== null && pos >= capacity ? null : pos;
    }
  }

  function isAdaptiveImage(card) {
    if (!card) return false;
    return Boolean(card.image_primary || card.kind === "image" || card.file_preview === "picture");
  }

  // Matches ImageCardLayout::measure in src/presentation/image_card_layout.hpp (lines 64-85)
  function measureCardLayout(card, paper = 172, compact = false) {
    const spacing = compact ? 12 : 24;
    const shadowMargin = 2;
    if (!isAdaptiveImage(card)) {
      const w = (card?.width || paper) * (card?.scaleX || 1);
      const hangerWidth = w + 2 * Hanging.bar_overhang; // 180
      const inset = Math.max(0, (hangerWidth - w) / 2) + shadowMargin; // 6
      const advance = hangerWidth + 2 * shadowMargin + spacing; // 208
      return {
        width: w,
        body_width: w,
        top: Hanging.paper_top,
        bottom: Hanging.paper_top + (Hanging.paper_bottom - Hanging.paper_top) * (card?.scaleY || 1),
        bar_bottom: Hanging.bar_bottom,
        adaptive: false,
        spacing,
        hanger_width: hangerWidth,
        inset,
        advance,
        carried_padding: Math.max(32, Math.ceil(Hanging.paper_bottom * 0.08 + 12)),
      };
    }

    // Adaptive image card layout (ImageSizeMode::normal limit = {192, 144})
    const limitW = 192;
    const limitH = 144;
    const srcW = card.source && card.source.width ? card.source.width : 1920;
    const srcH = card.source && card.source.height ? card.source.height : 1080;
    const scale = Math.min(limitW / srcW, limitH / srcH, 1.5);
    const bodyW = srcW * scale * (card.scaleX || 1);
    const bodyH = srcH * scale * (card.scaleY || 1);
    const density = compact ? 0.5 : 1.0;
    const barEnd = Hanging.bar_top + 30; // 48
    const top = barEnd - 2; // 46
    const bottom = top + bodyH * density;
    const width = Math.max(160, bodyW + 12) * density;
    const hangerWidth = width; // adaptive ? width : width + 2 * bar_overhang
    const inset = shadowMargin; // (hanger_width - width) / 2 + shadow_margin = 2
    const advance = hangerWidth + 2 * shadowMargin + spacing;
    return {
      width,
      body_width: bodyW * density,
      top,
      bottom,
      bar_bottom: barEnd,
      adaptive: true,
      spacing,
      hanger_width: hangerWidth,
      inset,
      advance,
      carried_padding: Math.max(32, Math.ceil(bottom * 0.08 + 12)),
    };
  }

  class Viewport {
    static get margin() {
      return 14;
    }
    static get paper_width() {
      return 172;
    }
    constructor(cards = [], width = 1536) {
      this.allCards = cards;
      this.width = width;
      this.first = 0;
      this.offset = 0;
      this.compact = false;
      this.realize(true);
    }
    setCards(cards, preservePosition = true) {
      this.allCards = cards;
      this.realize(preservePosition);
    }
    refresh(width) {
      this.width = Math.max(1, width);
      this.realize(true);
    }
    paper() {
      return this.compact ? 86 : Viewport.paper_width;
    }
    measure(card) {
      return measureCardLayout(card, this.paper(), this.compact);
    }
    step() {
      return this.measure(null).advance; // 208
    }
    geometry(index) {
      const card = this.allCards[index];
      return this.measure(card);
    }
    advance(index) {
      return this.geometry(index).advance;
    }
    realize(preserveAnchor = false) {
      const count = this.allCards.length;
      if (!count) {
        this.first = 0;
        this.offset = 0;
        return;
      }
      this.first = Math.min(this.first, count - 1);
      const stride0 = this.advance(this.first);
      this.offset = Math.min(stride0 - 0.001, Math.max(preserveAnchor && this.first === 0 ? -this.width : 0, this.offset));
      while (this.offset < 0 && this.first > 0) {
        this.offset += this.advance(--this.first);
      }
      if (!preserveAnchor) {
        this.offset = Math.max(0, this.offset);
      }
      while (this.first + 1 < count && this.offset >= this.advance(this.first)) {
        this.offset -= this.advance(this.first++);
      }

      const available = Math.max(1, this.width - 2 * Viewport.margin);
      let edge = -this.offset;
      let end = this.first;
      while (end < count && edge < available) {
        edge += this.advance(end++);
      }
      // Clamp the trailing edge, backing into the preceding page only as needed.
      if (end === count && !preserveAnchor) {
        edge -= this.geometry(end - 1).spacing;
        if (edge < available) {
          this.offset -= available - edge;
          while (this.offset < 0 && this.first > 0) {
            this.offset += this.advance(--this.first);
          }
          this.offset = Math.max(0, this.offset);
        }
      }
    }
    wheel(turns) {
      const count = this.allCards.length;
      if (!Number.isFinite(turns) || !count) return;
      const current = this.first + this.offset / this.advance(this.first);
      const target = Math.min(count, Math.max(0, current + turns));
      this.first = Math.min(Math.floor(target), count - 1);
      const phase = target - this.first;
      this.offset = phase * this.advance(this.first);
      this.realize(false);
    }
    latest() {
      this.first = 0;
      this.offset = 0;
      this.realize(false);
    }
    origin(index) {
      let x = Viewport.margin - this.offset;
      if (index >= this.first) {
        for (let i = this.first; i < index; ++i) {
          x += this.advance(i);
        }
      } else {
        for (let i = this.first - 1; i >= index; --i) {
          x -= this.advance(i);
        }
      }
      return x;
    }
    left(index) {
      return this.origin(index) + this.geometry(index).inset;
    }
    projectedLeft(pos, preview) {
      const p = preview || new InsertionPreview();
      const afterGap = p.gap !== null && pos > p.gap;
      let original = pos - (afterGap ? 1 : 0);
      const afterSource = p.removed !== null && original >= p.removed;
      if (afterSource) ++original;
      const inset = p.gap === pos && p.gap_width > 0 ? p.gap_inset : this.geometry(original).inset;
      const removed = afterSource ? (p.removed_advance > 0 ? p.removed_advance : this.advance(p.removed)) : 0;
      return (
        this.origin(original) +
        inset -
        removed +
        (afterGap ? (p.gap_advance > 0 ? p.gap_advance : this.step()) : 0)
      );
    }
    insertion(clientX, preview, capacity) {
      const p = preview || new InsertionPreview();
      const total = this.allCards.length;
      const count = total - (p.removed !== null && p.removed < total ? 1 : 0);
      const limit = Math.min(count, capacity ? capacity - 1 : 0);
      let last = 0;
      for (let i = 0; i < total; ++i) {
        if (p.removed === i) continue;
        const pos = i - (p.removed !== null && p.removed < i ? 1 : 0);
        last = Math.min(pos + 1, limit);
        const shown = pos + (p.gap !== null && pos >= p.gap ? 1 : 0);
        if (clientX <= this.projectedLeft(shown, p) + this.geometry(i).width / 2) {
          return Math.min(pos, limit);
        }
      }
      return last;
    }
  }

  // Helper functions from src/presentation/card_content.hpp
  function extensionOf(name) {
    const s = String(name || "").replace(/[\\/]+$/, "");
    const slash = Math.max(s.lastIndexOf("\\"), s.lastIndexOf("/"));
    const leaf = slash >= 0 ? s.slice(slash + 1) : s;
    const dot = leaf.lastIndexOf(".");
    if (dot <= 0 || dot + 1 === leaf.length || leaf.length - dot > 6) return "";
    return leaf.slice(dot + 1).toUpperCase();
  }

  function linkHost(url) {
    let s = String(url || "");
    const scheme = s.indexOf("://");
    if (scheme >= 0) s = s.slice(scheme + 3);
    const end = s.search(/[/?#]/);
    return end >= 0 ? s.slice(0, end) : s;
  }

  function elidePath(pathStr, limit) {
    const s = String(pathStr || "");
    if (s.length <= limit || limit < 4) return s;
    let root = 0;
    if (s.length >= 3 && s[1] === ":" && (s[2] === "\\" || s[2] === "/")) {
      root = 3;
    } else if (s.startsWith("\\\\")) {
      const server = s.indexOf("\\", 2);
      const share = server < 0 ? -1 : s.indexOf("\\", server + 1);
      root = share < 0 ? 0 : share + 1;
    }
    const head = s.slice(0, root);
    let tail = "";
    for (let cut = s.length; cut > root; ) {
      const slash = Math.max(s.lastIndexOf("\\", cut - 1), s.lastIndexOf("/", cut - 1));
      if (slash < 0 || slash < root) break;
      const candidate = s.slice(slash + 1);
      if (head.length + 2 + candidate.length > limit) break;
      tail = candidate;
      cut = slash;
    }
    if (!tail) {
      return "\u2026" + s.slice(s.length - (limit - 1));
    }
    return head + "\u2026\\" + tail;
  }

  class WorkspacePainter {
    constructor() {
      const plaqueNoise = new Noise(0x9e3779b9);
      this.plaqueGrain = buildGrainLines(RopePlaque.width, RopePlaque.height - 3, plaqueNoise, 9, 0.8, () => 0);
      this.bars = new Map();
      this.fiberTile = null;
      this.fiberScale = 0;
      this.previewBitmaps = new Map();
    }

    getBarCache(width, base) {
      const key = `${Math.round(width * 100)}_${Math.round(base * 100)}`;
      let cached = this.bars.get(key);
      if (!cached) {
        cached = buildBarCache(width, base);
        this.bars.set(key, cached);
      }
      return cached;
    }

    ensureFiberTile(pixelScale) {
      if (this.fiberTile && this.fiberScale === pixelScale) {
        return this.fiberTile;
      }
      const tileSize = 96;
      const offscreen = document.createElement("canvas");
      offscreen.width = Math.round(tileSize * pixelScale);
      offscreen.height = Math.round(tileSize * pixelScale);
      const tctx = offscreen.getContext("2d");
      tctx.scale(pixelScale, pixelScale);
      tctx.lineCap = "round";
      tctx.lineJoin = "round";

      const noise = new Noise(0x5eed);
      for (let i = 0; i < 34; ++i) {
        const x = noise.range(0, tileSize);
        const y = noise.range(0, tileSize);
        const length = noise.range(1.6, 5);
        const angle = noise.range(0, 2 * PI);
        const bend = noise.range(-0.8, 0.8);
        const dx = Math.cos(angle);
        const dy = Math.sin(angle);
        const weight = noise.range(0.4, 0.7);
        const alpha = noise.range(0.025, 0.055);

        tctx.strokeStyle = rgbaCss(0x6e5a40, alpha);
        tctx.lineWidth = weight;
        for (const ox of [-tileSize, 0, tileSize]) {
          for (const oy of [-tileSize, 0, tileSize]) {
            tctx.beginPath();
            tctx.moveTo(x + ox, y + oy);
            tctx.quadraticCurveTo(
              x + (dx * length) / 2 - dy * bend + ox,
              y + (dy * length) / 2 + dx * bend + oy,
              x + dx * length + ox,
              y + dy * length + oy
            );
            tctx.stroke();
          }
        }
      }

      for (let i = 0; i < 120; ++i) {
        const alpha = noise.range(0.04, 0.1);
        const r = noise.range(0.2, 0.45);
        const ey = noise.range(1, tileSize - 1);
        const ex = noise.range(1, tileSize - 1);
        tctx.fillStyle = rgbaCss(0x6e5a40, alpha);
        tctx.beginPath();
        tctx.arc(ex, ey, r, 0, 2 * PI);
        tctx.fill();
      }

      this.fiberTile = offscreen;
      this.fiberScale = pixelScale;
      return offscreen;
    }

    ensurePreviewBitmap(card, w, h, pixelScale) {
      if (card.dataUrl) {
        this.images ||= new Map();
        let image = this.images.get(card.dataUrl);
        if (!image) {
          image = new Image();
          image.onload = () => this.onImageReady?.();
          image.src = card.dataUrl;
          this.images.set(card.dataUrl,image);
        }
        if (!image.complete || !image.naturalWidth) return null;
        return image;
      }
      const key = `${window.ClipRopeI18n.language}_${card.id}_${Math.round(w * 10)}_${Math.round(h * 10)}_${pixelScale}`;
      if (this.previewBitmaps.has(key)) {
        return this.previewBitmaps.get(key);
      }
      const off = document.createElement("canvas");
      off.width = Math.max(1, Math.round(w * pixelScale));
      off.height = Math.max(1, Math.round(h * pixelScale));
      const pctx = off.getContext("2d");
      pctx.scale(pixelScale, pixelScale);

      if (card.bitmapCanvas) {
        pctx.imageSmoothingEnabled = true;
        pctx.imageSmoothingQuality = "high";
        pctx.drawImage(card.bitmapCanvas, 0, 0, w, h);
        this.previewBitmaps.set(key, off);
        return off;
      }

      // Draw a crisp Windows 11 desktop / chart screenshot preview (scales proportionally to any w x h)
      const sx = w / 192;
      const sy = h / 108;
      const s = Math.min(sx, sy);

      const bg = pctx.createLinearGradient(0, 0, w, h);
      bg.addColorStop(0, "#1e293b");
      bg.addColorStop(0.55, "#0f172a");
      bg.addColorStop(1, "#1e3a5f");
      pctx.fillStyle = bg;
      pctx.fillRect(0, 0, w, h);

      // Subtle grid lines
      pctx.strokeStyle = "rgba(148, 163, 184, 0.12)";
      pctx.lineWidth = 0.6 * s;
      for (let gx = 16 * sx; gx < w; gx += 20 * sx) {
        pctx.beginPath();
        pctx.moveTo(gx, 20 * sy);
        pctx.lineTo(gx, h - 12 * sy);
        pctx.stroke();
      }
      for (let gy = 26 * sy; gy < h - 10 * sy; gy += 16 * sy) {
        pctx.beginPath();
        pctx.moveTo(10 * sx, gy);
        pctx.lineTo(w - 10 * sx, gy);
        pctx.stroke();
      }

      // Window title bar inside screenshot
      pctx.fillStyle = "rgba(30, 41, 59, 0.9)";
      pctx.fillRect(0, 0, w, 16 * sy);
      pctx.fillStyle = "#94a3b8";
      pctx.font = `600 ${Math.max(7.5, 7.5 * s)}px "Segoe UI", system-ui, sans-serif`;
      pctx.textBaseline = "alphabetic";
      pctx.fillText(t("chartTitle"), 7 * sx, 10.5 * sy);

      // Window control dots on right
      const dotColors = ["#38bdf8", "#a855f7", "#f43f5e"];
      for (let i = 0; i < 3; ++i) {
        pctx.fillStyle = dotColors[i];
        pctx.beginPath();
        pctx.arc(w - (24 - i * 8) * sx, 8 * sy, 2.1 * s, 0, 2 * PI);
        pctx.fill();
      }

      // Area chart curve
      const pts = [
        { x: 12 * sx, y: h - 22 * sy },
        { x: 38 * sx, y: h - 38 * sy },
        { x: 66 * sx, y: h - 31 * sy },
        { x: 96 * sx, y: h - 56 * sy },
        { x: 126 * sx, y: h - 49 * sy },
        { x: 154 * sx, y: h - 72 * sy },
        { x: w - 12 * sx, y: h - 66 * sy },
      ];
      const areaGrad = pctx.createLinearGradient(0, 22 * sy, 0, h - 12 * sy);
      areaGrad.addColorStop(0, "rgba(56, 189, 248, 0.42)");
      areaGrad.addColorStop(1, "rgba(56, 189, 248, 0.02)");
      pctx.fillStyle = areaGrad;
      pctx.beginPath();
      pctx.moveTo(pts[0].x, h - 12 * sy);
      for (const p of pts) pctx.lineTo(p.x, p.y);
      pctx.lineTo(pts[pts.length - 1].x, h - 12 * sy);
      pctx.closePath();
      pctx.fill();

      pctx.strokeStyle = "#38bdf8";
      pctx.lineWidth = 1.8 * s;
      pctx.lineJoin = "round";
      pctx.lineCap = "round";
      pctx.beginPath();
      pts.forEach((p, idx) => (idx === 0 ? pctx.moveTo(p.x, p.y) : pctx.lineTo(p.x, p.y)));
      pctx.stroke();

      for (const p of pts) {
        pctx.fillStyle = "#e0f2fe";
        pctx.beginPath();
        pctx.arc(p.x, p.y, 2.2 * s, 0, 2 * PI);
        pctx.fill();
      }

      this.previewBitmaps.set(key, off);
      return off;
    }

    strokePolylines(ctx, polylines, rgb, weight, alpha = 1, cap = "round") {
      ctx.save();
      ctx.strokeStyle = rgbaCss(rgb, alpha);
      ctx.lineWidth = weight;
      ctx.lineCap = cap;
      ctx.lineJoin = "round";
      ctx.beginPath();
      for (const pts of polylines) {
        if (pts.length < 2) continue;
        ctx.moveTo(pts[0].x, pts[0].y);
        for (let i = 1; i < pts.length; ++i) {
          ctx.lineTo(pts[i].x, pts[i].y);
        }
      }
      ctx.stroke();
      ctx.restore();
    }

    fillRoundedRect(ctx, x, y, w, h, r, fillStyle) {
      const radius = Math.max(0, Math.min(r, w / 2, h / 2));
      ctx.fillStyle = fillStyle;
      ctx.beginPath();
      ctx.roundRect(x, y, w, h, radius);
      ctx.fill();
    }

    strokeRoundedRect(ctx, x, y, w, h, r, strokeStyle, lineWidth) {
      const radius = Math.max(0, Math.min(r, w / 2, h / 2));
      ctx.strokeStyle = strokeStyle;
      ctx.lineWidth = lineWidth;
      ctx.beginPath();
      ctx.roundRect(x, y, w, h, radius);
      ctx.stroke();
    }

    woodGradient(ctx, top, bottom) {
      const grad = ctx.createLinearGradient(0, top, 0, bottom);
      grad.addColorStop(0.0, "#edcb93");
      grad.addColorStop(0.4, "#e2b67b");
      grad.addColorStop(0.8, "#d3a163");
      grad.addColorStop(1.0, "#be8a4c");
      return grad;
    }

    // Matches rope_art(curve, material, dpi) in src/rendering/workspace_paint.cpp (lines 689-804)
    drawRope(ctx, curve, rowTop = 0) {
      const width = curve.width;
      const t = Hanging.rope_thickness; // 9
      const lay = 0.95 * t; // 8.55
      const slant = 0.33 * t; // 2.97
      const colors = {
        deep: ropeDefaultTones[0],
        dark: ropeDefaultTones[1],
        mid: ropeDefaultTones[2],
        light: ropeDefaultTones[3],
        high: ropeDefaultTones[4],
      };

      ctx.save();
      if (rowTop !== 0) {
        ctx.translate(0, rowTop);
      }

      const centerPoints = [];
      for (let x = -8; x <= width + 8; x += 3) {
        centerPoints.push({ x, y: curve.y(x) });
      }

      const drawCenterline = (rgb, alpha, weight, dy = 0) => {
        ctx.save();
        ctx.translate(0, dy);
        ctx.strokeStyle = rgbaCss(rgb, alpha);
        ctx.lineWidth = weight;
        ctx.lineCap = "butt";
        ctx.lineJoin = "miter";
        ctx.beginPath();
        for (let i = 0; i < centerPoints.length; ++i) {
          const p = centerPoints[i];
          if (i === 0) ctx.moveTo(p.x, p.y);
          else ctx.lineTo(p.x, p.y);
        }
        ctx.stroke();
        ctx.restore();
      };

      const noise = new Noise(0xc0ffee);
      const surfaceFibers = [];
      for (let x = 0; x < width; x = f32(x + noise.range(5, 13))) {
        const y = curve.y(x) + noise.range(-t / 2.5, t / 2.5);
        const length = noise.range(2, 5);
        const tilt = noise.range(-0.8, 0.4);
        surfaceFibers.push([
          { x, y },
          { x: x + length, y: y + tilt },
        ]);
      }

      const looseHairs = [];
      for (let x = noise.range(0, 10); x < width; x = f32(x + noise.range(6, 16))) {
        const above = noise.next() < 0.55;
        const y = curve.y(x) + (above ? -1 : 1) * (t / 2 - 0.6);
        const reach = noise.range(1.4, 3.8) * (above ? -1 : 1);
        const drift = noise.range(-2.8, 2.8);
        looseHairs.push({
          x0: x,
          y0: y,
          cx: x + drift * 0.3,
          cy: y + reach * 0.7,
          x1: x + drift,
          y1: y + reach,
        });
      }

      // 1. Drop shadows & base rope strokes
      drawCenterline(0x000000, 0.07, t + 3, 2.8);
      drawCenterline(0x000000, 0.09, t, 2.4);
      drawCenterline(colors.deep, 0.9, t + 1.1, 0);
      drawCenterline(colors.mid, 1.0, t, 0);

      // 2. Clipped inner rope shading, laid strands, grooves, and glints
      ctx.save();
      ctx.beginPath();
      for (let i = 0; i < centerPoints.length; ++i) {
        const p = centerPoints[i];
        if (i === 0) ctx.moveTo(p.x, p.y - t / 2);
        else ctx.lineTo(p.x, p.y - t / 2);
      }
      for (let i = centerPoints.length - 1; i >= 0; --i) {
        const p = centerPoints[i];
        ctx.lineTo(p.x, p.y + t / 2);
      }
      ctx.closePath();
      ctx.clip();

      drawCenterline(colors.dark, 0.55, t * 0.42, t * 0.3);
      drawCenterline(colors.light, 0.45, t * 0.36, -t * 0.2);

      const drawStrands = (along, from, to, rgb, alpha, weight) => {
        ctx.save();
        ctx.strokeStyle = rgbaCss(rgb, alpha);
        ctx.lineWidth = weight;
        ctx.lineCap = "round";
        ctx.beginPath();
        for (let x = -lay; x <= width + lay; x += lay) {
          const cx = x + lay / 2 + along;
          const cy = curve.y(cx) - 0.1 * t;
          ctx.moveTo(cx - slant * from, cy + t * from);
          ctx.lineTo(cx + slant * to, cy - t * to);
        }
        ctx.stroke();
        ctx.restore();
      };

      drawStrands(0, 0.3, 0.3, colors.light, 0.55, t * 0.3);

      const strokeGrooves = (dx, rgb, alpha, weight) => {
        ctx.save();
        ctx.translate(dx, 0);
        ctx.strokeStyle = rgbaCss(rgb, alpha);
        ctx.lineWidth = weight;
        ctx.lineCap = "round";
        ctx.beginPath();
        for (let x = -lay; x <= width + lay; x += lay) {
          const y = curve.y(x);
          ctx.moveTo(x - slant, y + t / 2 + 0.8);
          ctx.quadraticCurveTo(x + 0.06 * t, y + 0.03 * t, x + slant, y - t / 2 - 0.8);
        }
        ctx.stroke();
        ctx.restore();
      };

      strokeGrooves(-0.14 * t, colors.dark, 0.3, t * 0.24);
      strokeGrooves(0, colors.deep, 0.78, 1.2);

      drawStrands(0.08 * t, -0.05, 0.24, colors.high, 0.6, t * 0.11);
      this.strokePolylines(ctx, surfaceFibers, colors.high, 0.45, 0.28, "round");
      drawCenterline(colors.high, 0.3, 0.8, -t * 0.36);

      ctx.restore();

      // 3. Unclipped loose hemp hairs
      ctx.save();
      ctx.strokeStyle = rgbaCss(mixColor(colors.mid, colors.light, 0.4), 0.6);
      ctx.lineWidth = 0.5;
      ctx.lineCap = "round";
      ctx.beginPath();
      for (const h of looseHairs) {
        ctx.moveTo(h.x0, h.y0);
        ctx.quadraticCurveTo(h.cx, h.cy, h.x1, h.y1);
      }
      ctx.stroke();
      ctx.restore();

      ctx.restore();
    }

    drawPlaque(ctx, curve, rowTop, title, ropeCount) {
      const half = RopePlaque.width / 2; // 72
      const height = RopePlaque.height; // 48
      const radius = RopePlaque.radius; // 5
      const left = (curve.width - RopePlaque.width) / 2;
      const top = rowTop + curve.lowest() + 0.4 - RopePlaque.gap - height;

      ctx.save();
      ctx.translate(left + half, top);

      // Bottom bevel box
      this.fillRoundedRect(ctx, -half, 0, RopePlaque.width, height, radius, rgbaCss(skin.wood_bottom, 1));

      // Wood face
      const faceX = -half + 0.5;
      const faceY = 0.5;
      const faceW = RopePlaque.width - 1;
      const faceH = height - 2.9;
      this.fillRoundedRect(ctx, faceX, faceY, faceW, faceH, radius - 0.5, this.woodGradient(ctx, 0, height - 2.4));

      // Wood grain on plaque
      this.strokePolylines(ctx, this.plaqueGrain, skin.wood_grain, 0.7, 0.22, "round");

      // Border
      this.strokeRoundedRect(ctx, faceX, faceY, faceW, faceH, radius - 0.5, rgbaCss(skin.wood_edge, 1), 0.9);

      // Top highlight line
      ctx.strokeStyle = rgbaCss(skin.wood_light, 0.85);
      ctx.lineWidth = 1;
      ctx.lineCap = "round";
      ctx.beginPath();
      ctx.moveTo(-half + radius, 1.5);
      ctx.lineTo(half - radius, 1.5);
      ctx.stroke();

      // Corner steel rivets
      for (const x of [10 - half, half - 10]) {
        ctx.fillStyle = rgbaCss(skin.wood_bottom, 0.6);
        ctx.beginPath();
        ctx.ellipse(x, 9.4, 3.8, 3.8, 0, 0, 2 * PI);
        ctx.fill();

        ctx.fillStyle = rgbaCss(skin.steel, 1);
        ctx.beginPath();
        ctx.ellipse(x, 8.5, 3.4, 3.4, 0, 0, 2 * PI);
        ctx.fill();

        ctx.strokeStyle = rgbaCss(skin.steel_edge, 1);
        ctx.lineWidth = 0.8;
        ctx.beginPath();
        ctx.ellipse(x, 8.5, 3.4, 3.4, 0, 0, 2 * PI);
        ctx.stroke();

        ctx.fillStyle = rgbaCss(skin.steel_light, 1);
        ctx.beginPath();
        ctx.ellipse(x - 0.6, 7.2, 1.9, 1.0, 0, 0, 2 * PI);
        ctx.fill();
      }

      // Plaque title ("main") and subtitle ("tol:3")
      ctx.fillStyle = rgbaCss(skin.paper_edge, 1);
      ctx.textAlign = "center";
      ctx.textBaseline = "alphabetic";
      ctx.font = '600 17px "Segoe UI", system-ui, sans-serif';
      ctx.fillText(title, 0, 20.8);

      ctx.font = '600 12px "Segoe UI", system-ui, sans-serif';
      ctx.fillText(t("ropeCount", ropeCount), 0, 38.2);

      ctx.restore();
    }

    drawDropGap(ctx, hookX, rowTop, curve) {
      const cy = rowTop + curve.contact(hookX) + Hanging.pivot;
      ctx.save();
      ctx.strokeStyle = rgbaCss(skin.drop, 1);
      ctx.lineWidth = 1.6;
      ctx.beginPath();
      ctx.arc(hookX, cy, 7, 0, 2 * PI);
      ctx.stroke();
      ctx.restore();
    }

    applyCardTransform(ctx, hookX, contactY, angleDeg) {
      ctx.translate(hookX, contactY);
      if (angleDeg !== 0) {
        ctx.translate(0, Hanging.pivot);
        ctx.rotate((angleDeg * PI) / 180);
        ctx.translate(0, -Hanging.pivot);
      }
    }

    drawHookBehind(ctx, hookX, contactY, angleDeg) {
      const a0 = (-240 * PI) / 180;
      const a1 = (-CROSSING_DEG * PI) / 180;

      const traceBackArc = () => {
        ctx.beginPath();
        ctx.arc(LOOP_X, LOOP_Y, LOOP_RADIUS, a0, a1, false);
      };

      ctx.save();
      this.applyCardTransform(ctx, hookX, contactY, angleDeg);
      ctx.lineCap = "round";
      ctx.lineJoin = "round";

      ctx.strokeStyle = rgbaCss(skin.steel_edge, 1);
      ctx.lineWidth = WIRE + 1.2;
      traceBackArc();
      ctx.stroke();

      ctx.strokeStyle = rgbaCss(mixColor(skin.steel, skin.steel_dark, 0.35), 1);
      ctx.lineWidth = WIRE;
      traceBackArc();
      ctx.stroke();

      ctx.translate(-0.55, -0.55);
      ctx.strokeStyle = rgbaCss(skin.steel_light, 0.6);
      ctx.lineWidth = 0.8;
      traceBackArc();
      ctx.stroke();

      ctx.restore();
    }

    drawHookFront(ctx) {
      const aStart = (-CROSSING_DEG * PI) / 180;
      const aBend = (45 * PI) / 180;
      const bend = loopPoint(-45);

      const traceFront = () => {
        ctx.beginPath();
        ctx.arc(LOOP_X, LOOP_Y, LOOP_RADIUS, aStart, aBend, false);
        ctx.bezierCurveTo(bend.x - 1.6, bend.y + 1.6, 0, bend.y + 1.8, 0, NECK_TOP);
        ctx.lineTo(0, Hanging.bar_top + 1);
      };

      ctx.save();
      ctx.lineCap = "butt";
      ctx.lineJoin = "round";

      ctx.strokeStyle = rgbaCss(skin.steel_edge, 1);
      ctx.lineWidth = WIRE + 1.2;
      traceFront();
      ctx.stroke();

      ctx.strokeStyle = rgbaCss(skin.steel, 1);
      ctx.lineWidth = WIRE;
      traceFront();
      ctx.stroke();

      ctx.save();
      ctx.translate(-0.55, -0.55);
      ctx.strokeStyle = rgbaCss(skin.steel_light, 0.9);
      ctx.lineWidth = 0.9;
      traceFront();
      ctx.stroke();
      ctx.restore();

      // Twisted neck diagonal ridges
      const half = WIRE / 2 + 0.2;
      ctx.lineCap = "round";
      for (let y = NECK_TOP + 1.4; y < Hanging.bar_top - 0.6; y += 1.9) {
        ctx.strokeStyle = rgbaCss(skin.steel_dark, 0.9);
        ctx.lineWidth = 0.85;
        ctx.beginPath();
        ctx.moveTo(-half, y + 0.95);
        ctx.lineTo(half, y - 0.95);
        ctx.stroke();

        ctx.strokeStyle = rgbaCss(skin.steel_light, 0.8);
        ctx.lineWidth = 0.6;
        ctx.beginPath();
        ctx.moveTo(-half + 0.5, y - 0.2);
        ctx.lineTo(half - 0.3, y - 1.45);
        ctx.stroke();
      }
      ctx.restore();
    }

    // Monochrome line icons drawn in a 13-DIP box (build_icons in workspace_paint.cpp lines 324-385)
    drawCardIcon(ctx, kind, boxRect, rgb, weight = 1.25) {
      const scale = (boxRect.right - boxRect.left) / 13;
      ctx.save();
      ctx.translate(boxRect.left, boxRect.top);
      ctx.scale(scale, scale);
      ctx.strokeStyle = rgbaCss(rgb, 1);
      ctx.lineWidth = weight / scale;
      ctx.lineCap = "round";
      ctx.lineJoin = "round";

      if (kind === "text") {
        const rows = [2.4, 5.1, 7.8, 10.5];
        const ends = [12, 12, 12, 7.6];
        ctx.beginPath();
        for (let i = 0; i < 4; ++i) {
          ctx.moveTo(1, rows[i]);
          ctx.lineTo(ends[i], rows[i]);
        }
        ctx.stroke();
      } else if (kind === "files") {
        ctx.beginPath();
        ctx.moveTo(2.6, 0.8);
        ctx.lineTo(8.2, 0.8);
        ctx.lineTo(10.6, 3.2);
        ctx.lineTo(10.6, 12.2);
        ctx.lineTo(2.6, 12.2);
        ctx.closePath();
        ctx.moveTo(8.2, 0.8);
        ctx.lineTo(8.2, 3.2);
        ctx.lineTo(10.6, 3.2);
        ctx.stroke();
      } else if (kind === "rich") {
        // icons[CardKind::rich]: page outline + 3 lines at y = {6.2, 8.4, 10.4}
        ctx.beginPath();
        ctx.moveTo(2.6, 0.8);
        ctx.lineTo(8.2, 0.8);
        ctx.lineTo(10.6, 3.2);
        ctx.lineTo(10.6, 12.2);
        ctx.lineTo(2.6, 12.2);
        ctx.closePath();
        ctx.moveTo(8.2, 0.8);
        ctx.lineTo(8.2, 3.2);
        ctx.lineTo(10.6, 3.2);
        for (const y of [6.2, 8.4, 10.4]) {
          ctx.moveTo(4.4, y);
          ctx.lineTo(y < 10 ? 8.8 : 7.2, y);
        }
        ctx.stroke();
      } else if (kind === "folder") {
        ctx.beginPath();
        ctx.moveTo(0.8, 2.6);
        ctx.lineTo(4.8, 2.6);
        ctx.lineTo(6.1, 4.1);
        ctx.lineTo(12.2, 4.1);
        ctx.lineTo(12.2, 11.0);
        ctx.lineTo(0.8, 11.0);
        ctx.closePath();
        ctx.moveTo(0.8, 5.6);
        ctx.lineTo(12.2, 5.6);
        ctx.stroke();
      } else if (kind === "link") {
        for (const d of [-1.9, 1.9]) {
          ctx.save();
          ctx.translate(d, -d);
          ctx.translate(6.5, 6.5);
          ctx.rotate((-45 * PI) / 180);
          ctx.translate(-6.5, -6.5);
          ctx.beginPath();
          ctx.roundRect(6.5 - 3.9, 6.5 - 2.1, 7.8, 4.2, 2.1);
          ctx.stroke();
          ctx.restore();
        }
      } else if (kind === "image") {
        ctx.beginPath();
        ctx.roundRect(0.7, 1.8, 11.6, 9.4, 1.8);
        ctx.stroke();
        ctx.beginPath();
        ctx.moveTo(2.2, 9.6);
        ctx.lineTo(5.2, 6.2);
        ctx.lineTo(7.4, 8.4);
        ctx.lineTo(9.0, 7.0);
        ctx.lineTo(10.9, 9.6);
        ctx.stroke();
        ctx.beginPath();
        ctx.arc(9.1, 4.4, 1.15, 0, 2 * PI);
        ctx.stroke();
      } else if (kind === "unavailable") {
        ctx.beginPath();
        ctx.moveTo(6.5, 1.0);
        ctx.lineTo(12.3, 11.6);
        ctx.lineTo(0.7, 11.6);
        ctx.closePath();
        ctx.moveTo(6.5, 4.8);
        ctx.lineTo(6.5, 7.9);
        ctx.stroke();
        ctx.beginPath();
        ctx.arc(6.5, 9.9, 0.35, 0, 2 * PI);
        ctx.stroke();
      }
      ctx.restore();
    }

    drawPencil(ctx, boxRect) {
      const x = boxRect.left;
      const y = boxRect.top;
      ctx.save();
      ctx.lineCap = "round";

      ctx.strokeStyle = rgbaCss(skin.label, 1);
      ctx.lineWidth = 3.3;
      ctx.beginPath();
      ctx.moveTo(x + 3, y + 9);
      ctx.lineTo(x + 10, y + 2);
      ctx.stroke();

      ctx.lineWidth = 1.2;
      ctx.beginPath();
      ctx.moveTo(x + 1, y + 12);
      ctx.lineTo(x + 3, y + 9);
      ctx.stroke();

      ctx.strokeStyle = rgbaCss(skin.wood_light, 1);
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(x + 8, y + 2);
      ctx.lineTo(x + 11, y + 5);
      ctx.stroke();
      ctx.restore();
    }

    drawCloseMark(ctx, cx, cy, hot, paperRgb, chip = true) {
      ctx.save();
      if (chip) {
        ctx.fillStyle = rgbaCss(0x000000, 0.1);
        ctx.beginPath();
        ctx.arc(cx, cy + 0.7, 10.4, 0, 2 * PI);
        ctx.fill();

        ctx.fillStyle = rgbaCss(paperRgb, 0.96);
        ctx.beginPath();
        ctx.arc(cx, cy, 10, 0, 2 * PI);
        ctx.fill();

        ctx.strokeStyle = rgbaCss(mixColor(paperRgb, skin.paper_edge, 0.4), 1);
        ctx.lineWidth = 0.8;
        ctx.beginPath();
        ctx.arc(cx, cy, 9.6, 0, 2 * PI);
        ctx.stroke();
      }

      if (hot) {
        ctx.fillStyle = rgbaCss(0x000000, 0.08);
        ctx.beginPath();
        ctx.arc(cx, cy, 10, 0, 2 * PI);
        ctx.fill();
      }

      const d = 3.6;
      ctx.strokeStyle = rgbaCss(skin.icon, 1);
      ctx.lineWidth = 1.5;
      ctx.lineCap = "round";
      ctx.beginPath();
      ctx.moveTo(cx - d, cy - d);
      ctx.lineTo(cx + d, cy + d);
      ctx.moveTo(cx + d, cy - d);
      ctx.lineTo(cx - d, cy + d);
      ctx.stroke();
      ctx.restore();
    }

    // Matches return_icon in src/rendering/workspace_paint.cpp (lines 385-392)
    drawReturnIcon(ctx, boxRect, rgb, weight = 1.5) {
      const scale = (boxRect.right - boxRect.left) / 13;
      ctx.save();
      ctx.translate(boxRect.left, boxRect.top);
      ctx.scale(scale, scale);
      ctx.strokeStyle = rgbaCss(rgb, 1);
      ctx.lineWidth = weight / scale;
      ctx.lineCap = "round";
      ctx.lineJoin = "round";
      ctx.beginPath();
      // Horizontal rope line at top
      ctx.moveTo(0.8, 1.6);
      ctx.lineTo(12.2, 1.6);
      // Vertical arrow shaft
      ctx.moveTo(6.5, 12.0);
      ctx.lineTo(6.5, 4.4);
      // Arrow head pointing up to the rope
      ctx.moveTo(3.4, 7.4);
      ctx.lineTo(6.5, 4.3);
      ctx.lineTo(9.6, 7.4);
      ctx.stroke();
      ctx.restore();
    }

    // Matches wood_bar in src/rendering/workspace_paint.cpp (lines 1353-1386)
    drawWoodBar(ctx, card, slotIndex, capacity, g, editAffordance) {
      const b = this.getBarCache(g.hanger_width, g.bar_bottom);
      const h = b.h;
      const base = b.base;
      const hump = b.hump;

      const traceProfile = () => {
        ctx.beginPath();
        ctx.moveTo(-h + 2.5, base);
        ctx.lineTo(h - 2.5, base);
        ctx.quadraticCurveTo(h, base, h, base - 2.5);
        ctx.lineTo(h, Hanging.bar_end + END_RADIUS);
        ctx.quadraticCurveTo(h, Hanging.bar_end, h - END_RADIUS, Hanging.bar_end);
        ctx.lineTo(hump, SHOULDER);
        ctx.bezierCurveTo(
          0.62 * hump,
          SHOULDER - 0.3,
          0.43 * hump,
          Hanging.bar_top,
          0,
          Hanging.bar_top
        );
        ctx.bezierCurveTo(
          -0.43 * hump,
          Hanging.bar_top,
          -0.62 * hump,
          SHOULDER - 0.3,
          -hump,
          SHOULDER
        );
        ctx.lineTo(-h + END_RADIUS, Hanging.bar_end);
        ctx.quadraticCurveTo(-h, Hanging.bar_end, -h, Hanging.bar_end + END_RADIUS);
        ctx.lineTo(-h, base - 2.5);
        ctx.quadraticCurveTo(-h, base, -h + 2.5, base);
        ctx.closePath();
      };

      ctx.save();
      ctx.fillStyle = this.woodGradient(ctx, Hanging.bar_top, Hanging.bar_bottom);
      traceProfile();
      ctx.fill();

      const seed = cardSeed(card);
      const variant = seed % 4;
      this.strokePolylines(ctx, b.grain[variant], skin.wood_grain, 0.7, 0.22, "round");
      this.strokePolylines(ctx, b.streaks[variant], 0x8a5a2b, 0.55, 0.3, "round");
      this.strokePolylines(ctx, [b.highlight], skin.wood_light, 1.0, 0.85, "round");

      // Bottom edge bevel shadow
      ctx.strokeStyle = rgbaCss(skin.wood_bottom, 0.75);
      ctx.lineWidth = 1.1;
      ctx.lineCap = "round";
      ctx.beginPath();
      ctx.moveTo(-h + 2.5, base - 0.9);
      ctx.lineTo(h - 2.5, base - 0.9);
      ctx.stroke();

      // Profile outline
      ctx.strokeStyle = rgbaCss(skin.wood_edge, 1);
      ctx.lineWidth = 0.9;
      ctx.lineJoin = "round";
      traceProfile();
      ctx.stroke();

      // Hook entry hole at top of bar
      ctx.fillStyle = rgbaCss(0x6b4724, 0.55);
      ctx.beginPath();
      ctx.ellipse(0, Hanging.bar_top + 0.9, 2.3, 0.95, 0, 0, 2 * PI);
      ctx.fill();

      // Engraved slot number (#1/50, #2/50, ...)
      const pad = 9;
      const iconSize = 13;
      const capStr = capacity ? String(capacity) : "\u221e";
      const numberText = `#${slotIndex + 1}/${capStr}`;
      ctx.font = '600 12px "Segoe UI", system-ui, sans-serif';
      ctx.textAlign = "left";
      ctx.textBaseline = "alphabetic";
      const labelX = -h + pad;
      const labelBaselineY = (Hanging.bar_end + g.bar_bottom) / 2 + 4.1;

      ctx.fillStyle = rgbaCss(skin.engraving, 0.55);
      ctx.fillText(numberText, labelX, labelBaselineY + 0.7);
      ctx.fillStyle = rgbaCss(skin.label, 1);
      ctx.fillText(numberText, labelX, labelBaselineY);

      // Engraved type icon on right side of bar (icon_kind in workspace_paint.cpp line 1136)
      const iconBox = {
        left: h - pad - iconSize,
        top: 27.5,
        right: h - pad,
        bottom: 27.5 + iconSize,
      };
      if (editAffordance) {
        this.drawPencil(ctx, iconBox);
      } else {
        const iconKind = card.image_primary ? "image" : card.kind === "rich" ? "text" : card.kind;
        this.drawCardIcon(
          ctx,
          iconKind,
          {
            left: iconBox.left,
            top: iconBox.top + 0.7,
            right: iconBox.right,
            bottom: iconBox.bottom + 0.7,
          },
          skin.engraving,
          1.25
        );
        this.drawCardIcon(ctx, iconKind, iconBox, skin.icon, 1.25);
      }

      ctx.restore();
    }

    // Matches document(cx, top, file) in src/rendering/workspace_paint.cpp (lines 926-967)
    drawDocumentArt(ctx, cx, top, filename, explicitExt) {
      const w = 32;
      const h = 40;
      const l = cx - w / 2;
      const fold = 9;

      const traceSheet = () => {
        ctx.beginPath();
        ctx.moveTo(l, top);
        ctx.lineTo(l + w - fold, top);
        ctx.lineTo(l + w, top + fold);
        ctx.lineTo(l + w, top + h);
        ctx.lineTo(l, top + h);
        ctx.closePath();
      };

      ctx.save();
      // Sheet drop shadow
      ctx.save();
      ctx.translate(0.8, 1.6);
      ctx.fillStyle = rgbaCss(0x000000, 0.08);
      traceSheet();
      ctx.fill();
      ctx.restore();

      // Sheet fill & outline
      ctx.fillStyle = rgbaCss(0xfffdf8, 1);
      traceSheet();
      ctx.fill();

      ctx.strokeStyle = rgbaCss(0x8c7b68, 1);
      ctx.lineWidth = 1.1;
      ctx.lineCap = "round";
      ctx.lineJoin = "round";
      traceSheet();
      ctx.stroke();

      // Corner fold
      ctx.beginPath();
      ctx.moveTo(l + w - fold, top);
      ctx.lineTo(l + w - fold, top + fold);
      ctx.lineTo(l + w, top + fold);
      ctx.stroke();

      // Extension badge
      const ext = explicitExt !== undefined ? explicitExt : extensionOf(filename);
      if (ext) {
        let color = skin.icon;
        if (ext === "PDF") color = 0xc8473b;
        else if (["DOC", "DOCX", "RTF"].includes(ext)) color = 0x2f6db5;
        else if (["XLS", "XLSX", "CSV"].includes(ext)) color = 0x2e8b57;
        else if (["PPT", "PPTX"].includes(ext)) color = 0xd0622f;
        else if (["ZIP", "7Z", "RAR"].includes(ext)) color = 0x8d6e4a;
        else if (["PNG", "JPG", "JPEG", "GIF", "BMP", "SVG", "WEBP", "TIF", "TIFF"].includes(ext)) color = 0x7a5ba6;

        ctx.font = '700 8px "Segoe UI", system-ui, sans-serif';
        const measured = ctx.measureText(ext).width;
        const labelWidth = Math.min(w + 6, Math.max(18, measured + 7));
        const bx = cx - labelWidth / 2;
        const by = top + h - 15;
        const bh = 10;
        this.fillRoundedRect(ctx, bx, by, labelWidth, bh, 2, rgbaCss(color, 1));

        ctx.fillStyle = "#ffffff";
        ctx.textAlign = "center";
        ctx.textBaseline = "alphabetic";
        ctx.fillText(ext, cx, by + 7.8);
      }

      ctx.restore();
    }

    // Matches folder_art(cx, top) in src/rendering/workspace_paint.cpp (lines 968-976)
    drawFolderArt(ctx, cx, top) {
      const w = 40;
      const h = 30;
      const l = cx - w / 2;
      const t = top + 6;
      ctx.save();
      this.fillRoundedRect(ctx, l + 0.8, t + 1.6, w, h, 3, rgbaCss(0x000000, 0.08));
      this.fillRoundedRect(ctx, l, t - 4, 16, 8, 2.5, rgbaCss(0xd4a85a, 1));
      this.fillRoundedRect(ctx, l, t, w, h, 3, rgbaCss(0xe5c07b, 1));
      this.fillRoundedRect(ctx, l, t + 5, w, h - 5, 3, rgbaCss(0xedcd8e, 1));
      this.strokeRoundedRect(ctx, l, t, w, h, 3, rgbaCss(0xa07c3c, 1), 1);
      ctx.restore();
    }

    // Matches notice(kind, message, area, rgb) in src/rendering/workspace_paint.cpp (lines 1003-1007)
    drawNotice(ctx, kind, message, areaLeft, areaTop, areaRight, areaBottom, rgb) {
      const cx = (areaLeft + areaRight) / 2;
      const cy = areaTop + (areaBottom - areaTop) * 0.38;
      this.drawCardIcon(
        ctx,
        kind,
        { left: cx - 12, top: cy - 12, right: cx + 12, bottom: cy + 12 },
        rgb,
        1.5
      );
      ctx.save();
      ctx.fillStyle = rgbaCss(rgb, 1);
      ctx.font = '400 12px "Segoe UI", system-ui, sans-serif';
      ctx.textAlign = "center";
      ctx.textBaseline = "alphabetic";
      ctx.fillText(message, cx, cy + 31);
      ctx.restore();
    }

    drawVerticalFade(ctx, left, top, right, bottom, rgb, fromAlpha, toAlpha) {
      const bands = 10;
      const step = (bottom - top) / bands;
      const w = right - left;
      for (let i = 0; i < bands; ++i) {
        const a = fromAlpha + ((toAlpha - fromAlpha) * (i + 0.5)) / bands;
        ctx.fillStyle = rgbaCss(rgb, a);
        ctx.fillRect(left, top + i * step, w, step + 0.15);
      }
    }

    // Matches body_text in src/rendering/workspace_paint.cpp (lines 912-925)
    drawBodyText(ctx, textStr, areaLeft, areaTop, areaRight, areaBottom, rgb, fontSize, bold, paperRgb) {
      const s = String(textStr || "");
      if (!s || areaBottom - areaTop < 4) return;
      const maxWidth = Math.max(1, areaRight - areaLeft);
      const areaHeight = areaBottom - areaTop;
      const lineSpacing = 1.3 * fontSize;
      const baselineOffset = 1.04 * fontSize;

      ctx.save();
      ctx.beginPath();
      ctx.rect(areaLeft, areaTop, maxWidth, areaHeight);
      ctx.clip();

      ctx.fillStyle = rgbaCss(rgb, 1);
      ctx.font = `${bold ? 700 : 400} ${fontSize}px "Segoe UI", system-ui, sans-serif`;
      ctx.textAlign = "left";
      ctx.textBaseline = "alphabetic";

      const paragraphs = s.split(/\r?\n/);
      const lines = [];
      for (const para of paragraphs) {
        if (!para) {
          lines.push("");
          continue;
        }
        let current = "";
        const tokens = para.match(/(\S+\s*|\s+)/g) || [para];
        for (const token of tokens) {
          if (ctx.measureText(current + token).width <= maxWidth) {
            current += token;
          } else {
            if (current) {
              lines.push(current.trimEnd());
              current = "";
            }
            if (ctx.measureText(token).width <= maxWidth) {
              current = token.trimStart();
            } else {
              for (const ch of token) {
                if (ctx.measureText(current + ch).width > maxWidth && current) {
                  lines.push(current);
                  current = ch;
                } else {
                  current += ch;
                }
              }
            }
          }
        }
        if (current) lines.push(current);
      }

      for (let i = 0; i < lines.length; ++i) {
        const y = areaTop + baselineOffset + i * lineSpacing;
        if (y - fontSize > areaBottom + 4) break;
        ctx.fillText(lines[i], areaLeft, y);
      }

      const totalHeight = lines.length * lineSpacing;
      if (totalHeight > areaHeight + 0.5) {
        const band = Math.min(18, 1.1 * fontSize);
        this.drawVerticalFade(ctx, areaLeft - 1, areaBottom - band, areaRight + 1, areaBottom + 1, paperRgb, 0, 1);
      }

      ctx.restore();
    }

    drawSingleLineEllipsis(ctx, textStr, x, y, maxWidth, align = "left") {
      const s = String(textStr || "");
      if (!s) return;
      ctx.textAlign = align;
      ctx.textBaseline = "alphabetic";
      if (ctx.measureText(s).width <= maxWidth) {
        ctx.fillText(s, x, y);
        return;
      }
      const ellipsis = "\u2026";
      let lo = 0;
      let hi = s.length;
      while (lo < hi) {
        const mid = (lo + hi + 1) >>> 1;
        if (ctx.measureText(s.slice(0, mid) + ellipsis).width <= maxWidth) {
          lo = mid;
        } else {
          hi = mid - 1;
        }
      }
      ctx.fillText(s.slice(0, lo) + ellipsis, x, y);
    }

    // Matches image_body & resolution in src/rendering/workspace_paint.cpp (lines 1278-1315)
    drawImageBody(ctx, card, g, hovered, pixelScale) {
      const left = -g.body_width / 2;
      const right = g.body_width / 2;
      const top = g.top;
      const bottom = g.bottom;
      const w = g.body_width;
      const h = bottom - top;

      const spread = hovered ? 7.0 : 4.5;
      const dy = hovered ? 4.6 : 2.4;
      const alpha = hovered ? 0.05 : 0.035;
      for (let i = 6; i > 0; --i) {
        const d = (spread * i) / 6;
        ctx.fillStyle = rgbaCss(0x000000, alpha);
        ctx.fillRect(left - d / 2, top + 2, d / 2, h - 2);
        ctx.fillRect(right, top + 2, d / 2, h - 2);
        this.fillRoundedRect(ctx, left - d / 2, bottom, w + d, dy + d, 2, rgbaCss(0x000000, alpha));
      }

      const bmp = this.ensurePreviewBitmap(card, w, h, pixelScale);
      if (bmp) ctx.drawImage(bmp, left, top, w, h);

      ctx.strokeStyle = rgbaCss(skin.paper_edge, 0.3);
      ctx.lineWidth = 0.8;
      ctx.strokeRect(left, top, w, h);

      if (hovered && card.source && card.source.width && card.source.height) {
        const dimText = `${card.source.width} \u00d7 ${card.source.height} px`;
        ctx.save();
        ctx.font = '400 10px "Segoe UI", system-ui, sans-serif';
        const tw = ctx.measureText(dimText).width;
        const half = tw / 2 + 4;
        const bh = 15;
        this.fillRoundedRect(ctx, -half, bottom - bh - 4, half * 2, bh, 2, rgbaCss(0x202020, 0.85));
        ctx.fillStyle = "#ffffff";
        ctx.textAlign = "center";
        ctx.textBaseline = "alphabetic";
        ctx.fillText(dimText, 0, bottom - 7.5);
        ctx.restore();
      }
    }

    // Matches content(card, area, paper) in src/rendering/workspace_paint.cpp (lines 1008-1090)
    drawCardContent(ctx, card, areaLeft, areaTop, areaRight, areaBottom, paperRgb) {
      const areaWidth = areaRight - areaLeft;
      const areaHeight = areaBottom - areaTop;
      const cx = (areaLeft + areaRight) / 2;

      if (card.kind === "text") {
        this.drawBodyText(ctx, card.text, areaLeft, areaTop, areaRight, areaBottom, 0x000000, 14, false, paperRgb);
      } else if (card.kind === "files" || card.kind === "folder") {
        const isFolder = card.kind === "folder";
        const fileTop = areaTop + Math.max(0, Math.min(6, (areaHeight - 100) / 2));
        if (isFolder) {
          this.drawFolderArt(ctx, cx, fileTop);
        } else {
          this.drawDocumentArt(ctx, cx, fileTop + 2, card.text, card.ext);
        }

        // Filename / folder name (13px Semibold centered)
        ctx.fillStyle = rgbaCss(skin.label, 1);
        ctx.font = '600 13px "Segoe UI", system-ui, sans-serif';
        this.drawSingleLineEllipsis(ctx, card.text, cx, fileTop + 62.8, areaWidth, "center");

        // File kind / item count (11px Regular centered)
        ctx.fillStyle = rgbaCss(skin.muted, 1);
        ctx.font = '400 11px "Segoe UI", system-ui, sans-serif';
        this.drawSingleLineEllipsis(ctx, card.detail || (isFolder ? t("folder") : t("file")), cx, fileTop + 77.6, areaWidth, "center");

        // Elided folder location (10px Regular left-aligned)
        if (card.location && areaHeight >= 96) {
          const limit = Math.max(8, Math.floor(areaWidth / 5.2));
          const loc = card.location.includes("\u2026") ? card.location : elidePath(card.location, limit);
          ctx.fillStyle = rgbaCss(skin.faint, 1);
          ctx.font = '400 10px "Segoe UI", system-ui, sans-serif';
          this.drawSingleLineEllipsis(ctx, loc, areaLeft, fileTop + 92.2, areaWidth, "left");
        }
      } else if (card.kind === "link") {
        const hostName = card.host || linkHost(card.text);
        const size = 14;
        const split = Math.min(areaBottom, areaTop + size * 1.5);
        this.drawBodyText(ctx, hostName, areaLeft, areaTop, areaRight, split, 0x000000, size, false, paperRgb);
        if (split + 3 < areaBottom) {
          this.drawBodyText(ctx, card.text, areaLeft, split + 3, areaRight, areaBottom, 0x000000, size, false, paperRgb);
        }
      } else if (card.kind === "rich") {
        this.drawNotice(ctx, "rich", card.notice || t("richContent"), areaLeft, areaTop, areaRight, areaBottom, skin.muted);
      } else if (card.kind === "unavailable") {
        this.drawNotice(ctx, "unavailable", card.notice || t("unavailable"), areaLeft, areaTop, areaRight, areaBottom, skin.warning);
      }
    }

    drawStatusDot(ctx, card, sx, sy) {
      if (card.status && card.status !== "complete") {
        const dotColor = card.status === "partial" ? skin.partial : skin.damaged;
        ctx.fillStyle = rgbaCss(dotColor, 1);
        ctx.beginPath();
        ctx.arc(sx, sy, 3, 0, 2 * PI);
        ctx.fill();

        ctx.strokeStyle = rgbaCss(mixColor(dotColor, 0x000000, 0.3), 1);
        ctx.lineWidth = 0.8;
        ctx.beginPath();
        ctx.arc(sx, sy, 3, 0, 2 * PI);
        ctx.stroke();
      }
    }

    drawPaperSheet(ctx, card, g, hovered, pixelScale) {
      const w = g.width;
      const x0 = -w / 2;
      const x1 = w / 2;
      const top = g.top - Hanging.paper_tuck; // 38
      const bottom = g.bottom; // 190
      const paperRgb = card.paper_color || skin.warm_paper;

      // 1. Soft 6-ring contact shadow (lines 1103-1112 of workspace_paint.cpp)
      const dx = hovered ? 1.2 : 0.8;
      const dy = hovered ? 4.6 : 2.4;
      const spread = hovered ? 7.0 : 4.5;
      const alpha = hovered ? 0.05 : 0.035;
      const rings = 6;
      for (let i = rings; i >= 1; --i) {
        const ringG = (spread * i) / rings;
        const sx = x0 + dx - ringG * 0.5;
        const sy = top + dy + ringG * 0.2;
        const sw = x1 - x0 + ringG;
        const sh = bottom - top + ringG * 0.8;
        this.fillRoundedRect(ctx, sx, sy, sw, sh, 2 + ringG, rgbaCss(0x000000, alpha));
      }

      // 2. Warm paper background
      ctx.fillStyle = rgbaCss(paperRgb, 1);
      ctx.fillRect(x0, top, w, bottom - top);

      // 3. Tiled paper fibers inside 8-DIP margin
      const tile = this.ensureFiberTile(pixelScale);
      const seed = cardSeed(card);
      const tx = seed % 96;
      const ty = (seed >>> 8) % 96;
      ctx.save();
      ctx.beginPath();
      ctx.rect(x0 + 8, Hanging.paper_top + 8, w - 16, bottom - Hanging.paper_top - 16);
      ctx.clip();
      for (let ox = tx - 96; ox < x1 - 8; ox += 96) {
        for (let oy = ty - 96; oy < bottom - 8; oy += 96) {
          if (ox + 96 > x0 + 8 && oy + 96 > Hanging.paper_top + 8) {
            ctx.drawImage(tile, ox, oy, 96, 96);
          }
        }
      }
      ctx.restore();

      // 4. Paper edge hairline
      ctx.strokeStyle = rgbaCss(mixColor(paperRgb, skin.paper_edge, 0.18), 1);
      ctx.lineWidth = 1;
      ctx.strokeRect(x0 + 0.5, top + 0.5, w - 1, bottom - top - 1);

      // 5. Top bar clamp shadow fade (10 bands from y=42 to y=49, alpha 0.14 -> 0)
      this.drawVerticalFade(ctx, x0 + 1, Hanging.paper_top, x1 - 1, Hanging.paper_top + 7, 0x000000, 0.14, 0);

      // 6. Paper content area [-74, 53, 74, 178]
      const areaLeft = x0 + TextInset.side;
      const areaTop = Hanging.paper_top + TextInset.hanging_top;
      const areaRight = x1 - TextInset.side;
      const areaBottom = bottom - TextInset.hanging_bottom;

      ctx.save();
      ctx.beginPath();
      ctx.rect(x0 + 2, Hanging.paper_top, w - 4, bottom - Hanging.paper_top - 2);
      ctx.clip();

      this.drawCardContent(ctx, card, areaLeft, areaTop, areaRight, areaBottom, paperRgb);

      if (hovered && card.capabilities && w >= 120) {
        this.drawVerticalFade(ctx, x0 + 1, bottom - 26, x1 - 1, bottom - 1, paperRgb, 0, 0.96);
        ctx.fillStyle = rgbaCss(skin.muted, 1);
        ctx.font = '400 10px "Segoe UI", system-ui, sans-serif';
        const capabilityKeys = {Text: "capText", Image: "capImage", Files: "capFiles", Partial: "capPartial", "Content unavailable": "unavailable"};
        const caption = card.capabilities.split(" · ").map(c => capabilityKeys[c] ? t(capabilityKeys[c]) : c).join(" · ");
        this.drawSingleLineEllipsis(ctx, caption, x0 + 11, bottom - 9.5, w - 29, "left");
      }

      ctx.restore();

      // 7. Status indicator dot at bottom-right (x1 - 9, bottom - 9)
      this.drawStatusDot(ctx, card, x1 - 9, bottom - 9);
    }

    // Matches WorkspacePaint::draw_floating in src/rendering/workspace_paint.cpp (lines 1545-1603)
    drawFloatingCard(ctx, card, w, h, hoverPart, pixelScale, editing = false) {
      const paperRgb = card.paper_color || skin.warm_paper;
      const g = measureCardLayout(card);
      const toolbarH = 38;

      ctx.save();
      if (g.adaptive) {
        const bodyH = g.bottom - g.top;
        const fit = Math.max(0, Math.min(1, Math.min(w / g.body_width, (h - toolbarH) / bodyH)));
        const imgW = g.body_width * fit;
        const imgH = bodyH * fit;
        const imgX = (w - imgW) / 2;
        const imgY = toolbarH;

        const bmp = this.ensurePreviewBitmap(card, imgW, imgH, pixelScale);
        if (bmp) ctx.drawImage(bmp, imgX, imgY, imgW, imgH);

        ctx.strokeStyle = rgbaCss(skin.paper_edge, 0.35);
        ctx.lineWidth = 0.8;
        ctx.strokeRect(imgX + 0.4, imgY + 0.4, imgW - 0.8, imgH - 0.8);

        if (hoverPart === "body" && card.source && card.source.width && card.source.height) {
          const dimText = `${card.source.width} \u00d7 ${card.source.height} px`;
          ctx.save();
          ctx.font = '400 10px "Segoe UI", system-ui, sans-serif';
          const tw = ctx.measureText(dimText).width;
          const half = tw / 2 + 4;
          const bh = 15;
          this.fillRoundedRect(ctx, w / 2 - half, imgY + imgH - bh - 4, half * 2, bh, 2, rgbaCss(0x202020, 0.85));
          ctx.fillStyle = "#ffffff";
          ctx.textAlign = "center";
          ctx.textBaseline = "alphabetic";
          ctx.fillText(dimText, w / 2, imgY + imgH - 7.5);
          ctx.restore();
        }

        // Top toolbar background on adaptive image floating card
        ctx.fillStyle = rgbaCss(skin.warm_paper, 1);
        ctx.fillRect(0, 0, w, toolbarH);
        ctx.strokeStyle = rgbaCss(mixColor(paperRgb, skin.paper_edge, 0.24), 1);
        ctx.lineWidth = 1;
        ctx.strokeRect(0.5, 0.5, w - 1, toolbarH - 1);
      } else {
        // Standard Floating paper sheet (240 x 160 DIP default)
        ctx.fillStyle = rgbaCss(paperRgb, 1);
        ctx.fillRect(0, 0, w, h);

        // Paper fibers inside RectF(8, 46, w - 8, h - 8)
        if (w > 24 && h > 60) {
          const tile = this.ensureFiberTile(pixelScale);
          const seed = cardSeed(card);
          const tx = seed % 96;
          ctx.save();
          ctx.beginPath();
          ctx.rect(8, 46, w - 16, h - 54);
          ctx.clip();
          for (let ox = tx - 96; ox < w - 8; ox += 96) {
            for (let oy = 0; oy < h - 8; oy += 96) {
              if (ox + 96 > 8 && oy + 96 > 46) {
                ctx.drawImage(tile, ox, oy, 96, 96);
              }
            }
          }
          ctx.restore();
        }

        // Outer sheet border & toolbar divider line
        ctx.strokeStyle = rgbaCss(mixColor(paperRgb, skin.paper_edge, 0.24), 1);
        ctx.lineWidth = 1;
        ctx.strokeRect(0.5, 0.5, w - 1, h - 1);

        ctx.strokeStyle = rgbaCss(mixColor(paperRgb, skin.paper_edge, 0.12), 1);
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(10, 38.5);
        ctx.lineTo(w - 10, 38.5);
        ctx.stroke();

        // Red/brass pushpin at top center (lines 1571-1582 of workspace_paint.cpp)
        if (w >= 200) {
          const pinX = w / 2;
          const pinY = 13;
          ctx.fillStyle = rgbaCss(0x000000, 0.18);
          ctx.beginPath();
          ctx.arc(pinX + 1.4, pinY + 2.2, 5.2, 0, 2 * PI);
          ctx.fill();

          ctx.fillStyle = rgbaCss(0xb7473b, 1);
          ctx.beginPath();
          ctx.arc(pinX, pinY, 5.2, 0, 2 * PI);
          ctx.fill();

          ctx.fillStyle = rgbaCss(0xe58b7f, 0.9);
          ctx.beginPath();
          ctx.ellipse(pinX - 1.6, pinY - 1.7, 1.8, 1.6, 0, 0, 2 * PI);
          ctx.fill();

          ctx.strokeStyle = rgbaCss(0x7d2a22, 0.8);
          ctx.lineWidth = 0.8;
          ctx.beginPath();
          ctx.arc(pinX, pinY, 5.2, 0, 2 * PI);
          ctx.stroke();
        }
      }

      // Return pill button at top-left: RectF(8, 7, return_control_right(w), 31)
      const wide = w >= 200;
      const pillRight = wide ? 84 : 40;
      const pillW = pillRight - 8;
      const pillH = 24;
      const pillFill = mixColor(paperRgb, 0x8a6a40, hoverPart === "restore" ? 0.17 : 0.09);
      this.fillRoundedRect(ctx, 8, 7, pillW, pillH, 12, rgbaCss(pillFill, 1));
      this.strokeRoundedRect(
        ctx,
        8,
        7,
        pillW,
        pillH,
        12,
        rgbaCss(mixColor(paperRgb, skin.paper_edge, 0.24), 1),
        1
      );
      this.drawReturnIcon(ctx, { left: 16, top: 12.5, right: 28, bottom: 24.5 }, skin.label, 1.5);
      if (wide) {
        ctx.fillStyle = rgbaCss(skin.label, 1);
        ctx.font = '600 12px "Segoe UI", system-ui, sans-serif';
        ctx.textAlign = "left";
        ctx.textBaseline = "alphabetic";
        ctx.fillText(t("returnCard"), 32, 23.2);
      }

      // Top-right card kind icon or edit pencil: RectF(w - 52, 12.5, w - 39, 25.5)
      const typeIconBox = { left: w - 52, top: 12.5, right: w - 39, bottom: 25.5 };
      if (!editing && hoverPart && hoverPart !== "none" && (card.kind === "text" || card.kind === "link")) {
        this.drawPencil(ctx, typeIconBox);
      } else {
        const iconKind = card.image_primary ? "image" : card.kind === "rich" ? "text" : card.kind;
        this.drawCardIcon(ctx, iconKind, typeIconBox, skin.icon, 1.25);
      }

      // Top-right close button at Point2F(w - 19, 19) with chip = false
      this.drawCloseMark(ctx, w - 19, 19, hoverPart === "close", paperRgb, false);

      if (!g.adaptive) {
        if (!editing) {
          ctx.save();
          ctx.beginPath();
          ctx.rect(2, 40, w - 4, h - 42);
          ctx.clip();
          this.drawCardContent(
            ctx,
            card,
            TextInset.side,
            TextInset.floating_top,
            w - TextInset.side,
            h - TextInset.floating_bottom,
            paperRgb
          );
          ctx.restore();
        }
        this.drawStatusDot(ctx, card, w - 9, h - 9);
      }

      ctx.restore();
    }

    // Matches hanging() in src/rendering/workspace_paint.cpp (lines 1225-1277)
    drawHangingCard(ctx, card, slotIndex, capacity, g, hookX, contactY, angleDeg, hovered, hoverClose, pixelScale) {
      ctx.save();
      this.applyCardTransform(ctx, hookX, contactY, angleDeg);
      this.drawHookFront(ctx);
      if (g.adaptive) {
        this.drawImageBody(ctx, card, g, hovered, pixelScale);
      } else {
        this.drawPaperSheet(ctx, card, g, hovered, pixelScale);
      }
      this.drawWoodBar(ctx, card, slotIndex, capacity, g, hovered && (card.kind === "text" || card.kind === "link"));
      if (hovered) {
        const closeX = g.adaptive ? g.width / 2 - 38 : g.width / 2 - Hanging.close_size / 2;
        const closeY = g.adaptive ? g.bar_bottom - 12 : Hanging.paper_top + 15;
        this.drawCloseMark(ctx, closeX, closeY, hoverClose, skin.warm_paper);
      }
      ctx.restore();
    }
  }

  // Matches ImageCardLayout::floating_size in src/presentation/image_card_layout.hpp (lines 54-58)
  function floatingSizeForCard(card) {
    const g = measureCardLayout(card);
    if (g.adaptive) {
      return {
        width: Math.max(144, g.body_width),
        height: 38 + (g.bottom - g.top),
        adaptive: true,
        body_width: g.body_width,
        body_height: g.bottom - g.top,
      };
    }
    return {
      width: 240 * (card.scaleX || 1),
      height: 160 * (card.scaleY || 1),
      adaptive: false,
      body_width: 240,
      body_height: 160,
    };
  }

  global.ClipRopePaint = {
    Hanging,
    RopePlaque,
    RopeCurve,
    Swing,
    InsertionPreview,
    Viewport,
    WorkspacePainter,
    measureCardLayout,
    floatingSizeForCard,
    seedOf,
  };
})(window);
