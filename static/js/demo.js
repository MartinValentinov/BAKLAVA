const STEPS      = BAKLAVA_SETTINGS.demo_steps || [];
const AGGREGATES = new Set(BAKLAVA_SETTINGS.demo_aggregate_stages || []);
const AUTOPLAY_MS = 6000;
const SCENE_ASSETS = BAKLAVA_SETTINGS.demo_scene_assets || {};
const STATIC_ROOT  = "/static/";

const el = (id) => document.getElementById(id);

const demoStart    = el("demoStart");
const startList    = el("startList");
const startSub     = el("startSub");
const btnStart     = el("btnStart");

const demoBusy     = el("demoBusy");
const demoBusyText = el("demoBusyText");

const sceneLabel   = el("demoSceneLabel");
const btnRestart   = el("btnRestart");

const stepIndex    = el("stepIndex");
const stepCount    = el("stepCount");
const stepTitle    = el("stepTitle");
const stepLead     = el("stepLead");
const stepBody     = el("stepBody");
const stepParallel = el("stepParallel");
const stepFact     = el("stepFact");
const stepMs       = el("stepMs");
const stepCum      = el("stepCum");
const stepStages   = el("stepStages");
const stepFree     = el("stepFree");

const railSteps    = el("railSteps");
const btnPrev      = el("btnPrev");
const btnNext      = el("btnNext");
const btnAuto      = el("btnAuto");

const demoInset    = el("demoInset");
const demoInsetImg = el("demoInsetImg");
const demoInsetCap = el("demoInsetCap");
const autoFill     = el("autoFill");

const demoStats    = el("demoStats");
const statTotal    = el("statTotal");
const statDark     = el("statDark");

let map            = null;
let footprintLayer = null;
let swathLayer     = null;
let sarOverlay     = null;
let vesselLayer    = null;

let scene       = null;
let timingMap   = new Map();
let pickedId    = null;
let current     = -1;
let autoTimer   = null;
let autoTick    = null;
let autoStarted = 0;
let assets      = {};
let maskOverlay = null;
let landLayer   = null;
let coastline   = null;
let coastReq    = null;
let lastViewedId = null;

const SEA_REGIONS = [
    { name: "Sea of Azov",     lat: [45.2, 47.4], lon: [34.8, 39.6] },
    { name: "Sea of Marmara",  lat: [40.2, 41.3], lon: [26.5, 30.0] },
    { name: "Black Sea",       lat: [40.9, 47.4], lon: [27.3, 42.0] },
    { name: "Aegean Sea",      lat: [35.0, 41.0], lon: [22.5, 28.5] },
    { name: "Ionian Sea",      lat: [35.8, 40.5], lon: [15.5, 22.5] },
    { name: "Adriatic Sea",    lat: [39.5, 45.9], lon: [12.2, 20.0] },
    { name: "Tyrrhenian Sea",  lat: [38.0, 44.0], lon: [9.0,  16.0] },
    { name: "Ligurian Sea",    lat: [42.8, 44.6], lon: [7.0,  10.0] },
    { name: "Balearic Sea",    lat: [37.8, 43.8], lon: [0.0,   9.0] },
    { name: "Alboran Sea",     lat: [34.8, 37.6], lon: [-6.0,  0.0] },
    { name: "Algerian Basin",  lat: [35.0, 38.5], lon: [0.0,   9.5] },
    { name: "Levantine Sea",   lat: [30.5, 37.2], lon: [28.0, 36.6] },
    { name: "Libyan Sea",      lat: [30.0, 35.6], lon: [15.0, 25.0] },
    { name: "Strait of Sicily", lat: [33.0, 38.0], lon: [10.0, 15.0] },
];

const S1_NAME = /^(S1[A-D])_([A-Z]{2})_([A-Z]{4})_[A-Z0-9]{4}_(\d{8})T(\d{6})_/;

function regionName(lat, lon) {
    const hit = SEA_REGIONS.find(r =>
        lat >= r.lat[0] && lat <= r.lat[1] && lon >= r.lon[0] && lon <= r.lon[1]);
    return hit ? hit.name : null;
}

function shortCoords(lat, lon) {
    const ns = lat >= 0 ? "N" : "S";
    const ew = lon >= 0 ? "E" : "W";
    return `${Math.abs(lat).toFixed(2)}°${ns} ${Math.abs(lon).toFixed(2)}°${ew}`;
}

