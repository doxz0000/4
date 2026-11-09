// JUCE 9 interop — ES-модуль, тому ui.js теж завантажується як type="module"
import { getNativeFunction } from "./juce_interop.js";

// =====================================================================
// MODEL-CLIPONIZER — Web UI logic
//
// Комунікація з C++:
//   JS -> C++ :  callNative("setParam", id, value)
//   C++ -> JS :  window.__pluginUpdate(state)   (викликається ~30 Гц)
// =====================================================================

// ---------------------------------------------------------------------
// Native bridge helper. Якщо у вашій версії JUCE точка входу
// window.__JUCE__.backend.invoke() називається інакше — правте лише тут.
// ---------------------------------------------------------------------
function callNative (name, ...args) {
  try {
    // JUCE 9 API: функція getNativeFunction() з juce_interop.js.
    // Повертаємо справжній Promise з результатом від C++.
    if (typeof getNativeFunction === "function") {
      return getNativeFunction(name)(...args);
    }
  } catch (e) { /* ignore - running outside JUCE (e.g. plain browser preview) */ }
  return Promise.resolve(null);
}

// Те саме, але з таймаутом: поза JUCE (перегляд у браузері) Promise
// від getNativeFunction ніколи не завершиться — тоді повертаємо null.
function callNativeWithTimeout (ms, name, ...args) {
  return Promise.race([
    callNative(name, ...args),
    new Promise((resolve) => setTimeout(() => resolve(null), ms)),
  ]).catch(() => null);
}

// ---------------------------------------------------------------------
// DSP helpers — точна копія ClipShaper::process() з PluginProcessor.h
// ---------------------------------------------------------------------
function dbToGain (db) { return Math.pow(10, db / 20); }
function gainToDb (g)  { return 20 * Math.log10(Math.max(1e-6, Math.abs(g))); }

function clipShaperProcess (x, thresholdLin, knee01) {
  const sign = x < 0 ? -1 : 1;
  const absX = Math.abs(x);

  if (knee01 < 0.001) {
    if (absX <= thresholdLin) return [x, 0];
    return [sign * thresholdLin, 1];
  }

  // Коліно відносне до порогу (як у C++): стеля завжди = thresholdLin
  const kneeWidth = Math.max(1e-6, Math.min(1, knee01) * thresholdLin);
  const kneeStart = thresholdLin - kneeWidth;

  if (absX <= kneeStart) return [x, 0];

  const overshoot = absX - kneeStart;
  const shaped = kneeWidth * Math.tanh(overshoot / kneeWidth);
  const outAbs = kneeStart + shaped;
  const clipAmount = Math.min(1, Math.max(0, shaped / kneeWidth));
  return [sign * outAbs, clipAmount];
}

// Static curve of Saturator::shape() з PluginProcessor.h (без ADAA/DC-blocker)
function saturatorProcess (x, thresholdLin, amount01) {
  const amt = Math.min(1, Math.max(0, amount01));
  const a = 3.0 * Math.pow(amt, 1.3);
  if (a < 1e-4) return x;
  const b = (0.06 * amt) / a;
  const tab = Math.tanh(a * b);
  const dPos = Math.tanh(a * (1 + b)) - tab;
  const dNeg = tab - Math.tanh(a * (b - 1));
  const invD = 1 / Math.max(1e-12, Math.max(dPos, dNeg));
  const T = Math.max(1e-6, thresholdLin);
  const u = x / T;
  return T * (Math.tanh(a * (u + b)) - tab) * invD;
}

// Повна крива: SATURATOR -> CLIPPER
function chainProcess (x, thresholdLin, knee01, sat01) {
  return clipShaperProcess(saturatorProcess(x, thresholdLin, sat01), thresholdLin, knee01);
}

// Початок коліна (лінійно) — те саме правило, що в C++ і scopeGeometry
function kneeStartFor (thresholdLin, knee01) {
  if (knee01 < 0.001) return thresholdLin;
  return thresholdLin - Math.max(1e-6, Math.min(1, knee01) * thresholdLin);
}

// Зона для вхідного рівня (по сигналу на вході кліпера, як scopeClipAmount у C++):
// 0 = зелена (лінійно), 1 = м'яке коліно, 2 = кліпінг (за порогом)
function zoneFor (amp, thresholdLin, knee01, sat01) {
  const s = Math.abs(saturatorProcess(amp, thresholdLin, sat01));
  if (s > thresholdLin) return 2;
  if (s > kneeStartFor(thresholdLin, knee01)) return 1;
  return 0;
}

// ---------------------------------------------------------------------
// Global UI state
// ---------------------------------------------------------------------
const paramDescriptors = {};   // id -> {min,max,step,label}
let latestState = null;        // last payload from window.__pluginUpdate
let verticalZoom = 1.0;
let barsOnScreen = 1.0;

let liveDotPosSmoothed = 0;
let liveDotNegSmoothed = 0;
let clipLedSmoothed = 0;

// id параметра, який зараз перетягують (щоб 30 Гц push не смикав ручку назад)
let activeDragParam = null;

// ---------------------------------------------------------------------
// KNOBS
// ---------------------------------------------------------------------
const ROTARY_START = Math.PI * 1.2;
const ROTARY_END   = Math.PI * 2.8;

