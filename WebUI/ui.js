// =====================================================================
// MODEL-CLIPONIZER No.8 — Web UI logic
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
    // JUCE 9 API: функція getNativeFunction() з juce_interop.js
    if (typeof getNativeFunction === "function") {
      getNativeFunction(name)(...args);
      return Promise.resolve(null);
    }
  } catch (e) { /* ignore - running outside JUCE (e.g. plain browser preview) */ }
  return Promise.resolve(null);
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

  const kneeWidth = Math.max(0.0005, knee01);
  const kneeStart = Math.max(0, thresholdLin - kneeWidth);

  if (absX <= kneeStart) return [x, 0];

  const overshoot = absX - kneeStart;
  const shaped = kneeWidth * Math.tanh(overshoot / kneeWidth);
  const outAbs = kneeStart + shaped;
  const clipAmount = Math.min(1, Math.max(0, shaped / kneeWidth));
  return [sign * outAbs, clipAmount];
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
    startY = e.clientY;
    startValue = d.currentValue !== undefined ? d.currentValue : d.value;

    callNative("beginGesture", paramId);
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
    callNative("endGesture", paramId);
  });

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
    img.src = deltaState ? "DeltaOn.png" : "DeltaOff.png";
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

  slider.addEventListener("input", () => {
    verticalZoom = parseFloat(slider.value);
    label.textContent = verticalZoom.toFixed(2) + "x";
    render();
  });
}

// ---------------------------------------------------------------------
// CANVAS SETUP (handles devicePixelRatio + resizing)
// ---------------------------------------------------------------------
function setupCanvas (canvas) {
  const ctx = canvas.getContext("2d");

  const resize = () => {
    const rect = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(rect.width * dpr));
    canvas.height = Math.max(1, Math.round(rect.height * dpr));
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  };

  new ResizeObserver(resize).observe(canvas);
  resize();

  return ctx;
}

let shaperCtx, scopeCtx;

