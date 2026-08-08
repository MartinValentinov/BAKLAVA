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
| GET    | `/api/scenes`             | `jetson_client.sh list` — processed scene names |
| GET    | `/api/scenes/{name}`      | `jetson_client.sh get NAME` — one scene's JSON |
| POST   | `/api/scenes/sync`        | `jetson_client.sh get-all`, then reads the pulled JSONs back and returns them |
| POST   | `/api/scenes/available`   | `jetson_client.sh list-images` — raw, not-yet-processed image names |
| POST   | `/api/scenes/{name}/process` | `jetson_client.sh process NAME` — runs detection on the Jetson, blocks until done |
| POST   | `/api/coords?name=`       | `jetson_client.sh send-coords`   |

`POST /api/coords` expects a raw JSON body (the coordinates payload) and
an optional `?name=` query param.

`POST /api/scenes/{name}/process` responds `{ ok: true, message }` on
success or `502 { ok: false, error }` on failure — the Jetson-side service
marks the image processed on success, so it drops out of future
`list-images` results.

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
