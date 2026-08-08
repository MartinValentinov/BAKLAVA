const API_BASE = BAKLAVA_SETTINGS.api_base_url;

const btnRequest       = document.getElementById("btnRequest");
const btnDownload      = document.getElementById("btnDownload");

const notice           = document.getElementById("notice");
const noticeText       = document.getElementById("noticeText");

const mapPanel         = document.getElementById("mapPanel");

const catalogueView    = document.getElementById("catalogueView");
const catalogueHint    = document.getElementById("catalogueHint");
const imageList        = document.getElementById("imageList");

const detailsView      = document.getElementById("detailsView");
const reportText       = document.getElementById("reportText");
const btnDetailsBack   = document.getElementById("btnDetailsBack");

const btnHamburger     = document.getElementById("btnHamburger");
const btnSidebarClose  = document.getElementById("btnSidebarClose");
const sidebar          = document.getElementById("sidebar");
const sidebarBackdrop  = document.getElementById("sidebarBackdrop");
const btnSarScenes     = document.getElementById("btnSarScenes");
const btnDarkAlerts    = document.getElementById("btnDarkAlerts");

const popupOverlay     = document.getElementById("popupOverlay");
const popupMessage     = document.getElementById("popupMessage");
const popupClose       = document.getElementById("popupClose");

const processingOverlay  = document.getElementById("processingOverlay");
const processingMessage  = document.getElementById("processingMessage");

let map = null;
let drawnItems = null;

let sceneIsOpen = false;
let catalogueMode = null;
let noticeTimer = null;

function notify(message, kind = "success", autoHide = true) {
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
        noticeTimer = setTimeout(hideNotice, 4500);
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

function showProcessingOverlay(message) {
    processingMessage.textContent = message;
    processingOverlay.classList.remove("is-hidden");
}

function hideProcessingOverlay() {
    processingOverlay.classList.add("is-hidden");
}

function openSidebar() {
    sidebar.classList.add("is-open");
    sidebarBackdrop.classList.remove("is-hidden");
}

function closeSidebar() {
    sidebar.classList.remove("is-open");
    sidebarBackdrop.classList.add("is-hidden");
}

function setButtonsLocked(locked) {
    sceneIsOpen = locked;
    btnRequest.classList.toggle("is-locked", locked);
    btnDownload.classList.toggle("is-locked", locked);
}

function showCatalogue() {
    detailsView.classList.add("is-hidden");
    catalogueView.classList.remove("is-hidden");

    mapPanel.classList.remove("is-ready");
    clearMap();

    imageList.querySelectorAll(".scene-list__item").forEach(row => {
        row.classList.remove("is-selected");
    });

    setButtonsLocked(false);
}

function showDetails() {
    catalogueView.classList.add("is-hidden");
    detailsView.classList.remove("is-hidden");

    mapPanel.classList.add("is-ready");
    if (map) {
        map.invalidateSize();
    }

    setButtonsLocked(true);
}

function renderCatalogue(names, mode) {
    catalogueMode = mode;
    imageList.innerHTML = "";

    names.forEach(name => {
        const row = document.createElement("li");
        row.className   = "scene-list__item";
        row.textContent = name;
        row.dataset.name = name;

        row.addEventListener("click", () => onCatalogueRowClick(name, row));

        imageList.appendChild(row);
    });

    catalogueHint.classList.toggle("is-hidden", names.length > 0);
}

function onCatalogueRowClick(name, row) {
    if (sceneIsOpen) {
        return;
    }
    if (catalogueMode === "raw") {
        processImage(name, row);
    } else if (catalogueMode === "processed") {
        downloadAndRender(name, row);
    }
}

async function sendRequest() {
    notify(BAKLAVA_SETTINGS.msg_request_sending, "pending", false);

    try {
        const response = await fetch(`${API_BASE}/api/scenes/available`, { method: "POST" });
        const data     = await response.json();

        if (!response.ok) {
            notify(data.error || BAKLAVA_SETTINGS.msg_request_failed, "error");
            return false;
        }

        renderCatalogue(data, "raw");
        notify(BAKLAVA_SETTINGS.msg_request_ok, "success");
        return true;

    } catch (error) {
        notify(BAKLAVA_SETTINGS.msg_request_failed, "error");
        console.error(error);
        return false;
    }
}

async function processImage(name, row) {
    setButtonsLocked(true);
    showProcessingOverlay(`Processing ${name}...`);

    try {
        const response = await fetch(`${API_BASE}/api/scenes/${encodeURIComponent(name)}/process`, { method: "POST" });
        const data     = await response.json();

        hideProcessingOverlay();
        setButtonsLocked(false);

        if (!response.ok || !data.ok) {
            notify(data.error || BAKLAVA_SETTINGS.msg_process_failed, "error");
            return false;
        }

        notify(data.message || BAKLAVA_SETTINGS.msg_process_ok, "success");
        row.remove();
        catalogueHint.classList.toggle("is-hidden", imageList.children.length > 0);
        return true;

    } catch (error) {
        hideProcessingOverlay();
        setButtonsLocked(false);
        notify(BAKLAVA_SETTINGS.msg_process_failed, "error");
        console.error(error);
        return false;
    }
}

async function loadDownloadList() {
    try {
        const response = await fetch(`${API_BASE}/api/scenes`);
        const data     = await response.json();

        if (!response.ok) {
            notify(data.error || BAKLAVA_SETTINGS.msg_download_failed, "error");
            return false;
        }

        renderCatalogue(data, "processed");
        notify(BAKLAVA_SETTINGS.msg_download_ok, "success");
        return true;

    } catch (error) {
        notify(BAKLAVA_SETTINGS.msg_download_failed, "error");
        console.error(error);
        return false;
    }
}

async function downloadAndRender(name, row) {
    imageList.querySelectorAll(".scene-list__item").forEach(item => {
        item.classList.remove("is-selected");
    });
    if (row) {
        row.classList.add("is-selected");
    }

    let scene;
    try {
        const response = await fetch(`${API_BASE}/api/scenes/${encodeURIComponent(name)}`);
        if (!response.ok) {
            throw new Error(`status ${response.status}`);
        }
        scene = await response.json();
    } catch (error) {
        notify("Could not download the scene.", "error");
        console.error(error);
        return;
    }

    showDetails();
    renderShipScene(scene)
}

function renderShipScene(scene) {
    const ships = scene.ships || [];
    const meta  = scene.meta || {};

    clearMap();

    if (ships.length) {
        const avgLat = ships.reduce((sum, s) => sum + s.latitude, 0) / ships.length;
        const avgLon = ships.reduce((sum, s) => sum + s.longitude, 0) / ships.length;
        map.setView([avgLat, avgLon], 8);
    } else {
        map.setView([BAKLAVA_SETTINGS.map_default_lat, BAKLAVA_SETTINGS.map_default_lon], BAKLAVA_SETTINGS.map_default_zoom);
    }

    ships.forEach(ship => {
        addPin(ship.latitude, ship.longitude, `Vessel #${ship.id} — conf ${ship.conf}`, true);
    });

    const lines = [
        `Scene: ${meta.scene || ""}`,
        `Acquired: ${meta.acquired || ""}`,
        `Model: ${meta.model || ""}`,
        `Ships detected: ${meta.ships ?? ships.length}`,
        `On land: ${meta["on land"] ?? "0"}`,
        "",
    ];

    ships.forEach(ship => {
        lines.push(
            `#${ship.id}  conf ${ship.conf}  ${ship.latitude.toFixed(5)}, ${ship.longitude.toFixed(5)}  ` +
            `${ship.length_m}m x ${ship.width_m}m  heading ${ship.heading}°`
        );
    });

    reportText.value = lines.join("\n");
}

function initMap() {
    map = L.map("map").setView(
        [BAKLAVA_SETTINGS.map_default_lat, BAKLAVA_SETTINGS.map_default_lon],
        BAKLAVA_SETTINGS.map_default_zoom
    );

    L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
        maxZoom: 18,
        attribution: "&copy; OpenStreetMap contributors"
    }).addTo(map);

    drawnItems = L.layerGroup().addTo(map);
}

