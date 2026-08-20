const mapBoard              = document.getElementById("mapBoard");

const statsBar              = document.getElementById("statsBar");
const statTotalShips        = document.getElementById("statTotalShips");
const statDarkVessels       = document.getElementById("statDarkVessels");
const statSceneName         = document.getElementById("statSceneName");
const btnCloseScene         = document.getElementById("btnCloseScene");

const notice                = document.getElementById("notice");
const noticeText            = document.getElementById("noticeText");

const shipCard              = document.getElementById("shipCard");
const shipCardRows          = document.getElementById("shipCardRows");
const btnCloseShipCard      = document.getElementById("btnCloseShipCard");

const scenePicker           = document.getElementById("scenePicker");
const pickerBody            = document.getElementById("pickerBody");
const btnClosePicker        = document.getElementById("btnClosePicker");

const btnPickScene          = document.getElementById("btnPickScene");
const btnSarOverlay         = document.getElementById("btnSarOverlay");
const switchDarkOnly        = document.getElementById("switchDarkOnly");
const switchDarkOnlyGroup   = document.getElementById("switchDarkOnlyGroup");

const btnOpenMenu           = document.getElementById("btnOpenMenu");
const btnCloseMenu          = document.getElementById("btnCloseMenu");
const sidebar               = document.getElementById("sidebar");
const sidebarBackdrop       = document.getElementById("sidebarBackdrop");
const btnMenuScenes         = document.getElementById("btnMenuScenes");

const popupOverlay          = document.getElementById("popupOverlay");
const popupMessage          = document.getElementById("popupMessage");
const btnClosePopup         = document.getElementById("btnClosePopup");

const loaderOverlay         = document.getElementById("loaderOverlay");
const loaderText            = document.getElementById("loaderText");

let map               = null;
let scenePickerLayer  = null;
let sceneOutlineLayer = null;
let vesselBoxLayer    = null;
let vesselDotLayer    = null;
let sarOverlayLayer   = null;

let cachedSceneList     = null;
let isPickingScene      = false;
let selectedScene       = null;
let showOnlyDarkVessels = false;
let sarOverlayOn        = false;
let openVesselId        = null;
let vesselDotsById      = new Map();
let vesselBoxesById     = new Map();
let imageFootprints     = new Map();
let processingImage     = null;

const DOT_HIDE_FROM_ZOOM = 12;

let noticeHideTimer = null;
let loaderShownAt   = 0;

function paletteColor(variableName) {
    return getComputedStyle(document.documentElement)
        .getPropertyValue(variableName)
        .trim();
}

function notify(message, kind = "info", hideByItself = true) {
    clearTimeout(noticeHideTimer);

    noticeText.textContent = message;

    notice.classList.remove("notice--success", "notice--error",
                            "notice--info", "notice--pending");
    const knownKinds = ["success", "error", "info", "pending"];
    notice.classList.add("notice--" + (knownKinds.includes(kind) ? kind : "info"));

    notice.classList.remove("is-hidden");

    notice.classList.remove("is-entering");
    void notice.offsetWidth;
    notice.classList.add("is-entering");

    if (hideByItself) {
        noticeHideTimer = setTimeout(hideNotice, 3800);
    }
}

function hideNotice() {
    clearTimeout(noticeHideTimer);
    notice.classList.add("is-hidden");
}

function showPopup(message) {
    popupMessage.textContent = message;
    popupOverlay.classList.remove("is-hidden");
}

function hidePopup() {
    popupOverlay.classList.add("is-hidden");
}

function showLoader(message) {
    if (typeof message === "string") {
        loaderText.textContent = message;
    }
    loaderShownAt = Date.now();
    loaderOverlay.classList.remove("is-hidden");
}

function hideLoader() {
    loaderOverlay.classList.add("is-hidden");
    loaderText.textContent = BAKLAVA_SETTINGS.loading_text || "";
}

function pause(milliseconds) {
    return new Promise(done => setTimeout(done, milliseconds));
}

