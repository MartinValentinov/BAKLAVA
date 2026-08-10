/* ============================================================================
   BAKLAVA - browser logic
   ============================================================================
   This file decides WHAT HAPPENS when you click things, and draws the map.

   THE WHOLE APP IN SIX LINES
     1. Press the pencil  -> every SAR scene appears as a see-through blue box.
     2. Click a box       -> that scene is selected: dotted outline, the map
                             flies to it, the bar on top shows its numbers, and
                             the Dark Vessels switch turns itself ON.
     3. The switch        -> ON shows the dark vessels, OFF the "safe" ones.
     4. Click a vessel    -> the "Ship details" card opens in the top-right.
     5. Press the layers  -> the SAR picture of the scene goes over the map.
     6. Press the ×       -> the scene closes and the boxes come back.

   How to read this file:
     PART 1  - shortcuts to the boxes in index.html, and the app's state
     PART 2  - the notification, the popup and the loading animation
     PART 3  - the sidebar
     PART 4  - the map itself
     PART 5  - the scenes (the blue boxes)
     PART 6  - the vessels and the "Ship details" card
     PART 7  - the SAR overlay
     PART 8  - wiring the controls to the functions
     PART 9  - start-up and a console shortcut for testing by hand

   A note on words used here:
     "element"  = one box/button/text on the page
     "class"    = a label on an element that CSS reacts to (e.g. "is-active")
     "layer"    = one thing drawn on the map (a box, a dot, a picture)
     "async / await" = "do this, wait for the answer, then continue"
   ========================================================================= */


/* ============================================================================
   PART 1 - THE ELEMENTS AND THE STATE
   document.getElementById("x") finds the element whose id="x" in index.html.
   ========================================================================= */
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

/* --- the map and the four groups of things drawn on it --------------------
   Keeping them apart means each one can be wiped on its own: closing a scene
   empties the vessels without touching the boxes, and so on. */
let map           = null;   // the Leaflet map object
let sceneLayer    = null;   // the see-through blue boxes (picking mode)
let selectedLayer = null;   // the dotted outline of the chosen scene
let vesselLayer   = null;   // the dots
let sarLayer      = null;   // the radar picture, when it is switched on

/* --- what the app is currently doing -------------------------------------- */

// The scenes from GET /api/scenes. Fetched once, the first time they are needed.
let allScenes = null;

// True while the pencil is pressed and the blue boxes are on the map.
let isPicking = false;

// The scene currently open, exactly as GET /api/scenes/<id> returned it,
// or null when none is open.
let currentScene = null;

// The state of the Dark Vessels switch.
//   false -> every vessel of the scene is drawn, dark and safe alike
//   true  -> only the dark ones are left on the map
let showDark = false;

// The vessel whose card is open, so it can be drawn bigger than the rest.
let selectedVesselId = null;

// Every dot currently on the map, kept by vessel id. It is what lets one dot
// be redrawn on its own - highlighting the chosen vessel must not wipe and
// rebuild the whole layer, because the dot being clicked would then be taken
// off the map in the middle of its own click.
let vesselMarkers = new Map();

// Remembers the timer that hides the notification again, so that a second
// notification does not get wiped by the first one's countdown.
let noticeTimer = null;

// The moment showLoader() was called, so hideLoader() can tell whether the
// animation has been on screen long enough to have been seen. See withLoader().
let loaderShownAt = 0;


/**
 * cssColour(name)
 * Reads one of the colour variables out of static/css/style.css, so the shapes
 * drawn on the map use the same palette as the rest of the page.
 *
 * Leaflet takes colours as plain strings, which is why they cannot simply be
 * styled in CSS like everything else - this is the bridge between the two.
 *
 * @param {string} name - the variable, e.g. "--color-scene"
 * @returns {string} the colour, e.g. "#2F6FD0"
 */
function cssColour(name) {
    return getComputedStyle(document.documentElement)
        .getPropertyValue(name)
        .trim();
}


