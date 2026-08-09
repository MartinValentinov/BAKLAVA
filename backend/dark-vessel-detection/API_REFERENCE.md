# Dark Vessel API — contract for the UI

This is everything a separately-built frontend needs to know to talk to this
backend. The frontend can be anything (React, plain JS, another team's
framework) — it never needs to see the C# code, only this contract.

**Base URL:** wherever this app is running, e.g. `http://localhost:5000` in dev.

**Auth:** every `/api/*` request needs a header:
```
X-Api-Key: <the value someone configured with `dotnet user-secrets set "Api:ApiKey" "..."`>
```
Missing or wrong key → `401 Unauthorized`, body `{ "ok": false, "error": "missing or invalid X-Api-Key" }`.

**CORS:** enabled. In dev, any origin is allowed by default (`Cors:AllowedOrigins`
is empty in `appsettings.json`). Before this backend is reachable by anyone
but a trusted developer, `Cors:AllowedOrigins` should be set to the UI's real
URL(s) — ask whoever's running the backend to do that once the UI has a fixed address.

---

## `GET /api/status`

Collector's current state — poll this to build a live dashboard tile (same
idea as the old PHP console's status strip).

**Response `200`:**
```json
{
  "ok": true,
  "running": true,
  "desired": "running",
  "coverageId": 42,
  "messages": 1204,
  "rowsWritten": 890,
  "startedAt": "2026-08-07T09:00:00Z",
  "lastMessageAt": "2026-08-07T09:14:57Z",
  "lastError": null
}
```
- `running` — is the collector actually connected right now.
- `desired` — `"running"` or `"stopped"` — what the operator last asked for (may briefly differ from `running` while (re)connecting).
- `lastError` — `null` normally; a message string if the last session failed.

---

## `POST /api/control/start`

Tells the collector to start (or resume) archiving AIS. No request body.

**Response `200`:** `{ "ok": true, "desired": "running" }`

---

## `POST /api/control/stop`

Tells the collector to stop. No request body.

**Response `200`:** `{ "ok": true, "desired": "stopped" }`

---

## `GET /api/archive-stats`

Size/span of the archive on disk — for a "storage used" widget.

**Response `200`:**
```json
{
  "rowsEstimate": 790000,
  "bytes": 157286400,
  "oldest": "2026-08-01T00:00:00Z",
  "newest": "2026-08-07T09:14:00Z"
}
```
Any field can be `null` if the table is empty or the stats query hasn't run yet.

---

## `POST /api/match`

The actual dark-vessel check. Send one onboard detection, get back whether
it's explained by AIS traffic.

**Request body:**
```json
{
  "detectionId": "sar-scene-042-crop-7",
  "lat": 42.501,
  "lon": 27.201,
  "timestampUtc": "2026-08-07T09:12:00Z",
  "headingDeg": 134.5
}
```
- `detectionId` — any string you want to identify this detection by (echoed back).
- `lat`, `lon` — required.
- `timestampUtc` — required, ISO 8601. **Must actually be UTC** — send it with a `Z` suffix or a `+00:00` offset; a timestamp with no offset will be rejected.
- `headingDeg` — optional, currently carried through but not used in the match decision (the algorithm is a simple distance+time gate, not a heading-weighted score).

**Response `200`:**
```json
{
  "detectionId": "sar-scene-042-crop-7",
  "status": "Matched",
  "hadCoverage": true,
  "best": {
    "mmsi": 123456789,
    "distanceKm": 0.34,
    "timeGapSeconds": 0,
    "interpolatedLat": 42.5008,
    "interpolatedLon": 27.2006
  },
  "candidates": [
    { "mmsi": 123456789, "distanceKm": 0.34, "timeGapSeconds": 0, "interpolatedLat": 42.5008, "interpolatedLon": 27.2006 }
  ]
}
```

- **`status`** — one of three strings, this is the field the UI cares about most:
  - `"Matched"` — a real AIS-reporting vessel explains this detection. Not dark, nothing to alert on.
  - `"Dark"` — no AIS vessel nearby, **and** the archive was actually listening at that time. This is the alert-worthy case.
  - `"UnknownNoCoverage"` — no AIS vessel nearby, but the archive **wasn't listening** at that time (collector was off/crashed). This is *not* a dark-vessel verdict — the UI should show it as "inconclusive," not as an alert. Never treat this the same as `"Dark"`.
- **`hadCoverage`** — `true`/`false`, whether the archive was listening. Redundant with the status logic above but useful to show directly (e.g. "based on 47 minutes of AIS coverage around this time").
- **`best`** — the closest candidate considered, or `null` if none were found at all. This is what explains a `"Matched"` verdict.
- **`candidates`** — every AIS vessel considered, nearest-first, even for a `"Dark"` result. Useful for showing "closest traffic was 4.2 km / 11 min away" on a dark-vessel alert, so a reviewer has context instead of a bare "dark" label.

---

## Example: full round-trip with `curl`

```bash
curl -X POST http://localhost:5000/api/match \
  -H "X-Api-Key: your-api-key" \
  -H "Content-Type: application/json" \
  -d '{"detectionId":"test-1","lat":42.5,"lon":27.2,"timestampUtc":"2026-08-07T09:00:00Z"}'
```