function sceneCentre(corners) {
    if (!corners || !corners.length) {
        return null;
    }
    const lat = corners.reduce((sum, c) => sum + c[0], 0) / corners.length;
    const lon = corners.reduce((sum, c) => sum + c[1], 0) / corners.length;
    return [lat, lon];
}

function scenePlace(scene) {
    const centre = sceneCentre(scene.corners);
    if (!centre) {
        return null;
    }
    return {
        region: regionName(centre[0], centre[1]),
        coords: shortCoords(centre[0], centre[1]),
    };
}

function sentinelParts(rawName) {
    const m = S1_NAME.exec(rawName);
    if (!m) {
        return null;
    }
    const [, mission, mode, product, day, time] = m;
    const when = new Date(Date.UTC(
        Number(day.slice(0, 4)), Number(day.slice(4, 6)) - 1, Number(day.slice(6, 8)),
        Number(time.slice(0, 2)), Number(time.slice(2, 4)), Number(time.slice(4, 6))));
    return { mission, mode, product, when };
}

function formatWhen(date) {
    return date.toLocaleString("en-GB", {
        day: "numeric", month: "short", year: "numeric",
        hour: "2-digit", minute: "2-digit",
        timeZone: "UTC",
    }) + " UTC";
}

function sceneWhen(rawName) {
    const parts = sentinelParts(rawName);
    return parts ? formatWhen(parts.when) : null;
}

function sceneDisplayName(scene) {
    const place = scenePlace(scene);
    const when  = sceneWhen(scene.id) || scene.label;
    if (!place) {
        return when;
    }
    return `${place.region || place.coords} — ${when}`;
}

function fmtMs(ms) {
    if (ms === null || ms === undefined) return "-";
    return ms >= 1000 ? `${(ms / 1000).toFixed(2)} s` : `${Math.round(ms)} ms`;
}

function stepMsOf(step) {
    return step.stages.reduce((sum, name) => sum + (timingMap.get(name) || 0), 0);
}

function serialCumThrough(index) {
    let total = 0;
    for (let i = 0; i <= index; i += 1) {
        if (!STEPS[i].parallel) total += stepMsOf(STEPS[i]);
    }
    return total;
}

function mercatorY(lat) {
    return Math.log(Math.tan(Math.PI / 4 + lat * Math.PI / 360));
}

function squareToQuad(p) {
    const [x0, y0] = [p[0].x, p[0].y];
    const [x1, y1] = [p[1].x, p[1].y];
    const [x2, y2] = [p[2].x, p[2].y];
    const [x3, y3] = [p[3].x, p[3].y];

    const dx1 = x1 - x2, dx2 = x3 - x2, dx3 = x0 - x1 + x2 - x3;
    const dy1 = y1 - y2, dy2 = y3 - y2, dy3 = y0 - y1 + y2 - y3;

    let a, b, c, d, e, f, g, h;

    if (Math.abs(dx3) < 1e-9 && Math.abs(dy3) < 1e-9) {
        a = x1 - x0; b = x2 - x1; c = x0;
        d = y1 - y0; e = y2 - y1; f = y0;
        g = 0; h = 0;
    } else {
        const den = dx1 * dy2 - dx2 * dy1;
        g = (dx3 * dy2 - dx2 * dy3) / den;
        h = (dx1 * dy3 - dx3 * dy1) / den;
        a = x1 - x0 + g * x1;
        b = x3 - x0 + h * x3;
        c = x0;
        d = y1 - y0 + g * y1;
        e = y3 - y0 + h * y3;
        f = y0;
    }

    return { a, b, c, d, e, f, g, h };
}

function swathClipPath(bounds, swath) {
    if (!swath || swath.length < 3) return "";

    const latTop = bounds[0][0], latBottom = bounds[2][0];
    const lonLeft = bounds[0][1], lonRight = bounds[1][1];
    const yTop = mercatorY(latTop), yBottom = mercatorY(latBottom);

    const points = swath.map((corner) => {
        const x = (corner[1] - lonLeft) / (lonRight - lonLeft) * 100;
        const y = (mercatorY(corner[0]) - yTop) / (yBottom - yTop) * 100;
        return `${x.toFixed(3)}% ${y.toFixed(3)}%`;
    });

    return `polygon(${points.join(", ")})`;
}