/* ============================================================================
   PART 2 - THE NOTIFICATION, THE POPUP AND THE LOADING ANIMATION
   ========================================================================= */

/**
 * notify(message, kind, autoHide)
 * Shows the small strip floating over the top-left of the map.
 *
 * @param {string} message  - the text to show, e.g. "Pick a scene on the map"
 * @param {string} kind     - "success" (green), "error" (red), "info" (amber)
 *                            or "pending" (grey, with a pulsing dot).
 *                            Anything else falls back to "info".
 * @param {boolean} autoHide- true -> disappears again after a few seconds.
 *
 * Example:  notify("Scene loaded", "success");
 */
function notify(message, kind = "info", autoHide = true) {
    // Cancel the previous countdown, otherwise an old timer could hide this
    // brand-new message a moment after it appeared.
    clearTimeout(noticeTimer);

    noticeText.textContent = message;

    // Exactly one colour class at a time.
    notice.classList.remove("notice--success", "notice--error",
                            "notice--info", "notice--pending");
    const known = ["success", "error", "info", "pending"];
    notice.classList.add("notice--" + (known.includes(kind) ? kind : "info"));

    notice.classList.remove("is-hidden");

    // Restart the little slide-in animation even when the strip was already
    // visible: remove the class, force the browser to notice, add it back.
    notice.classList.remove("is-entering");
    void notice.offsetWidth;                  // this line triggers the reflow
    notice.classList.add("is-entering");

    if (autoHide) {
        noticeTimer = setTimeout(hideNotice, 3800);
    }
}

/**
 * hideNotice()
 * Takes the notification off the map again.
 */
function hideNotice() {
    clearTimeout(noticeTimer);
    notice.classList.add("is-hidden");
}

/**
 * showPopup(message)
 * Shows the dim overlay with a message box in the middle. Used by the sidebar
 * sections that do not exist yet.
 */
function showPopup(message) {
    popupMessage.textContent = message;
    popupOverlay.classList.remove("is-hidden");   // remove "is-hidden" -> visible
}

/**
 * hidePopup()
 * Closes the popup again.
 */
function hidePopup() {
    popupOverlay.classList.add("is-hidden");      // add "is-hidden" -> invisible
}

/**
 * showLoader(message)
 * Puts the dimmed sheet with the animated logo over the whole page.
 *
 * @param {string} message - the line under the animation. Leave it out to keep
 *                           the default wording from app.py ("Working...").
 */
function showLoader(message) {
    if (typeof message === "string") {
        loaderText.textContent = message;
    }
    loaderShownAt = Date.now();
    loaderOverlay.classList.remove("is-hidden");
}

/**
 * hideLoader()
 * Takes the sheet away again and puts the caption back to its default.
 */
function hideLoader() {
    loaderOverlay.classList.add("is-hidden");
    loaderText.textContent = BAKLAVA_SETTINGS.loading_text || "";
}

/**
 * wait(milliseconds)
 * A pause you can "await". 1000 milliseconds = 1 second.
 */
function wait(milliseconds) {
    return new Promise(done => setTimeout(done, milliseconds));
}

/**
 * withLoader(job, message)
 * Runs "job" with the loading animation on screen and takes the animation away
 * afterwards - whether the job succeeded, failed or threw.
 *
 * @param {Function} job   - the work to do. Usually an async function.
 * @param {string} message - the line under the animation. Optional.
 * @returns whatever "job" returned.
 *
 * >>> WHY THE MINIMUM TIME <<<
 * A local server answers in a few milliseconds. Without a floor the animation
 * would appear and vanish within one frame, which looks like a glitch rather
 * than like progress. So it always stays for at least loading_min_ms (set in
 * app.py). Set it to 0 there to switch this off.
 */
