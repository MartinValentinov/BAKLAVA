
const board            = document.getElementById("board");

const statsBar         = document.getElementById("statsBar");
const statTotal        = document.getElementById("statTotal");
const statDark         = document.getElementById("statDark");
const statScene        = document.getElementById("statScene");
const btnCloseScene    = document.getElementById("btnCloseScene");

const notice           = document.getElementById("notice");
const noticeText       = document.getElementById("noticeText");

const shipCard         = document.getElementById("shipCard");
const shipRows         = document.getElementById("shipRows");
const btnCloseShip     = document.getElementById("btnCloseShip");

const btnSelect        = document.getElementById("btnSelect");
const btnSar           = document.getElementById("btnSar");
const switchDark       = document.getElementById("switchDark");
const switchWrap       = document.querySelector(".switch-wrap");

const btnHamburger     = document.getElementById("btnHamburger");
const btnSidebarClose  = document.getElementById("btnSidebarClose");
const sidebar          = document.getElementById("sidebar");
const sidebarBackdrop  = document.getElementById("sidebarBackdrop");
const btnSarScenes     = document.getElementById("btnSarScenes");
const btnDarkAlerts    = document.getElementById("btnDarkAlerts");

const popupOverlay     = document.getElementById("popupOverlay");
const popupMessage     = document.getElementById("popupMessage");
const popupClose       = document.getElementById("popupClose");

const loaderOverlay    = document.getElementById("loaderOverlay");
const loaderText       = document.getElementById("loaderText");

let map           = null;
let sceneLayer    = null;
let selectedLayer = null;
let vesselLayer   = null;
let sarLayer      = null;

let allScenes = null;

let isPicking = false;

let currentScene = null;

let showDark = false;

let selectedVesselId = null;

let vesselMarkers = new Map();

let noticeTimer = null;

let loaderShownAt = 0;

function cssColour(name) {
    return getComputedStyle(document.documentElement)
        .getPropertyValue(name)
        .trim();
}

function notify(message, kind = "info", autoHide = true) {
    clearTimeout(noticeTimer);

    noticeText.textContent = message;

    notice.classList.remove("notice--success", "notice--error",
                            "notice--info", "notice--pending");
    const known = ["success", "error", "info", "pending"];
    notice.classList.add("notice--" + (known.includes(kind) ? kind : "info"));

    notice.classList.remove("is-hidden");

    notice.classList.remove("is-entering");
    void notice.offsetWidth;
    notice.classList.add("is-entering");

    if (autoHide) {
        noticeTimer = setTimeout(hideNotice, 3800);
    }
}