async function runWithLoader(job, message) {
    showLoader(message);
    try {
        return await job();
    } finally {
        const shownForMs = Date.now() - loaderShownAt;
        const minimumMs  = BAKLAVA_SETTINGS.loading_min_ms || 0;
        if (shownForMs < minimumMs) {
            await pause(minimumMs - shownForMs);
        }
        hideLoader();
    }
}

function openSidebar() {
    sidebar.classList.add("is-open");
    sidebarBackdrop.classList.remove("is-hidden");
}

function closeSidebar() {
    sidebar.classList.remove("is-open");
    sidebarBackdrop.classList.add("is-hidden");
}

function initMap() {
    map = L.map("map", {
        zoomControl: true,
        attributionControl: true,
    }).setView(
        [BAKLAVA_SETTINGS.map_default_lat, BAKLAVA_SETTINGS.map_default_lon],
        BAKLAVA_SETTINGS.map_default_zoom
    );

    L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
        maxZoom: 18,
        attribution: "&copy; OpenStreetMap contributors"
    }).addTo(map);

    sarOverlayLayer   = L.layerGroup().addTo(map);
    scenePickerLayer  = L.featureGroup().addTo(map);
    sceneOutlineLayer = L.layerGroup().addTo(map);
    vesselBoxLayer    = L.layerGroup().addTo(map);
    vesselDotLayer    = L.layerGroup().addTo(map);

    map.on("click", hideShipCard);
    map.on("zoomend", updateVesselDotVisibility);
}

function focusMapOn(lat, lon, zoom = 8) {
    map.setView([lat, lon], zoom);
}

async function loadSceneList({ refresh = false } = {}) {
    if (cachedSceneList && !refresh) {
        return cachedSceneList;
    }

    try {
        const response = await fetch("/api/scenes");
        const data     = await response.json();
        cachedSceneList = data.scenes || [];
        return cachedSceneList;
    } catch (error) {
        notify(BAKLAVA_SETTINGS.msg_scenes_failed, "error");
        console.error(error);
        return [];
    }
}

async function loadAvailableImages() {
    try {
        const response = await fetch("/api/scenes/available", { method: "POST" });
        if (!response.ok) {
            throw new Error(`Server answered ${response.status}`);
        }
        const rows = await response.json();
        if (!Array.isArray(rows)) return [];

        imageFootprints = new Map();
        return rows.map(row => {
            if (typeof row === "string") return row;
            if (row.corners) imageFootprints.set(row.name, row.corners);
            return row.name;
        });
    } catch (error) {
        notify(BAKLAVA_SETTINGS.msg_images_failed, "error");
        console.error(error);
        return [];
    }
}

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

function sceneDisplayName(scene) {
    const place = scenePlace(scene);
    const when  = sceneWhen(scene.id) || scene.label;
    if (!place) {
        return when;
    }
    return `${place.region || place.coords} — ${when}`;
}

const S1_NAME = /^(S1[A-D])_([A-Z]{2})_([A-Z]{4})_[A-Z0-9]{4}_(\d{8})T(\d{6})_/;

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

function imageDisplayName(imageName) {
    const parts = sentinelParts(imageName);
    if (!parts) {
        return imageName;
    }
    return `Sentinel-1${parts.mission.slice(2)} — ${formatWhen(parts.when)}`;
}

function isSafeProduct(imageName) {
    return imageName.toLowerCase().endsWith(".safe");
}

function imagePlace(imageName) {
    const corners = imageFootprints.get(imageName);
    if (!corners) return null;
    const centre = sceneCentre(corners);
    if (!centre) return null;
    return {
        region: regionName(centre[0], centre[1]),
        coords: shortCoords(centre[0], centre[1]),
    };
}

function imageDisplayLabel(imageName) {
    const place = imagePlace(imageName);
    const parts = sentinelParts(imageName);
    const when  = parts ? formatWhen(parts.when) : imageName;
    if (!place) return imageDisplayName(imageName);
    return `${place.region || place.coords} \u2014 ${when}`;
}

function sceneIdForImage(imageName) {
    return imageName.replace(/\.[^.]+$/, "") + "__cpp";
}