async function withLoader(job, message) {
    showLoader(message);
    try {
        return await job();
    } finally {
        // "finally" runs even when the job failed, so the animation can never
        // be left on screen with the page stuck behind it.
        const seenFor = Date.now() - loaderShownAt;
        const minimum = BAKLAVA_SETTINGS.loading_min_ms || 0;
        if (seenFor < minimum) {
            await wait(minimum - seenFor);
        }
        hideLoader();
    }
}


/* ============================================================================
   PART 3 - THE SIDEBAR
   ========================================================================= */

/** openSidebar() - slides the right-hand menu in and shows the veil behind it. */
function openSidebar() {
    sidebar.classList.add("is-open");
    sidebarBackdrop.classList.remove("is-hidden");
}

/** closeSidebar() - slides the menu back out to the right. */
function closeSidebar() {
    sidebar.classList.remove("is-open");
    sidebarBackdrop.classList.add("is-hidden");
}


/* ============================================================================
   PART 4 - THE MAP

   initMap() runs once when the page opens. Unlike the old version the map is
   visible from the very first second - it IS the interface.

   To use a different map style, replace the tile URL below. Free options:
     Standard streets : https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png
     Plain grey       : https://{s}.basemaps.cartocdn.com/light_all/{z}/{x}/{y}.png
     Dark             : https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png
     Satellite (Esri) : https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}
   ========================================================================= */
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
        attribution: "&copy; OpenStreetMap contributors"   // legally required
    }).addTo(map);

    // The four containers described in PART 1. The order they are created in
    // is the order they stack: scenes at the bottom, vessels on top, so a dot
    // is never hidden underneath a box.
    //
    // sceneLayer is a FEATURE group rather than a plain layer group: only a
    // feature group can be asked getBounds(), which is how startPicking() zooms
    // out far enough to show every scene at once.
    sarLayer      = L.layerGroup().addTo(map);
    sceneLayer    = L.featureGroup().addTo(map);
    selectedLayer = L.layerGroup().addTo(map);
    vesselLayer   = L.layerGroup().addTo(map);

    // Clicking the empty sea closes the ship card - the same reflex as clicking
    // outside any panel.
    map.on("click", hideShipCard);
}

/**
 * focusOn(lat, lon, zoom)
 * Moves the map to a given point without drawing anything.
 *
 * @param {number} zoom - 1 = whole world, 6 = country, 12 = city, 18 = street
 */
function focusOn(lat, lon, zoom = 8) {
    map.setView([lat, lon], zoom);
}


/* ============================================================================
   PART 5 - THE SCENES (THE BLUE BOXES)
   ========================================================================= */

/**
 * fetchScenes()
 * Asks the server for the list of scenes (GET /api/scenes) the first time it
 * is needed and remembers the answer, so pressing the pencil again is instant.
 *
 * @returns {Array} the scenes, or [] if the server could not be reached.
 */