const SarQuadOverlay = L.Layer.extend({
    initialize(url, quad, opacity, swath) {
        this._url = url;
        this._quad = quad;
        this._opacity = opacity;
        this._swath = swath;
    },

    onAdd(map) {
        this._map = map;
        const img = L.DomUtil.create("img", "sar-quad");
        img.style.position        = "absolute";
        img.style.left            = "0";
        img.style.top             = "0";
        img.style.transformOrigin = "0 0";
        img.style.pointerEvents   = "none";
        img.style.opacity         = String(this._opacity);
        img.style.clipPath        = swathClipPath(this._quad, this._swath);
        img.alt = "";
        img.onload = () => this._warp();
        img.src = this._url;

        this._img = img;
        map.getPanes().overlayPane.appendChild(img);
        map.on("viewreset zoom move zoomend moveend", this._warp, this);
        this._warp();
        requestAnimationFrame(() => this._warp());
    },

    onRemove(map) {
        L.DomUtil.remove(this._img);
        map.off("viewreset zoom move zoomend moveend", this._warp, this);
        this._img = null;
    },

    _warp() {
        const img = this._img;
        if (!img || !img.naturalWidth || !this._map) return;

        const w = img.naturalWidth;
        const h = img.naturalHeight;
        const points = this._quad.map(
            (corner) => this._map.latLngToLayerPoint(L.latLng(corner[0], corner[1])));
        const m = squareToQuad(points);

        img.style.transform = "matrix3d(" + [
            m.a / w, m.d / w, 0, m.g / w,
            m.b / h, m.e / h, 0, m.h / h,
            0,       0,       1, 0,
            m.c,     m.f,     0, 1,
        ].join(",") + ")";
    },
});

function initMap() {
    map = L.map("map", { zoomControl: true, attributionControl: true })
        .setView([BAKLAVA_SETTINGS.map_default_lat, BAKLAVA_SETTINGS.map_default_lon],
                 BAKLAVA_SETTINGS.map_default_zoom);

    L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
        maxZoom: 18,
        attribution: "&copy; OpenStreetMap contributors",
    }).addTo(map);

    vesselLayer = L.layerGroup().addTo(map);
}

function clearScene() {
    [footprintLayer, swathLayer, sarOverlay, maskOverlay].forEach((layer) => {
        if (layer) map.removeLayer(layer);
    });
    hideLandmask();
    footprintLayer = swathLayer = sarOverlay = maskOverlay = null;
    vesselLayer.clearLayers();
    demoStats.classList.add("is-hidden");
    demoInset.classList.add("is-hidden");
}

function showFootprint() {
    if (footprintLayer) return;
    footprintLayer = L.polygon(scene.corners, {
        color: "#2F6FD0", weight: 2, dashArray: "6 5", fill: false,
    }).addTo(map);
}

function fitToScene() {
    const swath = scene.sar_overlay && scene.sar_overlay.swath;
    const bounds = swath && swath.length >= 3
        ? L.latLngBounds(swath)
        : L.latLngBounds(scene.corners);
    map.fitBounds(bounds, { padding: [40, 40], animate: false });
}

function hideSar() {
    if (sarOverlay)  { map.removeLayer(sarOverlay);  sarOverlay = null; }
    if (maskOverlay) { map.removeLayer(maskOverlay); maskOverlay = null; }
    if (swathLayer)  { map.removeLayer(swathLayer);  swathLayer = null; }
}

function quadFor() {
    const overlay = scene.sar_overlay;
    return (overlay && overlay.corners) || scene.corners;
}

function addSwathOutline() {
    const overlay = scene.sar_overlay;
    if (!overlay || !overlay.swath || swathLayer) return;
    swathLayer = L.polygon(overlay.swath, {
        color: "#918450", weight: 2, fill: false,
    }).addTo(map);
}

function showSar() {
    showFootprint();
    const overlay = scene.sar_overlay;
    if (!overlay || !overlay.url) return;

    if (maskOverlay) { map.removeLayer(maskOverlay); maskOverlay = null; }
    addSwathOutline();
    if (sarOverlay) return;

    sarOverlay = new SarQuadOverlay(overlay.url, quadFor(), 1, overlay.swath);
    sarOverlay.addTo(map);
}

function loadCoastline(box) {
    const key = box.map((v) => v.toFixed(2)).join(",");
    if (coastline && coastReq === key) return Promise.resolve(coastline);
    coastReq = key;
    const query = `w=${box[0]}&s=${box[1]}&e=${box[2]}&n=${box[3]}`;
    return fetch(`/api/coastline?${query}`)
        .then((r) => (r.ok ? r.json() : null))
        .then((d) => { if (coastReq === key) coastline = d; return d; })
        .catch(() => null);
}