function hideNotice() {
    clearTimeout(noticeTimer);
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

function wait(milliseconds) {
    return new Promise(done => setTimeout(done, milliseconds));
}

async function withLoader(job, message) {
    showLoader(message);
    try {
        return await job();
    } finally {
        const seenFor = Date.now() - loaderShownAt;
        const minimum = BAKLAVA_SETTINGS.loading_min_ms || 0;
        if (seenFor < minimum) {
            await wait(minimum - seenFor);
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

    sarLayer      = L.layerGroup().addTo(map);
    sceneLayer    = L.featureGroup().addTo(map);
    selectedLayer = L.layerGroup().addTo(map);
    vesselLayer   = L.layerGroup().addTo(map);

    map.on("click", hideShipCard);
}

function focusOn(lat, lon, zoom = 8) {
    map.setView([lat, lon], zoom);
}

async function fetchScenes() {
    if (allScenes) {
        return allScenes;
    }

    try {
        const response = await fetch("/api/scenes");
        const data     = await response.json();
        allScenes = data.scenes || [];
        return allScenes;
    } catch (error) {
        notify(BAKLAVA_SETTINGS.msg_scenes_failed, "error");
        console.error(error);
        return [];
    }
}

function drawScenes(scenes) {
    sceneLayer.clearLayers();

    scenes.forEach(scene => {
        const box = L.polygon(scene.corners, {
            color: cssColour("--color-scene"),
            weight: 2,
            fillColor: cssColour("--color-scene"),
            fillOpacity: 0.18,
        });

        box.bindTooltip(scene.label, { sticky: true });

        box.on("click", () => selectScene(scene.id));

        box.addTo(sceneLayer);
    });
}

async function startPicking() {
    if (currentScene) {
        closeScene({ silent: true });
    }

    const scenes = await withLoader(fetchScenes);
    if (!scenes.length) {
        return;
    }

    isPicking = true;
    btnSelect.classList.add("is-active");
    btnSelect.setAttribute("aria-pressed", "true");
    board.classList.add("is-picking");

    drawScenes(scenes);

    map.invalidateSize();
    map.fitBounds(sceneLayer.getBounds(), { padding: [40, 40] });

    notify(BAKLAVA_SETTINGS.msg_pick_scene, "info");
}

function stopPicking(options = {}) {
    isPicking = false;
    btnSelect.classList.remove("is-active");
    btnSelect.setAttribute("aria-pressed", "false");
    board.classList.remove("is-picking");

    sceneLayer.clearLayers();

    if (!options.silent) {
        notify(BAKLAVA_SETTINGS.msg_pick_scene_off, "info");
    }
}

async function selectScene(sceneId) {
    let scene;
    try {
        scene = await withLoader(async () => {
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

    currentScene = scene;

    stopPicking({ silent: true });
    hideShipCard();
    setSar(false, { silent: true });

    selectedLayer.clearLayers();
    const outline = L.polygon(scene.corners, {
        color: cssColour("--color-scene-picked"),
        weight: 2,
        dashArray: "2 6",
        lineCap: "round",
        fill: false,
        interactive: false,
    });
    outline.addTo(selectedLayer);

    map.invalidateSize();
    map.fitBounds(outline.getBounds(), { padding: [30, 30] });

    statTotal.textContent = scene.totals.total;
    statDark.textContent  = scene.totals.dark;
    statScene.textContent = scene.label;
    statsBar.classList.remove("is-hidden");

    setControlsEnabled(true);

    setDarkToggle(true);
}

function closeScene(options = {}) {
    hideShipCard();

    currentScene = null;

    selectedLayer.clearLayers();
    vesselLayer.clearLayers();
    vesselMarkers.clear();
    setSar(false, { silent: true });

    statsBar.classList.add("is-hidden");
    setDarkToggle(false);
    setControlsEnabled(false);

    if (!options.silent) {
        startPicking();
    }
}

function setControlsEnabled(enabled) {
    btnSar.disabled     = !enabled;
    switchDark.disabled = !enabled;

    switchWrap.classList.toggle("is-disabled", !enabled);
}

function setDarkToggle(on) {
    showDark = on;
    switchDark.setAttribute("aria-checked", on ? "true" : "false");
    renderVessels();
}

function vesselStyle(vessel, isSelected) {
    return {
        radius: isSelected ? 9 : 6,
        color: vessel.dark ? cssColour("--color-vessel-dark")
                           : cssColour("--color-vessel-safe"),
        fillColor: vessel.dark ? cssColour("--color-vessel-dark") : "#FFFFFF",
        fillOpacity: 1,
        weight: isSelected ? 4 : 2,
    };
}

function renderVessels() {
    vesselLayer.clearLayers();
    vesselMarkers.clear();

    if (!currentScene) {
        return;
    }

    const vessels = showDark
        ? currentScene.vessels.filter(vessel => vessel.dark)
        : currentScene.vessels;

    vessels.forEach(vessel => {
        const dot = L.circleMarker(
            [vessel.lat, vessel.lon],
            Object.assign(vesselStyle(vessel, vessel.id === selectedVesselId), {
                bubblingMouseEvents: false,
            })
        );

        dot.on("click", () => showShipCard(vessel));

        dot.addTo(vesselLayer);
        vesselMarkers.set(vessel.id, dot);
    });

    if (selectedVesselId !== null && !vesselMarkers.has(selectedVesselId)) {
        hideShipCard();
    }
}

function showShipCard(vessel) {
    setVesselHighlight(selectedVesselId, false);
    selectedVesselId = vessel.id;
    setVesselHighlight(vessel.id, true);

    const rows = [
        ["Status",     vessel.dark ? "Dark vessel (no AIS)" : "AIS reported"],
        ["Latitude",   formatCoord(vessel.lat, "N", "S")],
        ["Longitude",  formatCoord(vessel.lon, "E", "W")],
        ["MMSI",       vessel.mmsi || "—"],
        ["Type",       vessel.type],
        ["Length",     vessel.length_m ? `${vessel.length_m} m` : ""],
        ["Heading",    vessel.heading_deg != null ? `${vessel.heading_deg}°` : ""],
        ["Speed",      vessel.speed_kn != null ? `${vessel.speed_kn} kn` : ""],
        ["Detected",   vessel.detected_at],
        ["Confidence", vessel.confidence != null
                        ? `${Math.round(vessel.confidence * 100)} %` : ""],
    ];

    shipRows.innerHTML = "";

    if (vessel.name) {
        const name = document.createElement("p");
        name.className   = "ship-card__name";
        name.textContent = vessel.name;
        shipRows.appendChild(name);
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
            detail.classList.add(vessel.dark ? "is-dark" : "is-safe");
        }

        shipRows.appendChild(term);
        shipRows.appendChild(detail);
    });

    shipCard.classList.remove("is-hidden");
}

function hideShipCard() {
    shipCard.classList.add("is-hidden");
    setVesselHighlight(selectedVesselId, false);
    selectedVesselId = null;
}

function setVesselHighlight(vesselId, on) {
    if (vesselId === null || !currentScene) {
        return;
    }

    const marker = vesselMarkers.get(vesselId);
    const vessel = currentScene.vessels.find(item => item.id === vesselId);
    if (!marker || !vessel) {
        return;
    }

    const style = vesselStyle(vessel, on);
    marker.setStyle(style);
    marker.setRadius(style.radius);
}

function formatCoord(value, positive, negative) {
    if (value == null) {
        return "";
    }
    const letter = value >= 0 ? positive : negative;
    return `${Math.abs(value).toFixed(6)}° ${letter}`;
}

function setSar(on, options = {}) {
    sarLayer.clearLayers();
    btnSar.classList.toggle("is-active", on);
    btnSar.setAttribute("aria-pressed", on ? "true" : "false");

    if (!on) {
        return;
    }

    if (!currentScene) {
        btnSar.classList.remove("is-active");
        btnSar.setAttribute("aria-pressed", "false");
        if (!options.silent) {
            notify(BAKLAVA_SETTINGS.msg_sar_needs_scene, "info");
        }
        return;
    }

    const overlay = currentScene.sar_overlay;

    if (overlay && overlay.url) {
        L.imageOverlay(overlay.url,
                       L.latLngBounds(overlay.corners || currentScene.corners),
                       { opacity: 0.85 })
         .addTo(sarLayer);
        return;
    }

    L.tileLayer("https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png", {
        bounds: L.latLngBounds(currentScene.corners),
        opacity: 0.9,
        maxZoom: 18,
        attribution: "&copy; CARTO",
    }).addTo(sarLayer);

    if (!options.silent) {
        notify(BAKLAVA_SETTINGS.msg_no_sar, "info");
    }
}

btnSelect.addEventListener("click", () => {
    if (isPicking) {
        stopPicking();
    } else {
        startPicking();
    }
});

btnSar.addEventListener("click", () => {
    setSar(!btnSar.classList.contains("is-active"));
});

switchDark.addEventListener("click", () => {
    if (!currentScene) {
        return;
    }
    setDarkToggle(switchDark.getAttribute("aria-checked") !== "true");
});

btnCloseScene.addEventListener("click", () => closeScene());

btnCloseShip.addEventListener("click", hideShipCard);

btnHamburger.addEventListener("click", openSidebar);
btnSidebarClose.addEventListener("click", closeSidebar);
sidebarBackdrop.addEventListener("click", closeSidebar);

btnSarScenes.addEventListener("click", closeSidebar);
btnDarkAlerts.addEventListener("click", () => {
    closeSidebar();
    showPopup("Under construction!");
});

popupClose.addEventListener("click", hidePopup);
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
    startPicking,
    stopPicking,
    selectScene,
    closeScene,
    setDarkToggle,
    setControlsEnabled,
    setSar,
    showShipCard,
    hideShipCard,
    focusOn,
    notify,
    hideNotice,
    showPopup,
    showLoader,
    hideLoader,
    withLoader,
    get map()     { return map; },
    get scene()   { return currentScene; },
    get scenes()  { return allScenes; },
};