async function fetchScenes() {
    if (allScenes) {
        return allScenes;                      // already have them
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

/**
 * drawScenes(scenes)
 * Draws one big see-through blue box per scene and makes each one clickable.
 *
 * The shape is a POLYGON, not a rectangle, because a SAR scene is flown at an
 * angle: its four corners come straight from the backend and the box is drawn
 * exactly as they describe it, tilt and all.
 */
function drawScenes(scenes) {
    sceneLayer.clearLayers();

    scenes.forEach(scene => {
        const box = L.polygon(scene.corners, {
            color: cssColour("--color-scene"),   // the outline
            weight: 2,
            fillColor: cssColour("--color-scene"),
            fillOpacity: 0.18,                   // 0 = invisible, 1 = solid
        });

        // The name of the scene follows the mouse, so you know what you are
        // about to click before you click it.
        box.bindTooltip(scene.label, { sticky: true });

        box.on("click", () => selectScene(scene.id));

        box.addTo(sceneLayer);
    });
}

/**
 * startPicking()
 * What the pencil button does. Puts every scene on the map as a blue box and
 * zooms out far enough to show all of them at once.
 *
 * If a scene is currently open it is closed first: you cannot pick a new scene
 * while the old one is still drawn over the map.
 */
async function startPicking() {
    if (currentScene) {
        closeScene({ silent: true });
    }

    const scenes = await withLoader(fetchScenes);
    if (!scenes.length) {
        return;                                 // fetchScenes already complained
    }

    isPicking = true;
    btnSelect.classList.add("is-active");
    btnSelect.setAttribute("aria-pressed", "true");
    board.classList.add("is-picking");

    drawScenes(scenes);

    // Fit all the boxes into the view, with a little air around them.
    // invalidateSize() first: Leaflet remembers how big its box was and works
    // the zoom out from that remembered number. If the window changed size
    // while the page was not being drawn, that number is stale and every
    // scene would be squeezed into a corner. This line re-measures.
    map.invalidateSize();
    map.fitBounds(sceneLayer.getBounds(), { padding: [40, 40] });

    notify(BAKLAVA_SETTINGS.msg_pick_scene, "info");
}

/**
 * stopPicking(options)
 * Takes the blue boxes off the map again.
 *
 * @param {object} options - {silent: true} skips the notification. Used when
 *                           picking ends because a scene was chosen, where a
 *                           "cancelled" message would be plainly wrong.
 */
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

/**
 * selectScene(sceneId)
 * Runs when one of the blue boxes is clicked. This is the centre of the app:
 *
 *   1. ask the backend for the scene and its vessels
 *   2. take the other boxes away and outline this one with a dotted line
 *   3. fly to it and show the bar with its numbers on top
 *   4. turn the Dark Vessels switch ON by itself and draw the dark vessels
 *
 * @param {string} sceneId - e.g. "TYR_20240311"
 */
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
        return;                                 // stop here on failure
    }

    currentScene = scene;

    // Step 2 - the other boxes go, this one stays as a dotted outline.
    stopPicking({ silent: true });
    hideShipCard();
    setSar(false, { silent: true });            // a new scene starts without it

    selectedLayer.clearLayers();
    const outline = L.polygon(scene.corners, {
        color: cssColour("--color-scene-picked"),
        weight: 2,
        // The dotted border: 2 pixels of line, 6 of gap, with round ends so the
        // dots are dots and not tiny dashes.
        dashArray: "2 6",
        lineCap: "round",
        fill: false,                            // see-through: it is only a border
        interactive: false,                     // clicks go to the vessels under it
    });
    outline.addTo(selectedLayer);

    // Step 3 - move to the scene and fill the bar on top.
    //
    // fitBounds and not flyToBounds: the flight is a long frame-by-frame
    // animation across the whole map, and it only ever arrives while the page
    // is actually being drawn. fitBounds is one short zoom instead. Add
    // {animate: false} to the options below to make it instant.
    // invalidateSize() re-measures the map box first - see startPicking().
    map.invalidateSize();
    map.fitBounds(outline.getBounds(), { padding: [30, 30] });

    statTotal.textContent = scene.totals.total;
    statDark.textContent  = scene.totals.dark;
    statScene.textContent = scene.label;
    statsBar.classList.remove("is-hidden");

    // Step 4 - the SAR button and the switch have something to work on now.
    setControlsEnabled(true);

    // The switch turns itself on, so the scene opens on its dark vessels.
    // Turning it off from here shows the whole scene, dark and safe together.
    setDarkToggle(true);
}

/**
 * closeScene(options)
 * The × in the bar on top. Clears the scene completely and goes back to
 * picking a new one.
 *
 * @param {object} options - {silent: true} closes without reopening the boxes.
 *                           Used by startPicking(), which opens them itself.
 */
function closeScene(options = {}) {
    // The card first, while the dots it points at are still there, then the
    // scene itself: the other way round it would try to let go of a dot that
    // has already gone.
    hideShipCard();

    currentScene = null;

    selectedLayer.clearLayers();
    vesselLayer.clearLayers();
    vesselMarkers.clear();
    setSar(false, { silent: true });

    statsBar.classList.add("is-hidden");
    setDarkToggle(false);
    setControlsEnabled(false);          // nothing left for the two of them to do

    if (!options.silent) {
        startPicking();                         // straight back to choosing one
    }
}