function normFor (id, value) {
  const d = paramDescriptors[id];
  if (!d) return 0;
  return Math.min(1, Math.max(0, (value - d.min) / (d.max - d.min)));
}

function setKnobVisual (id, value) {
  const d = paramDescriptors[id];
  if (!d) return;

  const norm = normFor(id, value);
  const angle = ROTARY_START + norm * (ROTARY_END - ROTARY_START);
  const deg = angle * 180 / Math.PI;

  const img = document.querySelector(`#knob${capitalize(id)} .knobImg`);
  if (img) img.style.transform = `rotate(${deg}deg)`;

  const label = document.getElementById(`val${capitalize(id)}`);
  if (label) {
    const unit = d.label || "";
    label.textContent = `${value.toFixed(1)} ${unit}`.trim();
  }
}

function capitalize (s) { return s.charAt(0).toUpperCase() + s.slice(1); }

function setupKnobDragging (knobEl, paramId) {
  let dragging = false;
  let startY = 0;
  let startValue = 0;

  const getDescriptor = () => paramDescriptors[paramId];

  knobEl.addEventListener("mousedown", (e) => {
    const d = getDescriptor();
    if (!d) return;

    dragging = true;
    activeDragParam = paramId;
    startY = e.clientY;
    startValue = d.currentValue !== undefined ? d.currentValue : d.value;

    callNative("beginGesture", paramId);
    document.body.classList.add("cursor-hidden");
    e.preventDefault();
  });

  window.addEventListener("mousemove", (e) => {
    if (!dragging) return;
    const d = getDescriptor();
    if (!d) return;

    const dragPixels = 220; // повний хід ручки = 220px вертикального руху
    const deltaNorm = (startY - e.clientY) / dragPixels;
    const range = d.max - d.min;

    let newValue = startValue + deltaNorm * range;
    newValue = Math.min(d.max, Math.max(d.min, newValue));

    d.currentValue = newValue;
    setKnobVisual(paramId, newValue);
    callNative("setParam", paramId, newValue);
  });

  window.addEventListener("mouseup", () => {
    if (!dragging) return;
    dragging = false;
    if (activeDragParam === paramId) activeDragParam = null;
    document.body.classList.remove("cursor-hidden");
    callNative("endGesture", paramId);
  });

  // Страховка: якщо вікно втратить фокус під час перетягування,
  // курсор повертається, щоб не залишився прихованим назавжди.
  const restoreCursor = () => document.body.classList.remove("cursor-hidden");
  window.addEventListener("blur", restoreCursor);

  // Double-click resets to default (matches old setDoubleClickReturnValue)
  knobEl.addEventListener("dblclick", () => {
    const d = getDescriptor();
    if (!d) return;
    const defaultValue = paramId === "threshold" ? 0.0
                        : paramId === "softness"  ? 20.0
                        : 0.0;
    d.currentValue = defaultValue;
    setKnobVisual(paramId, defaultValue);
    callNative("beginGesture", paramId);
    callNative("setParam", paramId, defaultValue);
    callNative("endGesture", paramId);
  });
}

// ---------------------------------------------------------------------
// DELTA BUTTON
// ---------------------------------------------------------------------
let deltaState = false;

function setupDeltaButton () {
  const btn = document.getElementById("deltaButton");
  const img = document.getElementById("deltaImg");

  btn.addEventListener("click", () => {
    deltaState = !deltaState;
    img.src = deltaState ? "Resources/DeltaOn.png" : "Resources/DeltaOff.png";
    callNative("beginGesture", "delta");
    callNative("setParam", "delta", deltaState);
    callNative("endGesture", "delta");
  });
}

// ---------------------------------------------------------------------
// TIME DIVISION BUTTONS
// ---------------------------------------------------------------------
function setupTimeButtons () {
  document.querySelectorAll(".barBtn").forEach((btn) => {
    btn.addEventListener("click", () => {
      document.querySelectorAll(".barBtn").forEach((b) => b.classList.remove("active"));
      btn.classList.add("active");
      const bars = parseFloat(btn.dataset.bars);
      barsOnScreen = bars;
      callNative("setTimeDivision", bars);
    });
  });
}

// ---------------------------------------------------------------------
// ZOOM SLIDER (pure client-side, like the original verticalZoomSlider)
// ---------------------------------------------------------------------
function setupZoomSlider () {
  const slider = document.getElementById("zoomSlider");
  const label = document.getElementById("zoomValueLabel");

  const DEFAULT_ZOOM = 1.0;

  const applyZoom = () => {
    verticalZoom = parseFloat(slider.value);
    label.textContent = verticalZoom.toFixed(2) + "x";
    requestRender();
  };

  slider.addEventListener("input", applyZoom);

  // Подвійний клік — скидання масштабу до 1x
  slider.addEventListener("dblclick", () => {
    slider.value = DEFAULT_ZOOM;
    applyZoom();
  });
}

// ---------------------------------------------------------------------
// COLORS — читаються з CSS один раз в init(), а не кожен кадр
// ---------------------------------------------------------------------
const COLORS = {
  grid:   "#2e3a2e",
  normal: "#8fe3a0",
  soft:   "#ffb347",
  hard:   "#ff4d4d",
  thresh: "#e0d9c0",
  zeroDb: "#ff3b3b",
};
const LABEL_COLOR = "rgba(232,195,122,0.55)";
const DB_MARKS = [-24, -12, -6, 0, 6];