function clipRingToQuad(ring, quad) {
    let out = ring;
    for (let i = 0; i < quad.length && out.length; i += 1) {
        const a = quad[i];
        const b = quad[(i + 1) % quad.length];
        const side = (p) => (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]);
        const input = out;
        out = [];
        for (let j = 0; j < input.length; j += 1) {
            const cur = input[j];
            const prev = input[(j + input.length - 1) % input.length];
            const dCur = side(cur);
            const dPrev = side(prev);
            if (dCur >= 0) {
                if (dPrev < 0) out.push(lerpEdge(prev, cur, dPrev, dCur));
                out.push(cur);
            } else if (dPrev >= 0) {
                out.push(lerpEdge(prev, cur, dPrev, dCur));
            }
        }
    }
    return out;
}

function lerpEdge(p, q, dp, dq) {
    const t = dp / (dp - dq);
    return [p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t];
}

function orientCcw(quad) {
    let area = 0;
    for (let i = 0; i < quad.length; i += 1) {
        const a = quad[i];
        const b = quad[(i + 1) % quad.length];
        area += a[0] * b[1] - b[0] * a[1];
    }
    return area >= 0 ? quad : quad.slice().reverse();
}

function clipFeatureToQuad(geometry, quad) {
    const polys = geometry.type === "Polygon"
        ? [geometry.coordinates]
        : geometry.coordinates;
    const kept = [];
    polys.forEach((rings) => {
        const outer = clipRingToQuad(rings[0], quad);
        if (outer.length >= 3) kept.push([outer]);
    });
    return kept.length ? { type: "MultiPolygon", coordinates: kept } : null;
}

function hideLandmask() {
    if (landLayer) { map.removeLayer(landLayer); landLayer = null; }
}

function showLandmask() {
    showSar();
    if (landLayer) return;

    const swathLL = scene.sar_overlay && scene.sar_overlay.swath;
    const ref = swathLL && swathLL.length ? swathLL : scene.corners;
    const lats = ref.map((c) => c[0]);
    const lons = ref.map((c) => c[1]);
    const near = [Math.min(...lons) - 0.2, Math.min(...lats) - 0.2,
                  Math.max(...lons) + 0.2, Math.max(...lats) + 0.2];

    loadCoastline(near).then((data) => {
        if (!data || landLayer) return;
        if (!STEPS[current] || STEPS[current].visual !== "landmask") return;

        const swath = scene.sar_overlay && scene.sar_overlay.swath;
        const quad = swath && swath.length === 4
            ? orientCcw(swath.map((c) => [c[1], c[0]]))
            : null;

        const shown = [];
        data.features.forEach((f) => {
            const geometry = quad ? clipFeatureToQuad(f.geometry, quad) : f.geometry;
            if (geometry) shown.push({ type: "Feature", properties: f.properties, geometry });
        });

        landLayer = L.geoJSON({ type: "FeatureCollection", features: shown }, {
            style: (f) => (f.properties.lvl === 2
                ? { color: "#2F6FD0", weight: 1, fillColor: "#2F6FD0", fillOpacity: 0.55 }
                : { color: "#D0342C", weight: 1, fillColor: "#D0342C", fillOpacity: 0.55 }),
            interactive: false,
        }).addTo(map);
    });
}

function showVessels(mode) {
    showSar();
    vesselLayer.clearLayers();

    const vessels = scene.vessels || [];
    const isFinal = mode === "final";

    vessels.forEach((v) => {
        const colour = !isFinal ? "#FFD29D" : (v.dark ? "#FF4438" : "#3DDC97");
        const style = {
            color: colour,
            weight: 2,
            opacity: 1,
            fill: true,
            fillColor: colour,
            fillOpacity: 0.15,
        };

        if (v.corners && v.corners.length === 4) {
            L.polygon(v.corners, style).addTo(vesselLayer);
        } else {
            L.circleMarker([v.lat, v.lon],
                           Object.assign({ radius: 4 }, style)).addTo(vesselLayer);
        }
    });

    if (mode === "final") {
        statTotal.textContent = (scene.totals && scene.totals.total) || vessels.length;
        statDark.textContent  = (scene.totals && scene.totals.dark) || 0;
        demoStats.classList.remove("is-hidden");
    } else {
        demoStats.classList.add("is-hidden");
    }
}