/* ============================================================================
   PART 6 - THE VESSELS AND THE "SHIP DETAILS" CARD
   ========================================================================= */

/**
 * setControlsEnabled(enabled)
 * Switches the SAR button and the Dark Vessels switch on and off as controls.
 *
 * Both of them work ON a scene, so before one is selected there is nothing for
 * them to do and they are dead: greyed out, unclickable, skipped when tabbing.
 * The pencil is not touched - picking a scene is exactly what you can always do.
 *
 * @param {boolean} enabled - true once a scene is open, false again when it closes
 */
function setControlsEnabled(enabled) {
    btnSar.disabled     = !enabled;
    switchDark.disabled = !enabled;

    // The label next to the switch fades with it, so the pair reads as one
    // unavailable thing rather than a live label beside a dead switch.
    switchWrap.classList.toggle("is-disabled", !enabled);
}

/**
 * setDarkToggle(on)
 * Flips the Dark Vessels switch and redraws the map to match.
 *
 * @param {boolean} on - false -> EVERY vessel of the scene is drawn
 *                       true  -> only the dark ones (no AIS) are left
 *
 * The switch's LOOK is driven straight off aria-checked in ui.css, so setting
 * that attribute is all it takes to move the knob - there is no second class
 * that could drift out of step with what a screen reader is told.
 */
function setDarkToggle(on) {
    showDark = on;
    switchDark.setAttribute("aria-checked", on ? "true" : "false");
    renderVessels();
}

/**
 * vesselStyle(vessel, isSelected)
 * The look of one dot. It is one function rather than a block of options in
 * the middle of renderVessels() because the same look has to be applied twice:
 * once when the dot is made, and again when it is picked out or let go of.
 *
 * @param {object} vessel      - one entry of scene.vessels
 * @param {boolean} isSelected - true if its card is the one open
 */
function vesselStyle(vessel, isSelected) {
    return {
        radius: isSelected ? 9 : 6,
        color: vessel.dark ? cssColour("--color-vessel-dark")
                           : cssColour("--color-vessel-safe"),
        fillColor: vessel.dark ? cssColour("--color-vessel-dark") : "#FFFFFF",
        fillOpacity: 1,
        weight: isSelected ? 4 : 2,              // the open one wears a thicker ring
    };
}

/**
 * renderVessels()
 * Draws the vessels of the open scene: all of them, or only the dark ones if
 * the switch is on. Called again after every flip of it.
 *
 * With no scene open there is simply nothing to draw and the map stays empty.
 */
function renderVessels() {
    vesselLayer.clearLayers();
    vesselMarkers.clear();

    if (!currentScene) {
        return;
    }

    // The switch decides: only the dark ones, or the whole scene.
    const vessels = showDark
        ? currentScene.vessels.filter(vessel => vessel.dark)
        : currentScene.vessels;

    vessels.forEach(vessel => {
        // A circleMarker is a coloured dot. It is used instead of the default
        // teardrop icon because its colour can be changed with one line.
        const dot = L.circleMarker(
            [vessel.lat, vessel.lon],
            Object.assign(vesselStyle(vessel, vessel.id === selectedVesselId), {
                // Without this the click carries on to the map underneath,
                // whose own handler closes the card we are just opening. This
                // is Leaflet's own way of saying "the click stops here".
                bubblingMouseEvents: false,
            })
        );

        dot.on("click", () => showShipCard(vessel));

        dot.addTo(vesselLayer);
        vesselMarkers.set(vessel.id, dot);
    });

    // The open card may belong to a vessel that has just been filtered away
    // (its card was open, then the switch hid the safe ships). Closing it is
    // better than leaving details on screen for a dot nobody can see.
    if (selectedVesselId !== null && !vesselMarkers.has(selectedVesselId)) {
        hideShipCard();
    }
}