function sceneBoxStyle(state) {
    const token = state === "todo" ? "--color-scene-todo"
                : state === "busy" ? "--color-scene-busy"
                : "--color-scene";
    return {
        color: paletteColor(token),
        weight: 2,
        dashArray: state === "todo" ? "7 5" : null,
        fillColor: paletteColor(token),
        fillOpacity: state === "busy" ? 0.3 : 0.18,
    };
}

function drawScenePickerBoxes(scenes, images = []) {
    scenePickerLayer.clearLayers();

    scenes.forEach(scene => {
        const box = L.polygon(scene.corners, sceneBoxStyle("done"));
        box.bindTooltip(sceneDisplayName(scene), { sticky: true });
        box.on("click", () => selectScene(scene.id));
        box.addTo(scenePickerLayer);
    });

    images.forEach(imageName => {
        const corners = imageFootprints.get(imageName);
        if (!corners) return;

        const busy = processingImage === imageName;
        const box = L.polygon(corners, sceneBoxStyle(busy ? "busy" : "todo"));
        const label = imageDisplayLabel(imageName);

        box.bindTooltip(busy ? `${label} \u2014 processing\u2026`
                             : `${label} \u2014 not processed yet`, { sticky: true });
        if (!busy) box.on("click", () => confirmProcess(imageName));
        box.addTo(scenePickerLayer);
    });
}

function addPickerGroup(label) {
    const heading = document.createElement("p");
    heading.className   = "picker__group-label";
    heading.textContent = label;
    pickerBody.appendChild(heading);
}

function addPickerNote(text) {
    const note = document.createElement("p");
    note.className   = "picker__empty";
    note.textContent = text;
    pickerBody.appendChild(note);
}

function addPickerItem({ name, note, tag, tagKind, onPick }) {
    const item = document.createElement("button");
    item.className = "picker__item";
    item.type      = "button";

    const text = document.createElement("span");
    text.className = "picker__item-text";

    const title = document.createElement("span");
    title.className   = "picker__item-name";
    title.textContent = name;
    text.appendChild(title);

    if (note) {
        const sub = document.createElement("span");
        sub.className   = "picker__item-note";
        sub.textContent = note;
        text.appendChild(document.createElement("br"));
        text.appendChild(sub);
    }

    item.appendChild(text);

    if (tag) {
        const badge = document.createElement("span");
        badge.className   = "picker__tag" + (tagKind ? ` picker__tag--${tagKind}` : "");
        badge.textContent = tag;
        item.appendChild(badge);
    }

    item.addEventListener("click", onPick);
    pickerBody.appendChild(item);
    return item;
}

function renderPicker(scenes, images) {
    pickerBody.innerHTML = "";

    if (!scenes.length && !images.length) {
        addPickerNote(BAKLAVA_SETTINGS.picker_empty);
        return;
    }

    addPickerGroup(BAKLAVA_SETTINGS.picker_available_label);
    if (!images.length) {
        addPickerNote(BAKLAVA_SETTINGS.picker_no_available);
    } else {
        images.forEach(image => {
            const place = imagePlace(image);
            const parts = sentinelParts(image);
            addPickerItem({
                name: imageDisplayLabel(image),
                note: place ? place.coords : (parts ? `${parts.mode} ${parts.product}` : image),
                tag: isSafeProduct(image) ? "SAFE" : "TIF",
                tagKind: isSafeProduct(image) ? "safe" : null,
                onPick: () => confirmProcess(image),
            });
        });
    }

    addPickerGroup(BAKLAVA_SETTINGS.picker_processed_label);
    if (!scenes.length) {
        addPickerNote(BAKLAVA_SETTINGS.picker_no_processed);
    } else {
        scenes.forEach(scene => {
            const place = scenePlace(scene);
            addPickerItem({
                name: sceneDisplayName(scene),
                note: place && place.region ? place.coords : null,
                onPick: () => selectScene(scene.id),
            });
        });
    }
}

function openPicker() {
    scenePicker.classList.remove("is-hidden");
}

function closePicker() {
    scenePicker.classList.add("is-hidden");
}