function readColors () {
  const style = getComputedStyle(document.documentElement);
  const get = (name, fallback) => style.getPropertyValue(name).trim() || fallback;
  COLORS.grid   = get("--scopeGrid",     COLORS.grid);
  COLORS.normal = get("--traceNormal",   COLORS.normal);
  COLORS.soft   = get("--traceSoftClip", COLORS.soft);
  COLORS.hard   = get("--traceHardClip", COLORS.hard);
  COLORS.thresh = get("--thresholdLine", COLORS.thresh);
  COLORS.zeroDb = get("--zeroDbLine",    COLORS.zeroDb);
}

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const crisp = (v) => Math.round(v) + 0.5;   // 1px лінії без розмиття

function hexWithAlpha (colorStr, alpha) {
  if (colorStr.startsWith("#") && colorStr.length === 7) {
    const r = parseInt(colorStr.substr(1, 2), 16);
    const g = parseInt(colorStr.substr(3, 2), 16);
    const b = parseInt(colorStr.substr(5, 2), 16);
    return `rgba(${r},${g},${b},${alpha})`;
  }
  return colorStr;
}

function hexToRgba (hex, alpha) { return hexWithAlpha(hex, alpha); }

function verticalRange () {
  return Math.max(0.05, 2.0 / Math.max(0.1, verticalZoom));
}

// ---------------------------------------------------------------------
// CANVAS SETUP (devicePixelRatio + resizing). Розмір кешується у view,
// щоб не читати clientWidth (layout) кожен кадр.
// ---------------------------------------------------------------------
function setupCanvas (canvas) {
  const view = { canvas, ctx: canvas.getContext("2d"), w: 1, h: 1, dpr: 1, sizeKey: "" };

  const resize = () => {
    const rect = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    view.w = Math.max(1, rect.width);
    view.h = Math.max(1, rect.height);
    view.dpr = dpr;
    canvas.width = Math.max(1, Math.round(view.w * dpr));
    canvas.height = Math.max(1, Math.round(view.h * dpr));
    view.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    view.sizeKey = `${canvas.width}x${canvas.height}`;
    requestRender();
  };

  new ResizeObserver(resize).observe(canvas);
  resize();
  return view;
}

// Offscreen-шар того ж розміру, що й view. Повторно використовує
// існуючий canvas, якщо розмір не змінився (без зайвих алокацій).
function ensureLayer (layer, view) {
  if (!layer || layer.canvas.width !== view.canvas.width || layer.canvas.height !== view.canvas.height) {
    const c = document.createElement("canvas");
    c.width = view.canvas.width;
    c.height = view.canvas.height;
    layer = { canvas: c, ctx: c.getContext("2d") };
  } else {
    layer.ctx.setTransform(1, 0, 0, 1, 0, 0);
    layer.ctx.clearRect(0, 0, layer.canvas.width, layer.canvas.height);
  }
  layer.ctx.setTransform(view.dpr, 0, 0, view.dpr, 0, 0);
  return layer;
}

function blit (view, layer) {
  view.ctx.drawImage(layer.canvas, 0, 0, view.w, view.h);
}

function drawDbLabels (ctx, w, h, ampToY) {
  ctx.fillStyle = LABEL_COLOR;
  ctx.font = "10px Consolas, monospace";
  ctx.textAlign = "right";
  DB_MARKS.forEach((db) => {
    const y = ampToY(dbToGain(db));
    if (y >= 0 && y <= h) ctx.fillText((db > 0 ? "+" : "") + db.toFixed(0) + " dB", w - 4, y - 2);
  });
}

function drawThresholdLines (ctx, w, ampToY, thresholdDb, thresholdLin, lineWidth, labelOverride) {
  ctx.strokeStyle = COLORS.thresh;
  ctx.setLineDash([5, 4]);
  ctx.lineWidth = lineWidth;
  ctx.beginPath();
  [thresholdLin, -thresholdLin].forEach((v) => {
    const y = crisp(ampToY(v));
    ctx.moveTo(0, y); ctx.lineTo(w, y);
  });
  ctx.stroke();
  ctx.setLineDash([]);

  ctx.fillStyle = COLORS.thresh;
  ctx.font = "bold 12px Consolas, monospace";
  ctx.textAlign = "left";
  const label = labelOverride
    || (thresholdDb > 0 ? "THRESHOLD +" : "THRESHOLD ") + thresholdDb.toFixed(1) + " dB";
  ctx.fillText(label, 2, ampToY(thresholdLin) - 4);
}

let shaperView, scopeView;

// ---------------------------------------------------------------------
// RENDER: CLIP SHAPER PANEL
//   Сітка + крива + пороги кешуються в offscreen-шар і перебудовуються
//   лише при зміні threshold/softness/saturator/zoom/розміру.
//   У звичайному кадрі: 1 drawImage + 2 точки.
// ---------------------------------------------------------------------
let shaperLayer = null;
let shaperKey = "";

function zoneColor (zone) {
  return zone === 0 ? COLORS.normal : zone === 1 ? COLORS.soft : COLORS.hard;
}