function applyInset(step) {
    const path = step.inset && assets[step.inset];
    if (!path) {
        demoInset.classList.add("is-hidden");
        demoInsetImg.removeAttribute("src");
        return;
    }
    demoInsetImg.src = STATIC_ROOT + path;
    demoInsetCap.textContent = step.inset_caption || "";
    demoInset.classList.remove("is-hidden");
}

function applyVisual(visual) {
    if (visual === "footprint") {
        clearVessels();
        hideLandmask();
        hideSar();
        showFootprint();
    } else if (visual === "sar") {
        clearVessels();
        hideLandmask();
        showSar();
    } else if (visual === "landmask") {
        clearVessels();
        showLandmask();
    } else if (visual === "vessels_raw") {
        hideLandmask();
        showVessels("raw");
    } else if (visual === "vessels_final") {
        hideLandmask();
        showVessels("final");
    }
}

function clearVessels() {
    vesselLayer.clearLayers();
    demoStats.classList.add("is-hidden");
}

function buildRail() {
    railSteps.innerHTML = "";
    const widths = STEPS.map((s) => Math.max(stepMsOf(s), 1));
    const widest = Math.max(...widths);

    STEPS.forEach((step, i) => {
        const li = document.createElement("li");
        li.style.flex = `${Math.max(0.55, widths[i] / widest)} 1 0`;

        const btn = document.createElement("button");
        btn.className = "demo-rail__step" + (step.parallel ? " is-parallel" : "");
        btn.dataset.index = String(i);
        btn.title = `${step.title} - ${fmtMs(stepMsOf(step))}`;

        const bar = document.createElement("div");
        bar.className = "demo-rail__bar";

        const name = document.createElement("span");
        name.className = "demo-rail__name";
        name.textContent = step.title;

        btn.appendChild(bar);
        btn.appendChild(name);
        btn.addEventListener("click", () => goTo(i));
        li.appendChild(btn);
        railSteps.appendChild(li);
    });
}

function paintRail() {
    [...railSteps.querySelectorAll(".demo-rail__step")].forEach((btn, i) => {
        btn.classList.toggle("is-current", i === current);
        btn.classList.toggle("is-done", i < current);
    });
}

function goTo(index) {
    if (index < 0 || index >= STEPS.length) return;
    current = index;
    const step = STEPS[index];

    stepIndex.textContent = String(index + 1);
    stepTitle.textContent = step.title;
    stepLead.textContent  = step.lead;
    stepBody.textContent  = step.body;
    stepFact.textContent  = step.fact || "";
    stepFact.style.display = step.fact ? "" : "none";

    if (step.parallel) {
        stepParallel.textContent = step.parallel;
        stepParallel.classList.remove("is-hidden");
    } else {
        stepParallel.classList.add("is-hidden");
    }

    const ms = stepMsOf(step);
    stepMs.textContent = fmtMs(ms);
    stepCum.textContent = fmtMs(serialCumThrough(index));
    stepFree.classList.toggle("is-hidden", !step.parallel);

    stepStages.innerHTML = "";
    step.stages.forEach((name) => {
        const li = document.createElement("li");
        const n = document.createElement("span");
        n.textContent = name;
        const v = document.createElement("span");
        v.textContent = fmtMs(timingMap.get(name));
        li.appendChild(n);
        li.appendChild(v);
        stepStages.appendChild(li);
    });

    applyVisual(step.visual);
    applyInset(step);
    paintRail();

    btnPrev.disabled = index === 0;
    btnNext.disabled = index === STEPS.length - 1;
    if (index === STEPS.length - 1) stopAuto();
}

function next() { if (current < STEPS.length - 1) goTo(current + 1); }
function prev() { if (current > 0) goTo(current - 1); }

function startAuto() {
    if (current >= STEPS.length - 1) goTo(0);
    stopAuto();
    btnAuto.setAttribute("aria-pressed", "true");
    autoStarted = Date.now();

    autoTick = setInterval(() => {
        const frac = Math.min(1, (Date.now() - autoStarted) / AUTOPLAY_MS);
        autoFill.style.width = `${(frac * 100).toFixed(1)}%`;
    }, 60);

    autoTimer = setInterval(() => {
        if (current >= STEPS.length - 1) { stopAuto(); return; }
        autoStarted = Date.now();
        next();
    }, AUTOPLAY_MS);
}