async function startScenePicking({ refresh = false } = {}) {
    if (selectedScene) {
        closeScene({ withoutReopeningPicker: true });
    }

    const [scenes, allImages] = await runWithLoader(() => Promise.all([
        loadSceneList({ refresh }),
        loadAvailableImages(),
    ]));

    const done = new Set(scenes.map(scene => scene.id));
    const images = allImages.filter(name => !done.has(sceneIdForImage(name)));

    isPickingScene = true;
    btnPickScene.classList.add("is-active");
    btnPickScene.setAttribute("aria-pressed", "true");
    mapBoard.classList.add("is-picking-scene");

    renderPicker(scenes, images);
    openPicker();

    drawScenePickerBoxes(scenes, images);

    map.invalidateSize();
    if (scenes.length || scenePickerLayer.getLayers().length) {
        map.fitBounds(scenePickerLayer.getBounds(), { padding: [40, 40] });
        notify(BAKLAVA_SETTINGS.msg_pick_scene, "info");
    }
}

function confirmProcess(imageName) {
    const label = imageDisplayLabel(imageName);
    if (!window.confirm(
            `${BAKLAVA_SETTINGS.confirm_process_title}\n\n${label}\n\n`
            + BAKLAVA_SETTINGS.confirm_process_body)) {
        return;
    }
    processImage(imageName);
}

async function processImage(imageName) {
    closePicker();

    processingImage = imageName;
    drawScenePickerBoxes(cachedSceneList || [], [...imageFootprints.keys()]);

    let failure = null;
    await runWithLoader(async () => {
        try {
            const response = await fetch(
                `/api/scenes/${encodeURIComponent(imageName)}/process`,
                { method: "POST" });

            if (!response.ok) {
                const problem = await response.json().catch(() => null);
                failure = (problem && problem.error) || `Server answered ${response.status}`;
            }
        } catch (error) {
            failure = error.message;
            console.error(error);
        }
    }, BAKLAVA_SETTINGS.msg_processing);

    processingImage = null;

    const sceneId = sceneIdForImage(imageName);
    const scenes  = await loadSceneList({ refresh: true });
    const landed  = scenes.some(scene => scene.id === sceneId);

    if (landed) {
        notify(`${BAKLAVA_SETTINGS.msg_process_done} ${imageDisplayName(imageName)}`, "success");
        await selectScene(sceneId);
        return;
    }

    showPopup(`${BAKLAVA_SETTINGS.msg_process_failed}\n\n${failure || imageDisplayName(imageName)}`);
    startScenePicking();
}

function stopScenePicking(options = {}) {
    isPickingScene = false;
    btnPickScene.classList.remove("is-active");
    btnPickScene.setAttribute("aria-pressed", "false");
    mapBoard.classList.remove("is-picking-scene");

    closePicker();
    scenePickerLayer.clearLayers();

    if (!options.withoutNotice) {
        notify(BAKLAVA_SETTINGS.msg_pick_scene_off, "info");
    }
}

async function selectScene(sceneId) {
    let scene;
    try {
        scene = await runWithLoader(async () => {
            const response = await fetch(`/api/scenes/${sceneId}`);
            if (!response.ok) {
                throw new Error(`Server answered ${response.status}`);
            }
            return response.json();
        });
    } catch (error) {
        notify(BAKLAVA_SETTINGS.msg_scene_failed, "error");
        console.error(error);
        return;
    }

    selectedScene = scene;

    stopScenePicking({ withoutNotice: true });
    hideShipCard();
    setSarOverlay(false, { withoutNotice: true });

    sceneOutlineLayer.clearLayers();
    const outline = L.polygon(scene.corners, {
        color: paletteColor("--color-scene-picked"),
        weight: 2,
        dashArray: "2 6",
        lineCap: "round",
        fill: false,
        interactive: false,
    });
    outline.addTo(sceneOutlineLayer);

    map.invalidateSize();
    map.fitBounds(outline.getBounds(), { padding: [30, 30] });

    statTotalShips.textContent  = scene.totals.total;
    statDarkVessels.textContent = scene.totals.dark;
    statSceneName.textContent   = sceneDisplayName(scene);
    statsBar.classList.remove("is-hidden");

    setSceneControlsEnabled(true);
    setShowOnlyDarkVessels(true);
}