// ---------------------------------------------------------------------
// RENDER: CLIP SHAPER PANEL
// ---------------------------------------------------------------------
function renderShaper (state) {
  const canvas = document.getElementById("shaperCanvas");
  const ctx = shaperCtx;
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  ctx.clearRect(0, 0, w, h);

  const style = getComputedStyle(document.documentElement);
  const cGrid   = style.getPropertyValue("--scopeGrid").trim();
  const cNormal = style.getPropertyValue("--traceNormal").trim();
  const cSoft   = style.getPropertyValue("--traceSoftClip").trim();
  const cHard   = style.getPropertyValue("--traceHardClip").trim();
  const cThresh = style.getPropertyValue("--thresholdLine").trim();
  const cAmber  = style.getPropertyValue("--textAmber").trim();

  const thresholdDb = state.params.threshold;
  const thresholdLin = dbToGain(thresholdDb);
  const knee01 = Math.min(1, Math.max(0, state.params.softness / 100));

  const baseRange = 2.0;
  const effectiveRange = Math.max(0.05, baseRange / Math.max(0.1, verticalZoom));
  const maxAmp = effectiveRange * 2.0;

  const cx = w / 2, cy = h / 2;
  const ampToY = (amp) => {
    const norm = Math.min(1, Math.max(-1, amp / effectiveRange));
    return cy - norm * (h / 2);
  };
  const ampToX = (amp) => {
    const norm = Math.min(1, Math.max(-1, amp / effectiveRange));
    return cx + norm * (w / 2);
  };

  // grid
  ctx.strokeStyle = cGrid;
  ctx.lineWidth = 1;
  ctx.font = "10px Consolas, monospace";
  [-24, -12, -6, 0, 6].forEach((db) => {
    const gain = dbToGain(db);
    [gain, -gain].forEach((g) => {
      const x = ampToX(g), y = ampToY(g);
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
    });
  });

  ctx.fillStyle = "rgba(232,195,122,0.55)";
  [-24, -12, -6, 0, 6].forEach((db) => {
    const y = ampToY(dbToGain(db));
    if (y >= 0 && y <= h) {
      const text = (db > 0 ? "+" : "") + db.toFixed(0) + " dB";
      ctx.textAlign = "right";
      ctx.fillText(text, w - 4, y - 2);
    }
  });

  // unity diagonal
  ctx.strokeStyle = "rgba(143,227,160,0.3)";
  ctx.beginPath(); ctx.moveTo(0, h); ctx.lineTo(w, 0); ctx.stroke();

  // transfer curve
  const steps = 300;
  for (let i = 0; i < steps; i++) {
    const xA = -maxAmp + (i / steps) * (2 * maxAmp);
    const xB = -maxAmp + ((i + 1) / steps) * (2 * maxAmp);
    const [outA, clipA] = clipShaperProcess(xA, thresholdLin, knee01);
    const [outB] = clipShaperProcess(xB, thresholdLin, knee01);

    ctx.strokeStyle = clipA < 0.001 ? cNormal : (clipA < 0.85 ? cSoft : cHard);
    ctx.lineWidth = 2.2;
    ctx.beginPath();
    ctx.moveTo(ampToX(xA), ampToY(outA));
    ctx.lineTo(ampToX(xB), ampToY(outB));
    ctx.stroke();
  }

  // threshold dashed lines
  ctx.strokeStyle = cThresh;
  ctx.setLineDash([5, 4]);
  ctx.lineWidth = 1.2;
  [thresholdLin, -thresholdLin].forEach((v) => {
    const y = ampToY(v);
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
  });
  ctx.setLineDash([]);

  ctx.fillStyle = cThresh;
  ctx.font = "bold 12px Consolas, monospace";
  ctx.textAlign = "left";
  const label = (thresholdDb > 0 ? "THRESHOLD +" : "THRESHOLD ") + thresholdDb.toFixed(1) + " dB";
  ctx.fillText(label, 2, ampToY(thresholdLin) - 4);

  // live dots
  liveDotPosSmoothed += (state.livePeakPos - liveDotPosSmoothed) * 0.6;
  liveDotNegSmoothed += (state.livePeakNeg - liveDotNegSmoothed) * 0.6;

  const drawDot = (amp) => {
    const [outAmp, clipAmt] = clipShaperProcess(amp, thresholdLin, knee01);
    const x = ampToX(amp), y = ampToY(outAmp);
    const col = clipAmt < 0.001 ? cNormal : (clipAmt < 0.85 ? cSoft : cHard);
    ctx.fillStyle = col;
    ctx.beginPath(); ctx.arc(x, y, 4.5, 0, Math.PI * 2); ctx.fill();
    ctx.strokeStyle = "rgba(255,255,255,0.6)";
    ctx.lineWidth = 1;
    ctx.beginPath(); ctx.arc(x, y, 4.5, 0, Math.PI * 2); ctx.stroke();
  };
  drawDot(liveDotPosSmoothed);
  drawDot(liveDotNegSmoothed);
}