/**
 * showShipCard(vessel)
 * Fills the card in the top-right corner with one vessel and shows it.
 *
 * @param {object} vessel - one entry of scene.vessels, see app.py
 *
 * Every row is optional: a field the backend did not send is simply left out
 * rather than printed as "undefined".
 */
function showShipCard(vessel) {
    // Let go of the dot that was picked out before, then pick out this one.
    // Only the two dots are touched: rebuilding the whole layer here would
    // take the dot being clicked off the map in the middle of its own click.
    setVesselHighlight(selectedVesselId, false);
    selectedVesselId = vessel.id;
    setVesselHighlight(vessel.id, true);

    // The rows, in the order they appear on the card. Each one is
    // [label, value]; a value of "" or undefined drops the whole row.
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

    // Build the list from scratch every time. innerHTML = "" empties it first.
    shipRows.innerHTML = "";

    if (vessel.name) {
        const name = document.createElement("p");
        name.className   = "ship-card__name";
        name.textContent = vessel.name;
        shipRows.appendChild(name);
    }

    rows.forEach(([label, value]) => {
        if (value === "" || value == null) {
            return;                              // nothing to say - skip the row
        }

        const term = document.createElement("dt");
        term.textContent = label;

        const detail = document.createElement("dd");
        detail.textContent = value;

        // The status line is coloured by its meaning: red for a dark vessel,
        // green for one that reports itself.
        if (label === "Status") {
            detail.classList.add(vessel.dark ? "is-dark" : "is-safe");
        }

        shipRows.appendChild(term);
        shipRows.appendChild(detail);
    });

    shipCard.classList.remove("is-hidden");
}

/**
 * hideShipCard()
 * Closes the card and takes the thicker ring off the dot again.
 */
function hideShipCard() {
    shipCard.classList.add("is-hidden");
    setVesselHighlight(selectedVesselId, false);
    selectedVesselId = null;
}

/**
 * setVesselHighlight(vesselId, on)
 * Draws one dot bigger (or back to normal) without touching any of the others.
 *
 * @param {string} vesselId - which dot, or null for "none"
 * @param {boolean} on      - true -> the thicker ring, false -> back to normal
 *
 * Nothing happens if that vessel is not currently on the map, which is exactly
 * what should happen: there is no dot left to let go of.
 */
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
    marker.setRadius(style.radius);      // the radius is not part of setStyle()
}

/**
 * formatCoord(value, positive, negative)
 * Turns -15.4168 into "15.416800° W".
 *
 * @param {number} value    - the coordinate in decimal degrees
 * @param {string} positive - the letter for a positive value ("N" or "E")
 * @param {string} negative - the letter for a negative one   ("S" or "W")
 */
function formatCoord(value, positive, negative) {
    if (value == null) {
        return "";
    }
    const letter = value >= 0 ? positive : negative;
    return `${Math.abs(value).toFixed(6)}° ${letter}`;
}


/* ============================================================================
   PART 7 - THE SAR OVERLAY

   The layers button lays the radar picture of the open scene over the map.

   >>> WHAT HAPPENS WHEN THERE IS NO PICTURE <<<
   The demo backend ships no radar images (sar_overlay is None in app.py), so
   the button falls back to a grey radar-style basemap limited to the scene's
   own area - the button stays demonstrable and it is obvious that this is a
   stand-in, not radar data. As soon as the backend sends a real picture the
   first branch below takes over and nothing here has to change.
   ========================================================================= */

/**
 * setSar(on, options)
 * Switches the overlay on or off.
 *
 * @param {boolean} on     - true -> put it on the map, false -> take it off
 * @param {object} options - {silent: true} skips the notifications. Used when
 *                           the overlay is cleared as part of something else.
 */