function closeScene(options = {}) {
    hideShipCard();

    selectedScene = null;

    sceneOutlineLayer.clearLayers();
    vesselDotLayer.clearLayers();
    vesselBoxLayer.clearLayers();
    vesselDotsById.clear();
    vesselBoxesById.clear();
    setSarOverlay(false, { withoutNotice: true });

    statsBar.classList.add("is-hidden");
    setShowOnlyDarkVessels(false);
    setSceneControlsEnabled(false);

    if (!options.withoutReopeningPicker) {
        startScenePicking();
    }
}

function setSceneControlsEnabled(enabled) {
    btnSarOverlay.disabled  = !enabled;
    switchDarkOnly.disabled = !enabled;
    switchDarkOnlyGroup.classList.toggle("is-disabled", !enabled);
}

function setShowOnlyDarkVessels(on) {
    showOnlyDarkVessels = on;
    switchDarkOnly.setAttribute("aria-checked", on ? "true" : "false");
    drawVessels();
}

function vesselDotStyle(vessel, isOpenInCard) {
    return {
        radius: isOpenInCard ? 9 : 6,
        color: vessel.dark ? paletteColor("--color-vessel-dark")
                           : paletteColor("--color-vessel-safe"),
        fillColor: vessel.dark ? paletteColor("--color-vessel-dark") : "#FFFFFF",
        fillOpacity: 1,
        weight: isOpenInCard ? 4 : 2,
    };
}

function vesselBoxStyle(vessel, isOpenInCard) {
    return {
        color: vessel.dark ? paletteColor("--color-vessel-dark")
                           : paletteColor("--color-vessel-safe"),
        weight: isOpenInCard ? 3 : 2,
        opacity: 1,
        fill: true,
        fillOpacity: isOpenInCard ? 0.25 : 0,
    };
}

function sceneHasVesselBoxes() {
    return Boolean(selectedScene)
        && selectedScene.vessels.some(vessel => vessel.corners);
}

function updateVesselDotVisibility() {
    if (!map || !vesselDotLayer) {
        return;
    }
    const hide = sceneHasVesselBoxes()
        && (sarOverlayOn || map.getZoom() >= DOT_HIDE_FROM_ZOOM);
    if (hide && map.hasLayer(vesselDotLayer)) {
        map.removeLayer(vesselDotLayer);
    } else if (!hide && !map.hasLayer(vesselDotLayer)) {
        vesselDotLayer.addTo(map);
    }
}

function drawVessels() {
    vesselDotLayer.clearLayers();
    vesselBoxLayer.clearLayers();
    vesselDotsById.clear();
    vesselBoxesById.clear();

    if (!selectedScene) {
        return;
    }

    const vesselsToDraw = showOnlyDarkVessels
        ? selectedScene.vessels.filter(vessel => vessel.dark)
        : selectedScene.vessels;

    vesselsToDraw.forEach(vessel => {
        const isOpen = vessel.id === openVesselId;

        if (vessel.corners) {
            const box = L.polygon(
                vessel.corners,
                Object.assign(vesselBoxStyle(vessel, isOpen), {
                    bubblingMouseEvents: false,
                })
            );

            box.on("click", () => showShipCard(vessel));

            box.addTo(vesselBoxLayer);
            vesselBoxesById.set(vessel.id, box);
        }

        const dot = L.circleMarker(
            [vessel.lat, vessel.lon],
            Object.assign(vesselDotStyle(vessel, isOpen), {
                bubblingMouseEvents: false,
            })
        );

        dot.on("click", () => showShipCard(vessel));

        dot.addTo(vesselDotLayer);
        vesselDotsById.set(vessel.id, dot);
    });

    updateVesselDotVisibility();

    if (openVesselId !== null && !vesselDotsById.has(openVesselId)) {
        hideShipCard();
    }
}