function addPin(lat, lon, label = "", isDark = false) {
    const pin = L.circleMarker([lat, lon], {
        radius: 8,
        color:  isDark ? "#D0342C" : "#1F271B",
        fillColor: isDark ? "#D0342C" : "#FFFFFF",
        fillOpacity: 1,
        weight: 3
    });

    if (label) {
        pin.bindPopup(label);
    }

    pin.addTo(drawnItems);
    return pin;
}

function drawCircle(lat, lon, radiusMetres, label = "") {
    const circle = L.circle([lat, lon], {
        radius: radiusMetres,
        color: "#918450",
        weight: 2,
        fillColor: "#918450",
        fillOpacity: 0.15
    });

    if (label) {
        circle.bindPopup(label);
    }

    circle.addTo(drawnItems);
    return circle;
}

function drawRectangle(south, west, north, east, label = "") {
    const bounds = [[south, west], [north, east]];

    const rectangle = L.rectangle(bounds, {
        color: "#1F271B",
        weight: 2,
        dashArray: "6 4",
        fillOpacity: 0.05
    });

    if (label) {
        rectangle.bindPopup(label);
    }

    rectangle.addTo(drawnItems);
    return rectangle;
}

function clearMap() {
    if (drawnItems) {
        drawnItems.clearLayers();
    }
}

function focusOn(lat, lon, zoom = 8) {
    map.setView([lat, lon], zoom);
}

btnRequest.addEventListener("click", () => {
    if (sceneIsOpen) {
        return;
    }
    sendRequest();
});

btnDownload.addEventListener("click", () => {
    if (sceneIsOpen) {
        return;
    }
    loadDownloadList();
});

btnDetailsBack.addEventListener("click", showCatalogue);

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
    if (event.key === "Escape") {
        hidePopup();
        closeSidebar();
        hideNotice();
    }
});

notice.addEventListener("click", hideNotice);

initMap();

// USAGE (browser console, F12):
//   BAKLAVA.sendRequest()
//   BAKLAVA.loadDownloadList()
//   BAKLAVA.notify("Anything you like", "error")
//   BAKLAVA.addPin(43.2, 28.6, "Test vessel", true)
//   BAKLAVA.drawCircle(43.2, 28.6, 30000, "30 km")
//   BAKLAVA.drawRectangle(42.9, 27.6, 43.6, 29.2, "Test box")
//   BAKLAVA.focusOn(43.2, 28.6, 8)
//   BAKLAVA.clearMap()
window.BAKLAVA = {
    addPin,
    drawCircle,
    drawRectangle,
    clearMap,
    focusOn,
    showPopup,
    notify,
    hideNotice,
    renderShipScene,
    sendRequest,
    loadDownloadList,
    showCatalogue,
    showDetails,
    get map() { return map; }
};
