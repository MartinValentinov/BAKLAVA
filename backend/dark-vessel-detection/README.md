# Dark Vessel — .NET backend

Rewrite of `ais_web/` (PHP console + collector) and `intelligence/matching.py`
(Python) as one ASP.NET Core app.

**Talks to the database over HTTP, not a direct MySQL connection.** MySQL's
own port (3306) is not reachable from outside the hosting server — proven by
testing, not assumed. Instead, `HttpAisSource` calls the already-deployed
Baklava HTTP API (`https://supm.online/api/`), which runs on the same host
MySQL does and reaches it via `localhost`. `AisStore` (direct MySQL via
Dapper) still exists in the codebase and stays a drop-in alternative if a
direct connection ever becomes available (SSH tunnel, a different host,
etc.) — swapping back is a one-line change in `Program.cs`.

**Building a separate UI against this?** See [`API_REFERENCE.md`](API_REFERENCE.md)
for the full endpoint contract — request/response shapes, auth header, CORS notes.
The UI can be any framework, on any port/domain; it only needs that contract,
never this C# code.

## Layout

| Project | Role |
|---|---|
| `src/DarkVessel.Core` | Pure domain logic: `GeoUtils`, `Detection`/`AisPosition`/`Candidate`/`MatchResult`, `IAisSource`, `InMemoryAisSource`, `Matcher`, shared `PositionWrite`/`VesselWrite`/`ArchiveStats` models. Zero I/O — direct port of `intelligence/matching.py` + `geo_utils.py`. |
| `src/DarkVessel.Infrastructure` | `HttpAisSource` (the backend actually in use — calls the Baklava HTTP API), `AisStore` (Dapper + MySqlConnector, direct-MySQL alternative, currently unreachable — see above), `AisStreamCollectorService` (a `BackgroundService` replacing `collector.php` + `lib/websocket.php`), `CollectorState` (shared status, replaces `runtime/state.json`). |
| `src/DarkVessel.Api` | ASP.NET Core host: minimal API (`/api/status`, `/api/control/start|stop`, `/api/match`, `/api/archive-stats`) + a small static console at `/`. Replaces `index.php`/`control.php`/`status.php`/`auth.php`. |
| `tests/DarkVessel.Core.Tests` | xUnit, 7 tests, entirely against `InMemoryAisSource` — direct port of `intelligence/test_matching.py`. No database needed to run these. |

## What's different from the PHP version, on purpose

- **One process, not a script + a spawned CLI + a cron watchdog.** `AisStreamCollectorService` is a hosted `BackgroundService` inside the same app that serves the API. As long as whatever runs `dotnet DarkVessel.Api.dll` keeps the process alive, the collector reconnects on its own.
- **Start/Stop is an in-memory flag (`CollectorState`)**, not a file (`runtime/state.json`).
- **Auth is a placeholder.** `/api/*` checks a shared `X-Api-Key` header against `Api:ApiKey` in config. Fine for one trusted operator on your own machine; not production auth.
- **No `ais_coverage` ledger via the Baklava API.** That API has no resource for it (no GET or POST). `HttpAisSource.HadCoverageAsync` approximates coverage by checking whether *any* AIS position was recorded near the requested moment — reasonable (nothing gets written while the collector is off), but coarser than a real ledger, which could tell "collector on, this region just quiet" apart from "collector off." Worth asking about adding a `coverage` resource to `readers.php`/`writers.php` if this matters later. The collector's `OpenCoverageAsync`/`TouchCoverageAsync`/`CloseCoverageAsync` are no-ops for the same reason (logged once, not silent) — positions and vessels still write for real.

## Running it

```bash
cd backend/dark-vessel-detection
dotnet build                                    # whole solution
dotnet test tests/DarkVessel.Core.Tests         # 7 tests, no DB needed
```

### Configure secrets (never in appsettings.json — that file is committed)

```bash
cd src/DarkVessel.Api
dotnet user-secrets set "Baklava:ApiKey" "the key from your friend"
dotnet user-secrets set "AisStream:ApiKey" "your-aisstream.io-key"
dotnet user-secrets set "Api:ApiKey" "pick-any-random-string-for-yourself"
```

`user-secrets` writes to a JSON file **outside the repo** (`%APPDATA%\Microsoft\UserSecrets\<id>\secrets.json` on Windows) — this is the .NET equivalent of the leaked `config.php` problem: it exists specifically so a real credential never has to touch a file git tracks.

`Baklava:BaseUrl` defaults to `https://supm.online/api/` (see `BaklavaApiOptions.cs`) — only set it via config if that ever changes. `ConnectionStrings:AisArchive` is only needed if you switch back to `AisStore`.

### Run it

```bash
dotnet run --project src/DarkVessel.Api
```

Then open `http://localhost:5252` (or whatever port it prints) — enter your `Api:ApiKey` when prompted, click START to run the live collector, or use the "Match a detection" form to call `/api/match` directly against the real archive.

### Smoke-test the connection without the UI

```bash
curl -X POST http://localhost:5252/api/match \
  -H "X-Api-Key: your-api-key" -H "Content-Type: application/json" \
  -d '{"detectionId":"smoke-1","lat":42.5,"lon":27.2,"timestampUtc":"2026-08-01T12:00:00Z"}'
```

Verified working (2026-08-08) against the real archive (2,000,257 real position rows) — returns a real `MatchResult` with `status` as a readable string (`"Matched"` / `"Dark"` / `"UnknownNoCoverage"`), not a bare number.

## Known gaps

- No `ais_coverage` resource on the Baklava API — see above.
- No real authentication yet (see above).
- Onboard detections still need a timestamp + heading before they can flow into `/api/match` for real.