function showShipCard(vessel) {
    highlightVesselDot(openVesselId, false);
    openVesselId = vessel.id;
    highlightVesselDot(vessel.id, true);

    const rows = [
        ["Status",     vessel.dark ? "Dark vessel (no AIS)" : "AIS reported"],
        ["Latitude",   formatCoordinate(vessel.lat, "N", "S")],
        ["Longitude",  formatCoordinate(vessel.lon, "E", "W")],
        ["MMSI",       vessel.mmsi || "—"],
        ["Type",       vessel.type],
        ["Length",     vessel.length_m ? `${vessel.length_m} m` : ""],
        ["Heading",    vessel.heading_deg != null ? `${vessel.heading_deg}°` : ""],
        ["Speed",      vessel.speed_kn != null ? `${vessel.speed_kn} kn` : ""],
        ["Detected",   vessel.detected_at],
        ["Confidence", vessel.confidence != null
                        ? `${Math.round(vessel.confidence * 100)} %` : ""],
    ];

    shipCardRows.innerHTML = "";

    if (vessel.name) {
        const name = document.createElement("p");
        name.className   = "ship-card__name";
        name.textContent = vessel.name;
        shipCardRows.appendChild(name);
    }

    rows.forEach(([label, value]) => {
        if (value === "" || value == null) {
            return;
        }

        const term = document.createElement("dt");
        term.textContent = label;

        const detail = document.createElement("dd");
        detail.textContent = value;

        if (label === "Status") {
            detail.classList.add(vessel.dark ? "is-dark-vessel" : "is-safe-vessel");
        }

        shipCardRows.appendChild(term);
        shipCardRows.appendChild(detail);
    });

    shipCard.classList.remove("is-hidden");
}

function hideShipCard() {
    shipCard.classList.add("is-hidden");
    highlightVesselDot(openVesselId, false);
    openVesselId = null;
}

function highlightVesselDot(vesselId, on) {
    if (vesselId === null || !selectedScene) {
        return;
    }

    const vessel = selectedScene.vessels.find(item => item.id === vesselId);
    if (!vessel) {
        return;
    }

    const dot = vesselDotsById.get(vesselId);
    if (dot) {
        const style = vesselDotStyle(vessel, on);
        dot.setStyle(style);
        dot.setRadius(style.radius);
    }

    const box = vesselBoxesById.get(vesselId);
    if (box) {
        box.setStyle(vesselBoxStyle(vessel, on));
        if (on) {
            box.bringToFront();
        }
    }
}