function buildShaperLayer (view, thresholdDb, knee01, sat01, outGain) {
  shaperLayer = ensureLayer(shaperLayer, view);
  const ctx = shaperLayer.ctx;
  const w = view.w, h = view.h;
  const range = verticalRange();
  const thresholdLin = dbToGain(thresholdDb);
  const cx = w / 2, cy = h / 2;
  const ampToY = (amp) => cy - Math.min(1, Math.max(-1, amp / range)) * (h / 2);
  const ampToX = (amp) => cx + Math.min(1, Math.max(-1, amp / range)) * (w / 2);

  // grid — один шлях, один stroke
  ctx.strokeStyle = COLORS.grid;
  ctx.lineWidth = 1;
  ctx.beginPath();
  DB_MARKS.forEach((db) => {
    const g = dbToGain(db);
    [g, -g].forEach((v) => {
      const x = crisp(ampToX(v)), y = crisp(ampToY(v));
      ctx.moveTo(x, 0); ctx.lineTo(x, h);
      // горизонтальну 0 dB малює червоним under-шар — тут не перекриваємо
      if (db !== 0) { ctx.moveTo(0, y); ctx.lineTo(w, y); }
    });
  });
  ctx.stroke();
  // dB-підписи та пороги малює шар хвилі "після" (outUnder/outOver),
  // тут вони не потрібні — інакше дублювались би.

  // unity diagonal
  ctx.strokeStyle = hexWithAlpha(COLORS.normal, 0.22);
  ctx.beginPath(); ctx.moveTo(0, h); ctx.lineTo(w, 0); ctx.stroke();

  // transfer curve — один Path2D, колір зон через горизонтальний
  // градієнт з жорсткими стопами (замість 300 окремих відрізків)
  const steps = Math.max(96, Math.round(w / 2));
  const path = new Path2D();
  const grad = ctx.createLinearGradient(0, 0, w, 0);
  let prevZone = 0, prevX = 0;

  for (let i = 0; i <= steps; i++) {
    const xin = -range + (i / steps) * 2 * range;
    const [out] = chainProcess(xin, thresholdLin, knee01, sat01);
    const x = ampToX(xin), y = ampToY(out * outGain);
    const zone = zoneFor(xin, thresholdLin, knee01, sat01);

    if (i === 0) {
      path.moveTo(x, y);
      grad.addColorStop(0, zoneColor(zone));
    } else {
      path.lineTo(x, y);
      if (zone !== prevZone) {
        const t = clamp01((prevX + x) / 2 / w);
        grad.addColorStop(t, zoneColor(prevZone));
        grad.addColorStop(t, zoneColor(zone));
      }
    }
    prevZone = zone;
    prevX = x;
  }
  grad.addColorStop(1, zoneColor(prevZone));

  ctx.lineJoin = "round";
  ctx.lineCap = "round";
  ctx.strokeStyle = grad;
  // м'яке світіння: широкі напівпрозорі проходи (без дорогого shadowBlur)
  ctx.globalAlpha = 0.14; ctx.lineWidth = 8;   ctx.stroke(path);
  ctx.globalAlpha = 0.28; ctx.lineWidth = 4;   ctx.stroke(path);
  ctx.globalAlpha = 1.0;  ctx.lineWidth = 2.2; ctx.stroke(path);
}

// Шари хвилі "ПІСЛЯ" (вихід сатуратора + кліпера) на панелі Clip Shaper
let outUnder = null, outOver = null;
let outUnderKey = "", outGeomKey = "";
let outFillGrad = null, outLineGrad = null;
const SHAPER_CURVE_ALPHA = 0.45;   // прозорість передавальної кривої поверх хвилі