function setSar(on, options = {}) {
    sarLayer.clearLayers();
    btnSar.classList.toggle("is-active", on);
    btnSar.setAttribute("aria-pressed", on ? "true" : "false");

    if (!on) {
        return;
    }

    // Nothing to lay a picture over.
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
        // The real thing: a picture pinned to its four corners on the map.
        // L.imageOverlay wants two opposite corners, so the picture is laid out
        // by its bounding box.
        L.imageOverlay(overlay.url,
                       L.latLngBounds(overlay.corners || currentScene.corners),
                       { opacity: 0.85 })
         .addTo(sarLayer);
        return;
    }

    // The stand-in: grey tiles, clipped to the scene's own area by the
    // "bounds" option, so only the scene turns radar-grey and the rest of the
    // map stays as it was.
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


/* ============================================================================
   PART 8 - CONNECTING THE CONTROLS
   "addEventListener('click', ...)" means: when this element is clicked, run
   this function. This is where the whole interface is wired together.
   ========================================================================= */

// THE PENCIL - shows the blue boxes, or takes them away again.
btnSelect.addEventListener("click", () => {
    if (isPicking) {
        stopPicking();
    } else {
        startPicking();
    }
});

// THE LAYERS BUTTON - the SAR overlay on the open scene.
// It is disabled until a scene is open, so it cannot even be reached before
// there is something to lay a picture over.
btnSar.addEventListener("click", () => {
    setSar(!btnSar.classList.contains("is-active"));
});

// THE SWITCH - every vessel when off, only the dark ones when on.
// Disabled until a scene is open, for the same reason.
switchDark.addEventListener("click", () => {
    // The browser already refuses the click while the switch is disabled. The
    // check is here as well so the knob can never move to a state the map
    // cannot show, whoever ends up calling this.
    if (!currentScene) {
        return;
    }
    setDarkToggle(switchDark.getAttribute("aria-checked") !== "true");
});

// The × in the bar on top - closes the scene, back to picking.
btnCloseScene.addEventListener("click", () => closeScene());

// The × on the ship card.
btnCloseShip.addEventListener("click", hideShipCard);

// The hamburger and everything that closes the sidebar again.
btnHamburger.addEventListener("click", openSidebar);
btnSidebarClose.addEventListener("click", closeSidebar);
sidebarBackdrop.addEventListener("click", closeSidebar);

// The sidebar sections.
// "AIS Database Archive" needs nothing here: it is a plain link in the HTML
// and the browser follows it by itself (see index.html, BLOCK 3).
btnSarScenes.addEventListener("click", closeSidebar);          // we are already here
btnDarkAlerts.addEventListener("click", () => {
    closeSidebar();
    showPopup("Under construction!");
});

// Popup "OK" button, and clicking the dim area around the popup.
popupClose.addEventListener("click", hidePopup);
popupOverlay.addEventListener("click", (event) => {
    if (event.target === popupOverlay) {
        hidePopup();
    }
});

// Escape closes whatever is open, innermost thing first.
document.addEventListener("keydown", (event) => {
    if (event.key !== "Escape") {
        return;
    }
    hidePopup();
    closeSidebar();
    hideShipCard();
    hideNotice();
});

// Clicking the notification dismisses it early.
notice.addEventListener("click", hideNotice);


/* ============================================================================
   PART 9 - START-UP AND MANUAL TESTING
   ========================================================================= */

// The map is built as soon as the page opens - it is the interface, so there
// is nothing to wait for. No scene is selected, so the bar on top is hidden
// and the map is simply the region.
initMap();

/*
   Everything below makes the inner workings reachable from the browser
   console, so you can try things without clicking through the interface.

   Open the page, press F12, choose "Console", and type for example:

       BAKLAVA.startPicking()                      // as if the pencil was clicked
       BAKLAVA.selectScene("TYR_20240311")         // open a scene straight away
       BAKLAVA.setDarkToggle(false)                // show every vessel of it
       BAKLAVA.setSar(true)                        // the SAR overlay
       BAKLAVA.closeScene()                        // back to picking
       BAKLAVA.notify("Anything you like", "error")
       BAKLAVA.focusOn(43.2, 28.6, 8)
*/
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