function formatCoordinate(value, positiveLetter, negativeLetter) {
    if (value == null) {
        return "";
    }
    const letter = value >= 0 ? positiveLetter : negativeLetter;
    return `${Math.abs(value).toFixed(6)}° ${letter}`;
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

function mercatorY(lat) {
    return Math.log(Math.tan(Math.PI / 4 + lat * Math.PI / 360));
}

function swathClipPath(bounds, swath) {
    if (!swath || swath.length < 3) {
        return "";
    }

    const latTop = bounds[0][0], latBottom = bounds[2][0];
    const lonLeft = bounds[0][1], lonRight = bounds[1][1];
    const yTop = mercatorY(latTop), yBottom = mercatorY(latBottom);

    const points = swath.map(corner => {
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
        img.style.position       = "absolute";
        img.style.left           = "0";
        img.style.top            = "0";
        img.style.transformOrigin = "0 0";
        img.style.pointerEvents  = "none";
        img.style.opacity        = String(this._opacity);
        img.style.clipPath       = swathClipPath(this._quad, this._swath);
        img.alt                  = "";
        img.onload = () => this._warp();
        img.src = this._url;

        this._img = img;
        map.getPanes().overlayPane.appendChild(img);
        map.on("viewreset zoom move zoomend moveend", this._warp, this);
        this._warp();
    },

    onRemove(map) {
        L.DomUtil.remove(this._img);
        map.off("viewreset zoom move zoomend moveend", this._warp, this);
        this._img = null;
    },

    _warp() {
        const img = this._img;
        if (!img || !img.naturalWidth || !this._map) {
            return;
        }

        const w = img.naturalWidth;
        const h = img.naturalHeight;
        const points = this._quad.map(
            corner => this._map.latLngToLayerPoint(L.latLng(corner[0], corner[1])));

        const m = squareToQuad(points);

        img.style.transform = "matrix3d(" + [
            m.a / w, m.d / w, 0, m.g / w,
            m.b / h, m.e / h, 0, m.h / h,
            0,       0,       1, 0,
            m.c,     m.f,     0, 1,
        ].join(",") + ")";
    },
});

function setSarOverlay(on, options = {}) {
    sarOverlayLayer.clearLayers();
    sarOverlayOn = false;
    btnSarOverlay.classList.toggle("is-active", on);
    btnSarOverlay.setAttribute("aria-pressed", on ? "true" : "false");

    if (!on) {
        updateVesselDotVisibility();
        return;
    }

    if (!selectedScene) {
        btnSarOverlay.classList.remove("is-active");
        btnSarOverlay.setAttribute("aria-pressed", "false");
        updateVesselDotVisibility();
        if (!options.withoutNotice) {
            notify(BAKLAVA_SETTINGS.msg_sar_needs_scene, "info");
        }
        return;
    }

    const radarPicture = selectedScene.sar_overlay;

    if (radarPicture && radarPicture.url) {
        const quad = radarPicture.corners || selectedScene.corners;
        new SarQuadOverlay(radarPicture.url, quad, 1, radarPicture.swath)
            .addTo(sarOverlayLayer);
        sarOverlayOn = true;
        updateVesselDotVisibility();
        return;
    }

    updateVesselDotVisibility();

    L.tileLayer("https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png", {
        bounds: L.latLngBounds(selectedScene.corners),
        opacity: 0.9,
        maxZoom: 18,
        attribution: "&copy; CARTO",
    }).addTo(sarOverlayLayer);

    if (!options.withoutNotice) {
        notify(BAKLAVA_SETTINGS.msg_no_sar, "info");
    }
}

btnPickScene.addEventListener("click", () => {
    if (isPickingScene) {
        stopScenePicking();
    } else {
        startScenePicking();
    }
});

btnSarOverlay.addEventListener("click", () => {
    setSarOverlay(!btnSarOverlay.classList.contains("is-active"));
});

switchDarkOnly.addEventListener("click", () => {
    if (!selectedScene) {
        return;
    }
    setShowOnlyDarkVessels(switchDarkOnly.getAttribute("aria-checked") !== "true");
});

btnCloseScene.addEventListener("click", () => closeScene());

btnCloseShipCard.addEventListener("click", hideShipCard);

btnClosePicker.addEventListener("click", () => stopScenePicking());

btnOpenMenu.addEventListener("click", openSidebar);
btnCloseMenu.addEventListener("click", closeSidebar);
sidebarBackdrop.addEventListener("click", closeSidebar);

btnMenuScenes.addEventListener("click", closeSidebar);

btnClosePopup.addEventListener("click", hidePopup);
popupOverlay.addEventListener("click", (event) => {
    if (event.target === popupOverlay) {
        hidePopup();
    }
});

document.addEventListener("keydown", (event) => {
    if (event.key !== "Escape") {
        return;
    }
    hidePopup();
    closeSidebar();
    hideShipCard();
    hideNotice();
    if (isPickingScene) {
        stopScenePicking();
    }
});

notice.addEventListener("click", hideNotice);

initMap();

window.BAKLAVA = {
    startScenePicking,
    stopScenePicking,
    loadAvailableImages,
    drawScenePickerBoxes,
    confirmProcess,
    processImage,
    selectScene,
    closeScene,
    setShowOnlyDarkVessels,
    setSceneControlsEnabled,
    setSarOverlay,
    showShipCard,
    hideShipCard,
    focusMapOn,
    notify,
    hideNotice,
    showPopup,
    showLoader,
    hideLoader,
    runWithLoader,
    get map()    { return map; },
    get scene()  { return selectedScene; },
    get scenes() { return cachedSceneList; },
    get footprints() { return imageFootprints; },
    get processing() { return processingImage; },
    set processing(name) { processingImage = name; },
};