function renderShaper (state) {
  const view = shaperView;
  const ctx = view.ctx;
  const w = view.w, h = view.h;

  const thresholdDb = state.params.threshold;
  const thresholdLin = dbToGain(thresholdDb);
  const knee01 = clamp01(state.params.softness / 100);
  const sat01 = clamp01((state.params.saturator || 0) / 100);

  // OUTPUT gain застосовується після кліпера (у Delta-режимі — ні),
  // тож уся панель "ПІСЛЯ" масштабується на нього.
  const outDb = state.params.delta ? 0 : (Number(state.params.output) || 0);
  const outGain = dbToGain(outDb);

  const key = `${thresholdDb}|${knee01}|${sat01}|${outGain}|${verticalZoom}|${view.sizeKey}`;
  if (key !== shaperKey || !shaperLayer) {
    buildShaperLayer(view, thresholdDb, knee01, sat01, outGain);
    shaperKey = key;
  }

  // --- вихідна хвиля в часі ---
  const wf = state.waveform || {};
  const n = wf.n || 600;
  const baseGeo = scopeGeometry(state, h);
  // Хвиля, зони та пунктири масштабуються на outGain; dB-сітка — ні.
  const geo = { ...baseGeo, ampToY: (a) => baseGeo.ampToY(a * outGain) };
  const geomKey = `${geo.thresholdDb}|${state.params.softness}|${outGain}|${verticalZoom}|${view.sizeKey}`;
  const deltaOn = !!state.params.delta;
  const underKey = `${geomKey}|${barsOnScreen}|${state.qnPerBar}|${wf.prevId}|${deltaOn}`;

  if (geomKey !== outGeomKey || !outOver) {
    let label = null;
    if (Math.abs(outDb) > 0.005) {
      const ceilDb = geo.thresholdDb + outDb;
      label = (ceilDb > 0 ? "CEILING +" : "CEILING ") + ceilDb.toFixed(1) + " dB";
    }
    outOver = buildOverLayer(outOver, view, geo, label);
    outFillGrad = makeZoneGradient(ctx, h, geo, 0.30);
    outLineGrad = makeZoneGradient(ctx, h, geo, 1.0);
    outGeomKey = geomKey;
  }
  if (underKey !== outUnderKey || !outUnder) {
    outUnder = buildUnderLayer(outUnder, view, state, geo, n, wf.outPrevious, baseGeo.ampToY);
    outUnderKey = underKey;
  }

  ctx.clearRect(0, 0, w, h);
  blit(view, outUnder);
  drawWave(ctx, wf.outCurrent, n, w, geo.ampToY, outFillGrad, outLineGrad, 1.2);
  drawPlayhead(ctx, wf.outCurrent, wf.progress, w, h);

  // --- передавальна крива поверх, напівпрозоро ---
  // У Delta-режимі крива й live-точки не стосуються дельти — не малюємо.
  if (!deltaOn) {
    ctx.globalAlpha = SHAPER_CURVE_ALPHA;
    blit(view, shaperLayer);
    ctx.globalAlpha = 1.0;
  }

  blit(view, outOver);

  if (deltaOn) return;

  // live dots
  liveDotPosSmoothed += ((Number(state.livePeakPos) || 0) - liveDotPosSmoothed) * 0.5;
  liveDotNegSmoothed += ((Number(state.livePeakNeg) || 0) - liveDotNegSmoothed) * 0.5;

  const range = verticalRange();
  const cx = w / 2, cy = h / 2;
  const drawDot = (amp) => {
    const [outAmp] = chainProcess(amp, thresholdLin, knee01, sat01);
    const x = cx + Math.min(1, Math.max(-1, amp / range)) * (w / 2);
    const y = cy - Math.min(1, Math.max(-1, outAmp * outGain / range)) * (h / 2);
    const col = zoneColor(zoneFor(amp, thresholdLin, knee01, sat01));

    ctx.fillStyle = col;
    ctx.globalAlpha = 0.22;
    ctx.beginPath(); ctx.arc(x, y, 10, 0, Math.PI * 2); ctx.fill();
    ctx.globalAlpha = 1.0;
    ctx.beginPath(); ctx.arc(x, y, 4.5, 0, Math.PI * 2); ctx.fill();
    ctx.strokeStyle = "rgba(255,255,255,0.6)";
    ctx.lineWidth = 1;
    ctx.stroke();
  };
  drawDot(liveDotPosSmoothed);
  drawDot(liveDotNegSmoothed);
}

// ---------------------------------------------------------------------
// RENDER: OSCILLOSCOPE PANEL
//   under-шар: сітки + бліда попередня хвиля (перебудова раз на такт)
//   over-шар : пунктири threshold/knee + підпис
//   кадр     : 2 drawImage + 1 заповнена згладжена фігура поточної хвилі
// ---------------------------------------------------------------------
let scopeUnder = null, scopeOver = null;
let scopeUnderKey = "", scopeGeomKey = "";
let scopeFillGrad = null, scopeLineGrad = null;

let waveXs = new Float32Array(0);
let waveTop = new Float32Array(0);
let waveBot = new Float32Array(0);

function ensureWaveBuffers (count) {
  if (waveXs.length < count) {
    waveXs = new Float32Array(count);
    waveTop = new Float32Array(count);
    waveBot = new Float32Array(count);
  }
}

// Прямі відрізки між колонками: на 600 точках виглядає так само, як
// quadraticCurveTo, але растеризується помітно дешевше.
function smoothTo (path, xs, ys, from, to, dir, move) {
  if (move) path.moveTo(xs[from], ys[from]); else path.lineTo(xs[from], ys[from]);
  for (let i = from + dir; i !== to + dir; i += dir) path.lineTo(xs[i], ys[i]);
}

// Вертикальний градієнт зон: hard (за порогом) -> soft (knee) -> normal.
function makeZoneGradient (ctx, h, geo, alpha) {
  const f = (amp) => clamp01(geo.ampToY(amp) / h);
  const tP = f(geo.thresholdLin), kP = f(geo.kneeStart);
  const kN = f(-geo.kneeStart),   tN = f(-geo.thresholdLin);
  const hard = hexWithAlpha(COLORS.hard, alpha);
  const soft = hexWithAlpha(COLORS.soft, alpha);
  const norm = hexWithAlpha(COLORS.normal, alpha);

  const g = ctx.createLinearGradient(0, 0, 0, h);
  g.addColorStop(0, hard);  g.addColorStop(tP, hard);
  g.addColorStop(tP, soft); g.addColorStop(kP, soft);
  g.addColorStop(kP, norm); g.addColorStop(kN, norm);
  g.addColorStop(kN, soft); g.addColorStop(tN, soft);
  g.addColorStop(tN, hard); g.addColorStop(1, hard);
  return g;
}