// ---------------------------------------------------------------------
// RENDER: OSCILLOSCOPE PANEL
// ---------------------------------------------------------------------
function renderScope (state) {
  const canvas = document.getElementById("scopeCanvas");
  const ctx = scopeCtx;
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  ctx.clearRect(0, 0, w, h);

  const style = getComputedStyle(document.documentElement);
  const cGrid   = style.getPropertyValue("--scopeGrid").trim();
  const cNormal = style.getPropertyValue("--traceNormal").trim();
  const cSoft   = style.getPropertyValue("--traceSoftClip").trim();
  const cHard   = style.getPropertyValue("--traceHardClip").trim();
  const cThresh = style.getPropertyValue("--thresholdLine").trim();

  const bpm = state.bpm;
  const qnPerBar = state.qnPerBar;
  const thresholdDb = state.params.threshold;
  const thresholdLin = dbToGain(thresholdDb);
  const knee01 = Math.min(1, Math.max(0, state.params.softness / 100));
  const kneeWidth = knee01 > 0.001 ? Math.max(0.0005, knee01) : 0.0;
  const kneeStart = kneeWidth > 0 ? Math.max(0, thresholdLin - kneeWidth) : thresholdLin;

  const baseRange = 2.0;
  const effectiveRange = Math.max(0.05, baseRange / Math.max(0.1, verticalZoom));
  const cy = h / 2;
  const ampToY = (amp) => {
    const norm = Math.min(1, Math.max(-1, amp / effectiveRange));
    return cy - norm * (h / 2);
  };

  // dB grid + labels
  ctx.font = "10px Consolas, monospace";
  [-24, -12, -6, 0, 6].forEach((db) => {
    const y = ampToY(dbToGain(db));
    if (y >= 0 && y <= h) {
      ctx.strokeStyle = cGrid;
      ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
      ctx.fillStyle = "rgba(232,195,122,0.55)";
      ctx.textAlign = "right";
      ctx.fillText((db > 0 ? "+" : "") + db.toFixed(0) + " dB", w - 4, y - 2);
    }
  });

  // musical grid
  const totalQuarterNotes = qnPerBar * barsOnScreen;
  const quarterLineCount = Math.max(1, Math.ceil(totalQuarterNotes));
  for (let q = 0; q <= quarterLineCount; q++) {
    const x = (q / quarterLineCount) * w;
    const isBarLine = (q % qnPerBar) < 0.001;
    ctx.strokeStyle = isBarLine ? "rgba(232,195,122,0.28)" : cGrid;
    ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
  }

  // waveform
  const drawWaveform = (buckets, endFraction, faded) => {
    if (!buckets || buckets.length === 0) return;

    const n = buckets.length;
    const endPx = Math.round(Math.min(1, Math.max(0, endFraction)) * w);
    if (endPx <= 0) return;

    const colorByY = (y) => {
      const thresholdY = ampToY(thresholdLin);
      const negThresholdY = ampToY(-thresholdLin);
      const kneePosY = ampToY(kneeStart);
      const kneeNegY = ampToY(-kneeStart);
      if (y <= thresholdY || y >= negThresholdY) return cHard;
      if (y <= kneePosY || y >= kneeNegY) return cSoft;
      return cNormal;
    };

    const alpha = faded ? 0.22 : 0.55;

    for (let px = 0; px < endPx; px++) {
      const bucketIndex = Math.min(n - 1, Math.floor((px / w) * n));
      const [minV, maxV] = buckets[bucketIndex];
      const yTop = ampToY(maxV);
      const yBot = ampToY(minV);
      if (yTop >= yBot) continue;

      const col = colorByY((yTop + yBot) / 2);
      ctx.fillStyle = hexWithAlpha(col, alpha);
      ctx.fillRect(px, yTop, 1.4, yBot - yTop);
    }

    // outline (top edge)
    ctx.lineWidth = 1;
    ctx.globalAlpha = faded ? 0.3 : 1.0;
    for (let px = 1; px < endPx; px++) {
      const bA = Math.min(n - 1, Math.floor(((px - 1) / w) * n));
      const bB = Math.min(n - 1, Math.floor((px / w) * n));
      const yA = ampToY(buckets[bA][1]);
      const yB = ampToY(buckets[bB][1]);
      ctx.strokeStyle = colorByY((yA + yB) / 2);
      ctx.beginPath(); ctx.moveTo(px - 1, yA); ctx.lineTo(px, yB); ctx.stroke();
    }
    ctx.globalAlpha = 1.0;
  };

  drawWaveform(state.waveform.previous, 1.0, true);
  drawWaveform(state.waveform.current, state.waveform.progress, false);

  // threshold lines
  ctx.strokeStyle = cThresh;
  ctx.setLineDash([5, 4]);
  ctx.lineWidth = 1.5;
  [thresholdLin, -thresholdLin].forEach((v) => {
    const y = ampToY(v);
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
  });
  ctx.setLineDash([]);

  if (kneeWidth > 0 && kneeStart < thresholdLin) {
    ctx.strokeStyle = hexWithAlpha(cSoft, 0.45);
    ctx.setLineDash([3, 5]);
    ctx.lineWidth = 1.0;
    [kneeStart, -kneeStart].forEach((v) => {
      const y = ampToY(v);
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
    });
    ctx.setLineDash([]);
  }

  ctx.fillStyle = cThresh;
  ctx.font = "bold 12px Consolas, monospace";
  ctx.textAlign = "left";
  const label = (thresholdDb > 0 ? "THRESHOLD +" : "THRESHOLD ") + thresholdDb.toFixed(1) + " dB";
  ctx.fillText(label, 2, ampToY(thresholdLin) - 4);
}

function hexWithAlpha (colorStr, alpha) {
  // colorStr can be "#rrggbb" — convert to rgba()
  if (colorStr.startsWith("#") && colorStr.length === 7) {
    const r = parseInt(colorStr.substr(1, 2), 16);
    const g = parseInt(colorStr.substr(3, 2), 16);
    const b = parseInt(colorStr.substr(5, 2), 16);
    return `rgba(${r},${g},${b},${alpha})`;
  }
  return colorStr;
}

