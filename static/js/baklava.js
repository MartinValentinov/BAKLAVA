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

const btnPickScene          = document.getElementById("btnPickScene");
const btnSarOverlay         = document.getElementById("btnSarOverlay");
const switchDarkOnly        = document.getElementById("switchDarkOnly");
const switchDarkOnlyGroup   = document.getElementById("switchDarkOnlyGroup");

const btnOpenMenu           = document.getElementById("btnOpenMenu");
const btnCloseMenu          = document.getElementById("btnCloseMenu");
const sidebar               = document.getElementById("sidebar");
const sidebarBackdrop       = document.getElementById("sidebarBackdrop");
const btnMenuScenes         = document.getElementById("btnMenuScenes");
const btnMenuDarkAlerts     = document.getElementById("btnMenuDarkAlerts");

const popupOverlay          = document.getElementById("popupOverlay");
const popupMessage          = document.getElementById("popupMessage");
const btnClosePopup         = document.getElementById("btnClosePopup");

const loaderOverlay         = document.getElementById("loaderOverlay");
const loaderText            = document.getElementById("loaderText");

let map               = null;
let scenePickerLayer  = null;
let sceneOutlineLayer = null;
let vesselDotLayer    = null;
let sarOverlayLayer   = null;

let cachedSceneList     = null;
let isPickingScene      = false;
let selectedScene       = null;
let showOnlyDarkVessels = false;
let openVesselId        = null;
let vesselDotsById      = new Map();

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
    vesselDotLayer    = L.layerGroup().addTo(map);

    map.on("click", hideShipCard);
}

function focusMapOn(lat, lon, zoom = 8) {
    map.setView([lat, lon], zoom);
}

async function loadSceneList() {
    if (cachedSceneList) {
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

function drawScenePickerBoxes(scenes) {
    scenePickerLayer.clearLayers();

    scenes.forEach(scene => {
        const box = L.polygon(scene.corners, {
            color: paletteColor("--color-scene"),
            weight: 2,
            fillColor: paletteColor("--color-scene"),
            fillOpacity: 0.18,
        });

        box.bindTooltip(scene.label, { sticky: true });
        box.on("click", () => selectScene(scene.id));
        box.addTo(scenePickerLayer);
    });
}

async function startScenePicking() {
    if (selectedScene) {
        closeScene({ withoutReopeningPicker: true });
    }

    const scenes = await runWithLoader(loadSceneList);
    if (!scenes.length) {
        return;
    }

    isPickingScene = true;
    btnPickScene.classList.add("is-active");
    btnPickScene.setAttribute("aria-pressed", "true");
    mapBoard.classList.add("is-picking-scene");

    drawScenePickerBoxes(scenes);

    map.invalidateSize();
    map.fitBounds(scenePickerLayer.getBounds(), { padding: [40, 40] });

    notify(BAKLAVA_SETTINGS.msg_pick_scene, "info");
}

function stopScenePicking(options = {}) {
    isPickingScene = false;
    btnPickScene.classList.remove("is-active");
    btnPickScene.setAttribute("aria-pressed", "false");
    mapBoard.classList.remove("is-picking-scene");

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
    statSceneName.textContent   = scene.label;
    statsBar.classList.remove("is-hidden");

    setSceneControlsEnabled(true);
    setShowOnlyDarkVessels(true);
}

function closeScene(options = {}) {
    hideShipCard();

    selectedScene = null;

    sceneOutlineLayer.clearLayers();
    vesselDotLayer.clearLayers();
    vesselDotsById.clear();
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

function drawVessels() {
    vesselDotLayer.clearLayers();
    vesselDotsById.clear();

    if (!selectedScene) {
        return;
    }

    const vesselsToDraw = showOnlyDarkVessels
        ? selectedScene.vessels.filter(vessel => vessel.dark)
        : selectedScene.vessels;

    vesselsToDraw.forEach(vessel => {
        const dot = L.circleMarker(
            [vessel.lat, vessel.lon],
            Object.assign(vesselDotStyle(vessel, vessel.id === openVesselId), {
                bubblingMouseEvents: false,
            })
        );

        dot.on("click", () => showShipCard(vessel));

        dot.addTo(vesselDotLayer);
        vesselDotsById.set(vessel.id, dot);
    });

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

    const dot    = vesselDotsById.get(vesselId);
    const vessel = selectedScene.vessels.find(item => item.id === vesselId);
    if (!dot || !vessel) {
        return;
    }

    const style = vesselDotStyle(vessel, on);
    dot.setStyle(style);
    dot.setRadius(style.radius);
}

function formatCoordinate(value, positiveLetter, negativeLetter) {
    if (value == null) {
        return "";
    }
    const letter = value >= 0 ? positiveLetter : negativeLetter;
    return `${Math.abs(value).toFixed(6)}° ${letter}`;
}

function setSarOverlay(on, options = {}) {
    sarOverlayLayer.clearLayers();
    btnSarOverlay.classList.toggle("is-active", on);
    btnSarOverlay.setAttribute("aria-pressed", on ? "true" : "false");

    if (!on) {
        return;
    }

    if (!selectedScene) {
        btnSarOverlay.classList.remove("is-active");
        btnSarOverlay.setAttribute("aria-pressed", "false");
        if (!options.withoutNotice) {
            notify(BAKLAVA_SETTINGS.msg_sar_needs_scene, "info");
        }
        return;
    }

    const radarPicture = selectedScene.sar_overlay;

    if (radarPicture && radarPicture.url) {
        L.imageOverlay(radarPicture.url,
                       L.latLngBounds(radarPicture.corners || selectedScene.corners),
                       { opacity: 0.85 })
         .addTo(sarOverlayLayer);
        return;
    }

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

btnOpenMenu.addEventListener("click", openSidebar);
btnCloseMenu.addEventListener("click", closeSidebar);
sidebarBackdrop.addEventListener("click", closeSidebar);

btnMenuScenes.addEventListener("click", closeSidebar);
btnMenuDarkAlerts.addEventListener("click", () => {
    closeSidebar();
    showPopup("Under construction!");
});

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
});

notice.addEventListener("click", hideNotice);

initMap();

window.BAKLAVA = {
    startScenePicking,
    stopScenePicking,
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
};