// arr — плаский масив цілих [min,max,...] x1000 (з C++), n — повна к-сть бакетів
function drawWave (ctx, arr, n, w, ampToY, fillStyle, strokeStyle, lineWidth) {
  if (!arr || n <= 0) return;
  const count = Math.min(n, arr.length >> 1);
  if (count < 2) return;

  ensureWaveBuffers(count);
  const step = w / n;
  for (let i = 0; i < count; i++) {
    waveXs[i]  = (i + 0.5) * step;
    waveBot[i] = ampToY(arr[2 * i] * 0.001);
    waveTop[i] = ampToY(arr[2 * i + 1] * 0.001);
  }

  const body = new Path2D();
  smoothTo(body, waveXs, waveTop, 0, count - 1, 1, true);
  smoothTo(body, waveXs, waveBot, count - 1, 0, -1, false);
  body.closePath();
  ctx.fillStyle = fillStyle;
  ctx.fill(body);

  const edges = new Path2D();
  smoothTo(edges, waveXs, waveTop, 0, count - 1, 1, true);
  smoothTo(edges, waveXs, waveBot, 0, count - 1, 1, true);
  ctx.lineJoin = "round";
  ctx.lineCap = "round";
  ctx.lineWidth = lineWidth;
  ctx.strokeStyle = strokeStyle;
  ctx.stroke(edges);
}

function scopeGeometry (state, h) {
  const thresholdDb = state.params.threshold;
  const thresholdLin = dbToGain(thresholdDb);
  const knee01 = clamp01(state.params.softness / 100);
  const kneeStart = kneeStartFor(thresholdLin, knee01);
  const kneeWidth = thresholdLin - kneeStart;
  const range = verticalRange();
  const cy = h / 2;
  const ampToY = (amp) => cy - Math.min(1, Math.max(-1, amp / range)) * cy;
  return { thresholdDb, thresholdLin, kneeWidth, kneeStart, ampToY };
}

// Спільний under-шар для обох панелей: сітки + бліда попередня хвиля.
function buildUnderLayer (layer, view, state, geo, n, previousArr, gridAmpToY) {
  layer = ensureLayer(layer, view);
  const ctx = layer.ctx;
  const w = view.w, h = view.h;
  const gridY = gridAmpToY || geo.ampToY;

  // dB grid
  ctx.strokeStyle = COLORS.grid;
  ctx.lineWidth = 1;
  ctx.beginPath();
  DB_MARKS.forEach((db) => {
    if (db === 0) return;
    const y = gridY(dbToGain(db));
    if (y >= 0 && y <= h) { ctx.moveTo(0, crisp(y)); ctx.lineTo(w, crisp(y)); }
  });
  ctx.stroke();

  // 0 dBFS — червона лінія (+0 і −0 dB)
  ctx.strokeStyle = COLORS.zeroDb;
  ctx.beginPath();
  [1, -1].forEach((v) => {
    const y = gridY(v);
    if (y >= 0 && y <= h) { ctx.moveTo(0, crisp(y)); ctx.lineTo(w, crisp(y)); }
  });
  ctx.stroke();

  drawDbLabels(ctx, w, h, gridY);

  // musical grid
  const qnPerBar = state.qnPerBar;
  const quarterLineCount = Math.max(1, Math.ceil(qnPerBar * barsOnScreen));
  const beats = new Path2D(), bars = new Path2D();
  for (let q = 0; q <= quarterLineCount; q++) {
    const x = crisp((q / quarterLineCount) * w);
    const target = (q % qnPerBar) < 0.001 ? bars : beats;
    target.moveTo(x, 0); target.lineTo(x, h);
  }
  ctx.strokeStyle = COLORS.grid;               ctx.stroke(beats);
  ctx.strokeStyle = "rgba(232,195,122,0.28)";  ctx.stroke(bars);

  // previous (faded) waveform
  drawWave(ctx, previousArr, n, w, geo.ampToY,
           makeZoneGradient(ctx, h, geo, 0.13),
           makeZoneGradient(ctx, h, geo, 0.32), 1.0);
  return layer;
}

// Спільний over-шар: пунктири knee + threshold з підписом.
function buildOverLayer (layer, view, geo, labelOverride) {
  layer = ensureLayer(layer, view);
  const ctx = layer.ctx;
  const w = view.w;

  // knee lines
  if (geo.kneeWidth > 0 && geo.kneeStart < geo.thresholdLin) {
    ctx.strokeStyle = hexWithAlpha(COLORS.soft, 0.45);
    ctx.setLineDash([3, 5]);
    ctx.lineWidth = 1.0;
    ctx.beginPath();
    [geo.kneeStart, -geo.kneeStart].forEach((v) => {
      const y = crisp(geo.ampToY(v));
      ctx.moveTo(0, y); ctx.lineTo(w, y);
    });
    ctx.stroke();
    ctx.setLineDash([]);
  }

  drawThresholdLines(ctx, w, geo.ampToY, geo.thresholdDb, geo.thresholdLin, 1.5, labelOverride);
  return layer;
}

// "Голівка" запису: м'який шлейф + тонка лінія
function drawPlayhead (ctx, currentArr, progressRaw, w, h) {
  const progress = clamp01(progressRaw || 0);
  if (!currentArr || currentArr.length === 0 || progress >= 1) return;
  const px = progress * w;
  const trail = ctx.createLinearGradient(px - 28, 0, px, 0);
  trail.addColorStop(0, hexWithAlpha(COLORS.thresh, 0));
  trail.addColorStop(1, hexWithAlpha(COLORS.thresh, 0.10));
  ctx.fillStyle = trail;
  ctx.fillRect(px - 28, 0, 28, h);
  ctx.fillStyle = hexWithAlpha(COLORS.thresh, 0.45);
  ctx.fillRect(px - 0.5, 0, 1, h);
}