function stopAuto() {
    if (autoTimer) clearInterval(autoTimer);
    if (autoTick)  clearInterval(autoTick);
    autoTimer = null;
    autoTick = null;
    autoFill.style.width = "0%";
    btnAuto.setAttribute("aria-pressed", "false");
}

function toggleAuto() {
    if (autoTimer) stopAuto(); else startAuto();
}

function setBusy(on, text) {
    demoBusyText.textContent = text || "Working...";
    demoBusy.classList.toggle("is-hidden", !on);
}

async function loadSceneList() {
    try {
        const response = await fetch("/api/scenes");
        if (!response.ok) throw new Error(`Server answered ${response.status}`);
        const data = await response.json();
        const scenes = data.scenes || [];

        try {
            const seen = await (await fetch("/api/last-viewed")).json();
            lastViewedId = seen && seen.id ? seen.id : null;
        } catch (error) {
            lastViewedId = null;
        }

        startList.innerHTML = "";
        if (!scenes.length) {
            startSub.textContent = "The backend returned no scenes.";
            return;
        }
        startSub.textContent = `${scenes.length} scene${scenes.length === 1 ? "" : "s"} on the backend. Pick one.`;

        const ordered = lastViewedId
            ? [...scenes].sort((a, b) => (b.id === lastViewedId) - (a.id === lastViewedId))
            : scenes;

        ordered.forEach((s) => {
            const btn = document.createElement("button");
            btn.className = "demo-scene-btn";
            btn.textContent = sceneDisplayName(s);

            if (s.id === lastViewedId) {
                const badge = document.createElement("span");
                badge.className = "demo-scene-btn__badge";
                badge.textContent = "last viewed";
                btn.appendChild(badge);
            }

            const small = document.createElement("small");
            const place = scenePlace(s);
            small.textContent = place ? place.coords : s.id;
            btn.appendChild(small);
            btn.addEventListener("click", () => {
                pickedId = s.id;
                [...startList.children].forEach((c) => c.classList.remove("is-picked"));
                btn.classList.add("is-picked");
                btnStart.disabled = false;
            });
            startList.appendChild(btn);
        });
    } catch (error) {
        startSub.textContent = `Could not reach the backend: ${error.message}`;
    }
}

async function beginDemo() {
    if (!pickedId) return;
    demoStart.classList.add("is-hidden");

    try {
        setBusy(true, "Fetching the run from the backend");
        const response = await fetch(`/api/scenes/${encodeURIComponent(pickedId)}`);
        if (!response.ok) throw new Error(`Server answered ${response.status}`);
        scene = await response.json();
    } catch (error) {
        setBusy(false);
        demoStart.classList.remove("is-hidden");
        startSub.textContent = `Could not start: ${error.message}`;
        return;
    }

    setBusy(false);

    timingMap = new Map((scene.timings || []).map((t) => [t.stage, t.ms]));
    assets = SCENE_ASSETS[scene.id] || {};
    sceneLabel.textContent = sceneDisplayName(scene);
    btnRestart.hidden = false;
    stepCount.textContent = String(STEPS.length);

    clearScene();
    fitToScene();
    buildRail();
    goTo(0);
}

function restart() {
    stopAuto();
    current = -1;
    scene = null;
    clearScene();
    btnRestart.hidden = true;
    sceneLabel.textContent = "";
    demoStart.classList.remove("is-hidden");
    loadSceneList();
}

btnStart.addEventListener("click", beginDemo);
btnNext.addEventListener("click", () => { stopAuto(); next(); });
btnPrev.addEventListener("click", () => { stopAuto(); prev(); });
btnAuto.addEventListener("click", toggleAuto);
btnRestart.addEventListener("click", restart);

document.addEventListener("keydown", (event) => {
    if (!demoStart.classList.contains("is-hidden")) return;
    if (event.key === "ArrowRight" || event.key === " " || event.key === "PageDown") {
        event.preventDefault(); stopAuto(); next();
    } else if (event.key === "ArrowLeft" || event.key === "PageUp") {
        event.preventDefault(); stopAuto(); prev();
    } else if (event.key === "Home") {
        event.preventDefault(); stopAuto(); goTo(0);
    } else if (event.key === "End") {
        event.preventDefault(); stopAuto(); goTo(STEPS.length - 1);
    }
});

initMap();
stepCount.textContent = String(STEPS.length);
loadSceneList();
