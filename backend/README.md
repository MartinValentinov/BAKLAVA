# Baklava Backend

ASP.NET Core Web API that sits between your frontend and the Jetson.
It doesn't talk to the Jetson directly — it shells out to
`jetson_client.sh` for every request, exactly like you'd run it by hand.
`jetson_client.sh` itself talks HTTP to `listener_service.py`, a daemon
running in Docker on the Jetson (see `../jetson/`), not SSH — this backend's
code is unaffected either way, since `jetson_client.sh`'s command-line
contract didn't change.

## Setup

1. `dotnet restore`
2. Edit `appsettings.json`:
   - `JetsonClient:ScriptPath` — absolute path to `jetson_client.sh` on
     the machine this backend runs on (must be executable, `chmod +x`).
   - `JetsonClient:DestDir` — must match the `DEST_DIR` hardcoded inside
     `jetson_client.sh` itself (currently `/Users/martinvalentinov/Desktop/scenes`).
     `/api/scenes/sync` reads files back out of this folder after calling
     `get-all`, so if these two ever disagree, sync will return stale or
     empty results even though the underlying pull succeeded.
   - `JetsonClient:TimeoutSeconds` — timeout for the short calls (list, get,
     get-all, list-images).
   - `JetsonClient:ProcessTimeoutSeconds` — timeout for `POST
     /api/scenes/{name}/process`, which blocks for the full length of the
     Jetson's detection run. Defaults to 1800s (30 min); raise it for large
     scenes.
   - `Cors:AllowedOrigins` — the frontend's origin(s), e.g.
     `http://127.0.0.1:5000` for the Flask dev server.
3. `dotnet run` (pinned to `http://localhost:5080` by
   `Properties/launchSettings.json`, to avoid clashing with Flask's own
   port 5000).
4. Swagger UI at `/swagger` in development mode, for manually poking endpoints.

## Endpoints

| Method | Path                      | Maps to                          |
|--------|---------------------------|-----------------------------------|
| GET    | `/api/scenes`             | `list`, then `get` per scene — the map's scene boxes |
| GET    | `/api/scenes/{name}`      | one scene with its vessels, in map-frontend shape |
| GET    | `/api/scenes/{name}/raw`  | the detector's own JSON, normalised onto `meta`+`ships` |
| GET    | `/api/scenes/{name}/overview` | `jetson_client.sh overview NAME` — the SAR overlay image |
| POST   | `/api/scenes/sync`        | `jetson_client.sh get-all`, then reads the pulled JSONs back and returns them |
| POST   | `/api/scenes/available`   | `jetson_client.sh list-images` — raw, not-yet-processed products |
| POST   | `/api/scenes/{name}/process` | `jetson_client.sh process NAME` — runs detection on the Jetson, blocks until done |
| POST   | `/api/coords?name=`       | `jetson_client.sh send-coords`   |

### What the map frontend gets

`GET /api/scenes` and `GET /api/scenes/{name}` answer in the shape
`new_frontend/` expects, written down in its README under "Backend contract".
The translation lives in `Common/SceneProjector.cs`; `Common/FrontendModels.cs`
is that contract expressed as C# records, so renaming a field here without
mirroring it there is a compile error rather than a silently empty map.

Three things are worth knowing about it:

**Scene corners.** The frontend draws each scene as a box *before* anything is
known about its vessels, so it needs the scene's own footprint. The detector
now writes one (`footprint_lonlat` in its output JSON). Scenes produced before
that fall back to the bounding box of their detections, which is smaller than
the truth but better than no box.

**`dark`.** A vessel is dark when `DarkVessel.Api` reports anything other than
`Matched` — so both a genuine AIS absence and "nobody was listening there" count
as dark, for the reasons that service's own docs give. When AIS matching is
switched off, or the scene has no usable acquisition time, or the matcher is
down, **every** detection is reported dark. That is the honest reading — with
nothing to compare against, nothing has been ruled out — and it is the safe
direction to fail in. A matching outage must not quietly empty the map.

**Caching.** Building the scene list means reading every scene, and each read is
a `jetson_client.sh` process over the link. `SceneCatalogService` caches them.
A processed scene is immutable (its name carries the acquisition timestamp), so
entries are only dropped when `POST /api/scenes/{name}/process` re-runs one.

### Imagery: the overview

This comes from the detector's newer output and is pulled lazily, per scene,
the first time it is asked for: a decimated whole-scene render with the
detections drawn on it, a few hundred kB, served as the frontend's
`sar_overlay`.

`POST /api/coords` expects a raw JSON body (the coordinates payload) and
an optional `?name=` query param.

`POST /api/scenes/{name}/process` responds `{ ok: true, message }` on
success or `502 { ok: false, error }` on failure — the Jetson-side service
marks the image processed on success, so it drops out of future
`list-images` results.

## Dark-vessel matching

`GET /api/scenes/{name}` matches every detected ship against the AIS archive
**in-process**: this project references `DarkVessel.Core` and
`DarkVessel.Infrastructure` from [`dark-vessel-detection/`](dark-vessel-detection)
directly, and `DarkVesselMatchService` runs `Matcher` against `MongoAisSource`.
There is no second service to keep alive for scenes to work. (`DarkVessel.Api`
still exists in that folder for the live AIS collector and its console — see its
own [README](dark-vessel-detection/README.md) — but scene matching no longer
goes through it.)

`GET /api/scenes/{name}` returns **only ships reported as `"Dark"`** — not
every ship the model detected. `"UnknownNoCoverage"` (AIS wasn't being listened
for at that time) and `"Matched"` (a real vessel explains the detection) are
both filtered out.

The archive connection comes from `Mongo:ConnectionString` (user-secrets, or the
`MONGODB_URI` environment variable); the rest of the `Mongo` section has working
defaults in `appsettings.json`. Leave the connection string blank and matching
switches off — the backend logs a warning at startup and reports every detection
as dark, rather than failing. `DarkVessel:MaxConcurrentMatches` (default 8) caps
how many ships from one scene are matched concurrently, since each match is its
own set of archive queries.

The wire shape of `GET /api/scenes/{name}` is unchanged (still `{ ships, meta }`)
— only which ships appear in `ships`, and `meta.ships`'s count, are affected.
`POST /api/scenes/sync` is untouched and still returns every ship.

## Notes

- Every call happens synchronously inside the request — for a scene the
  pipeline is still processing, `get` will just return whatever the Jetson
  service returns (a "not found" error), same as the CLI.
- Scene and image names are validated against `^[A-Za-z0-9_.\-]+$` before
  being passed anywhere, since they end up as filenames and process
  arguments.
- `jetson_client.sh` now reaches the Jetson over HTTP with a bearer token
  (`BAKLAVA_TOKEN`), not SSH. Make sure that token is set in the environment
  this backend (and thus `jetson_client.sh`) runs in — see `../jetson/`.