function renderScope (state) {
  const view = scopeView;
  const ctx = view.ctx;
  const w = view.w, h = view.h;
  const wf = state.waveform || {};
  const n = wf.n || 600;

  const geo = scopeGeometry(state, h);
  const geomKey = `${geo.thresholdDb}|${state.params.softness}|${verticalZoom}|${view.sizeKey}`;
  const underKey = `${geomKey}|${barsOnScreen}|${state.qnPerBar}|${wf.prevId}`;

  if (geomKey !== scopeGeomKey || !scopeOver) {
    scopeOver = buildOverLayer(scopeOver, view, geo);
    scopeFillGrad = makeZoneGradient(ctx, h, geo, 0.30);
    scopeLineGrad = makeZoneGradient(ctx, h, geo, 1.0);
    scopeGeomKey = geomKey;
  }
  if (underKey !== scopeUnderKey || !scopeUnder) {
    scopeUnder = buildUnderLayer(scopeUnder, view, state, geo, n, wf.previous);
    scopeUnderKey = underKey;
  }

  ctx.clearRect(0, 0, w, h);
  blit(view, scopeUnder);

  drawWave(ctx, wf.current, n, w, geo.ampToY, scopeFillGrad, scopeLineGrad, 1.2);
  drawPlayhead(ctx, wf.current, wf.progress, w, h);

  blit(view, scopeOver);
}

// ---------------------------------------------------------------------
// RENDER: CLIP LED + BPM + PROGRESS TEXT (DOM чіпаємо лише при змінах)
// ---------------------------------------------------------------------
let lastLedKey = "";
let lastBpmText = "";
let lastProgressText = "";

function renderMeta (state) {
  // state.clipLevel = пікове пониження рівня кліпером у dB (0 = без кліпу).
  // Атака/утримання/спад уже зроблені в процесорі — тут без додаткового
  // згладжування, щоб лампа реагувала одразу.
  const db = Math.max(0, Number(state.clipLevel) || 0);
  clipLedSmoothed = db;

  let color;
  if (db < 0.05)     color = "#1f7a1f";   // кліпу немає
  else if (db < 1)   color = "#7fd11a";   // ледь торкається
  else if (db < 3)   color = "#e6d21a";   // помірний кліп
  else if (db < 6)   color = "#ff8a1a";   // сильний
  else               color = "#ff2e2e";   // дуже сильний

  // Яскравість світіння: 0 dB -> 0, 9+ dB -> 1
  const ledLevel = db < 0.05 ? 0 : Math.round(Math.min(1, 0.25 + db / 12) * 40) / 40;
  const ledKey = color + ledLevel;
  if (ledKey !== lastLedKey) {
    lastLedKey = ledKey;
    const led = document.getElementById("clipLed");
    const glow = document.getElementById("clipGlow");
    led.style.background = color;
    led.style.boxShadow = `0 0 ${6 + ledLevel * 14}px ${color}`;
    glow.style.background = `radial-gradient(circle, ${hexToRgba(color, 0.6 * ledLevel)} 0%, ${hexToRgba(color, 0)} 70%)`;
  }

  const bpmText = "BPM: " + Math.round(state.bpm);
  if (bpmText !== lastBpmText) {
    lastBpmText = bpmText;
    document.getElementById("bpmLabel").textContent = bpmText;
  }

  const wf = state.waveform || {};
  const pct = wf.current && wf.current.length > 0 ? Math.round(wf.progress * 100) : 0;
  const progressText = `${barsOnScreen.toFixed(2)} BAR   ${pct}%`;
  if (progressText !== lastProgressText) {
    lastProgressText = progressText;
    document.getElementById("scopeProgress").textContent = progressText;
  }
}

// ---------------------------------------------------------------------
// RENDER: KNOBS + DELTA (reflect backend-confirmed values)
// ---------------------------------------------------------------------
function renderControls (state) {
  ["input", "threshold", "softness", "output", "saturator"].forEach((id) => {
    const d = paramDescriptors[id];
    if (!d || state.params[id] === undefined) return;
    // Під час перетягування ручки не перекреслюємо локальне значення
    // значенням, яке ще не підтвердив хост (інакше ручка смикається).
    if (activeDragParam === id) return;
    if (d.currentValue === state.params[id] && d.shown) return;   // без зайвих DOM-змін
    d.currentValue = state.params[id];
    d.shown = true;
    setKnobVisual(id, state.params[id]);
  });

  if (state.params.delta !== deltaState) {
    deltaState = state.params.delta;
    document.getElementById("deltaImg").src = deltaState ? "Resources/DeltaOn.png" : "Resources/DeltaOff.png";
  }

  // Заголовок панелі "ПІСЛЯ": у Delta-режимі там показується дельта.
  const titleDelta = !!state.params.delta;
  if (titleDelta !== shaperTitleDelta) {
    shaperTitleDelta = titleDelta;
    const t = document.getElementById("shaperTitle");
    if (t) t.textContent = titleDelta ? "AFTER · DELTA" : "AFTER · CLIP SHAPER";
  }
}
let shaperTitleDelta = false;