// ---------------------------------------------------------------------
// RENDER: CLIP LED + BPM + PROGRESS TEXT
// ---------------------------------------------------------------------
function renderMeta (state) {
  clipLedSmoothed += (state.clipLevel - clipLedSmoothed) * 0.2;

  const led = document.getElementById("clipLed");
  const glow = document.getElementById("clipGlow");

  let color;
  if (clipLedSmoothed < 0.05) color = "#1f7a1f";
  else if (clipLedSmoothed < 0.3) color = "#a6c11a";
  else if (clipLedSmoothed < 0.65) color = "#ff8a1a";
  else color = "#ff2e2e";

  led.style.background = color;
  led.style.boxShadow = `0 0 ${6 + clipLedSmoothed * 14}px ${color}`;
  glow.style.background = `radial-gradient(circle, ${hexToRgba(color, 0.6 * clipLedSmoothed)} 0%, ${hexToRgba(color, 0)} 70%)`;

  document.getElementById("bpmLabel").textContent = "BPM: " + Math.round(state.bpm);

  const pct = state.waveform.current && state.waveform.current.length > 0
    ? Math.round(state.waveform.progress * 100)
    : 0;
  document.getElementById("scopeProgress").textContent = `${barsOnScreen.toFixed(2)} BAR   ${pct}%`;
}

function hexToRgba (hex, alpha) {
  const r = parseInt(hex.substr(1, 2), 16);
  const g = parseInt(hex.substr(3, 2), 16);
  const b = parseInt(hex.substr(5, 2), 16);
  return `rgba(${r},${g},${b},${alpha})`;
}

// ---------------------------------------------------------------------
// RENDER: KNOBS + DELTA (reflect backend-confirmed values)
// ---------------------------------------------------------------------
function renderControls (state) {
  ["input", "threshold", "softness", "output"].forEach((id) => {
    const d = paramDescriptors[id];
    if (!d) return;
    d.currentValue = state.params[id];
    setKnobVisual(id, state.params[id]);
  });

  if (state.params.delta !== deltaState) {
    deltaState = state.params.delta;
    document.getElementById("deltaImg").src = deltaState ? "DeltaOn.png" : "DeltaOff.png";
  }
}

// ---------------------------------------------------------------------
// MAIN RENDER
// ---------------------------------------------------------------------
function render () {
  if (!latestState) return;
  renderControls(latestState);
  renderShaper(latestState);
  renderScope(latestState);
  renderMeta(latestState);
}

// ---------------------------------------------------------------------
// ENTRY POINT — called by C++ every ~33ms via evaluateJavascript()
// ---------------------------------------------------------------------
window.__pluginUpdate = function (state) {
  latestState = state;
  barsOnScreen = state.barsOnScreen;
  render();
};

// ---------------------------------------------------------------------
// INIT
// ---------------------------------------------------------------------
async function init () {
  shaperCtx = setupCanvas(document.getElementById("shaperCanvas"));
  scopeCtx = setupCanvas(document.getElementById("scopeCanvas"));

  setupTimeButtons();
  setupZoomSlider();
  setupDeltaButton();

  ["Input", "Threshold", "Softness", "Output"].forEach((label) => {
    const id = label.toLowerCase();
    const knobEl = document.getElementById(`knob${label}`);
    setupKnobDragging(knobEl, id);
  });

  const initial = await callNative("getInitialState");

  if (initial && initial.params) {
    initial.params.forEach((p) => {
      paramDescriptors[p.id] = { min: p.min, max: p.max, step: p.step, label: p.label, currentValue: p.value };
      setKnobVisual(p.id, p.value);
    });

    deltaState = !!initial.delta;
    document.getElementById("deltaImg").src = deltaState ? "DeltaOn.png" : "DeltaOff.png";

    barsOnScreen = initial.barsOnScreen || 1.0;
  } else {
    // Фолбек (напр. якщо відкрито просто у браузері для перегляду дизайну)
    paramDescriptors.input     = { min: -24, max: 24, step: 0.01, label: "dB", currentValue: 0 };
    paramDescriptors.threshold = { min: -24, max: 10, step: 0.01, label: "dB", currentValue: 0 };
    paramDescriptors.softness  = { min: 0,   max: 100, step: 0.1, label: "%",  currentValue: 20 };
    paramDescriptors.output    = { min: -24, max: 24, step: 0.01, label: "dB", currentValue: 0 };
    Object.keys(paramDescriptors).forEach((id) => setKnobVisual(id, paramDescriptors[id].currentValue));
  }

  render();
}

document.addEventListener("DOMContentLoaded", init);