// ---------------------------------------------------------------------
// MAIN RENDER — не частіше одного разу на кадр дисплея (rAF).
// Якщо вікно плагіна сховане, rAF не викликається -> навантаження ~0.
// ---------------------------------------------------------------------
let renderPending = false;

function requestRender () {
  if (renderPending) return;
  renderPending = true;
  requestAnimationFrame(() => {
    renderPending = false;
    render();
  });
}

function render () {
  if (!latestState || !shaperView || !scopeView) return;
  renderControls(latestState);
  renderShaper(latestState);
  renderScope(latestState);
  renderMeta(latestState);
}

// ---------------------------------------------------------------------
// ENTRY POINT — called by C++ (~30 Гц, порожні тіки пропускаються).
// "previous" приходить лише коли змінився (новий prevId) — інакше
// беремо збережений з минулого пакета.
// "current"/"outCurrent" приходять ДЕЛЬТОЮ: колонки починаючи з wf.from;
// усе до from уже є в curArr/outCurArr.
// ---------------------------------------------------------------------
let curArr = [];
let outCurArr = [];

function applyDelta (stored, from, part) {
  const start = Math.min(stored.length, 2 * from);
  stored.length = start;
  for (let i = 0; i < part.length; i++) stored.push(part[i]);
}

window.__pluginUpdate = function (state) {
  const wf = state.waveform;
  if (wf && wf.current !== undefined) {
    const from = wf.from | 0;
    applyDelta(curArr, from, wf.current);
    applyDelta(outCurArr, from, wf.outCurrent || []);
    wf.current = curArr;
    wf.outCurrent = outCurArr;
  }
  if (wf && wf.previous === undefined && latestState && latestState.waveform) {
    wf.previous = latestState.waveform.previous;
    wf.outPrevious = latestState.waveform.outPrevious;
    wf.prevId = latestState.waveform.prevId;
  }
  latestState = state;
  barsOnScreen = state.barsOnScreen;
  requestRender();
};

// ---------------------------------------------------------------------
// INIT
// ---------------------------------------------------------------------
async function init () {
  readColors();
  shaperView = setupCanvas(document.getElementById("shaperCanvas"));
  scopeView = setupCanvas(document.getElementById("scopeCanvas"));

  setupTimeButtons();
  setupZoomSlider();
  setupDeltaButton();

  ["Input", "Threshold", "Softness", "Output", "Saturator"].forEach((label) => {
    const id = label.toLowerCase();
    const knobEl = document.getElementById(`knob${label}`);
    setupKnobDragging(knobEl, id);
  });

  // Стан вшитий C++ у сторінку (withInitialisationData) — без round-trip.
  // Фолбек на виклик getInitialState — якщо даних немає (напр. сторінку
  // відкрито просто у браузері для перегляду дизайну).
  const injected = window.__JUCE__?.initialisationData?.initialState?.[0];
  const initial = (injected && injected.params)
    ? injected
    : await callNativeWithTimeout(1500, "getInitialState");

  if (initial && initial.params) {
    initial.params.forEach((p) => {
      paramDescriptors[p.id] = { min: p.min, max: p.max, step: p.step, label: p.label, currentValue: p.value };
      setKnobVisual(p.id, p.value);
    });

    deltaState = !!initial.delta;
    document.getElementById("deltaImg").src = deltaState ? "Resources/DeltaOn.png" : "Resources/DeltaOff.png";

    barsOnScreen = initial.barsOnScreen || 1.0;
    document.querySelectorAll(".barBtn").forEach((b) =>
      b.classList.toggle("active", Math.abs(parseFloat(b.dataset.bars) - barsOnScreen) < 1e-3));
  } else {
    // Фолбек (напр. якщо відкрито просто у браузері для перегляду дизайну)
    paramDescriptors.input     = { min: -24, max: 24, step: 0.01, label: "dB", currentValue: 0 };
    paramDescriptors.threshold = { min: -24, max: 10, step: 0.01, label: "dB", currentValue: 0 };
    paramDescriptors.softness  = { min: 0,   max: 100, step: 0.1, label: "%",  currentValue: 20 };
    paramDescriptors.output    = { min: -24, max: 24, step: 0.01, label: "dB", currentValue: 0 };
    paramDescriptors.saturator = { min: 0,   max: 100, step: 0.1, label: "%",  currentValue: 0 };
    Object.keys(paramDescriptors).forEach((id) => setKnobVisual(id, paramDescriptors[id].currentValue));
  }

  requestRender();

  // Дозволяємо C++ слати стан (до цього він не пушить, щоб не забивати
  // чергу WebView2 під час завантаження).
  callNative("uiReady").catch(() => {});

  revealWhenLoaded();
}

// Показуємо UI, коли картинки (фон, ручки) завантажені — одним кадром.
// Страховка: не довше ніж 600 мс, щоб ніколи не лишитись з порожнім вікном.
function revealWhenLoaded () {
  const app = document.getElementById("app");
  const show = () => requestAnimationFrame(() => app.classList.add("ready"));

  if (document.readyState === "complete") { show(); return; }

  let done = false;
  const once = () => { if (!done) { done = true; show(); } };
  window.addEventListener("load", once, { once: true });
  setTimeout(once, 600);
}

document.addEventListener("DOMContentLoaded", init);
