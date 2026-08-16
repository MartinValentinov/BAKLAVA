# Dark Vessel — .NET backend

Rewrite of `ais_web/` (PHP console + collector) and `intelligence/matching.py`
(Python) as one ASP.NET Core app.

**The archive lives in MongoDB.** `MongoAisSource` reads `ships.ships_table`
on the Atlas cluster — the import of the old MySQL `ais_positions` table —
and is what `Ais:Source` selects by default. The two older backends are still
in the codebase and remain a one-setting switch away:

| `Ais:Source` | Backend | Notes |
|---|---|---|
| `mongo` (default) | `MongoAisSource` | MongoDB Atlas, `Mongo:*` settings. |
| `http` | `HttpAisSource` | The Baklava HTTP API (`https://supm.online/api/`), which fronted MySQL from the same host. |
| `mysql` | `AisStore` | Direct MySQL via Dapper; MySQL's own port (3306) was never reachable from outside the hosting server. |

`ships_table` keeps the export's own column shapes, which `MongoAisSource`
absorbs: `ts` is text (`"31/07/2026 18:10"`, and the hour is not always
padded — `"05/08/2026 6:54"`), and MySQL NULLs arrive as the string `"NULL"`.
Because `ts` is text, a range query would compare lexicographically
(`"01/08" < "31/07"`), so a time window is queried as an `$in` of every minute
in it, answered by the `{ts, lat, lon}` index the source creates on first use.

**Building a separate UI against this?** See [`API_REFERENCE.md`](API_REFERENCE.md)
for the full endpoint contract — request/response shapes, auth header, CORS notes.
The UI can be any framework, on any port/domain; it only needs that contract,
never this C# code.

## Layout

| Project | Role |
|---|---|
| `src/DarkVessel.Core` | Pure domain logic: `GeoUtils`, `Detection`/`AisPosition`/`Candidate`/`MatchResult`, `IAisSource`, `IAisArchive` (reads + the collector's writes), `InMemoryAisSource`, `Matcher`, shared `PositionWrite`/`VesselWrite`/`ArchiveStats` models. Zero I/O — direct port of `intelligence/matching.py` + `geo_utils.py`. |
| `src/DarkVessel.Infrastructure` | `MongoAisSource` (the backend actually in use — MongoDB Atlas), `HttpAisSource` (the Baklava HTTP API), `AisStore` (Dapper + MySqlConnector, direct MySQL — see above), `AisStreamCollectorService` (a `BackgroundService` replacing `collector.php` + `lib/websocket.php`), `CollectorState` (shared status, replaces `runtime/state.json`). |
| `src/DarkVessel.Api` | ASP.NET Core host: minimal API (`/api/status`, `/api/control/start|stop`, `/api/match`, `/api/archive-stats`) + a small static console at `/`. Replaces `index.php`/`control.php`/`status.php`/`auth.php`. |
| `tests/DarkVessel.Core.Tests` | xUnit, 7 tests, entirely against `InMemoryAisSource` — direct port of `intelligence/test_matching.py`. No database needed to run these. |

## What's different from the PHP version, on purpose

- **One process, not a script + a spawned CLI + a cron watchdog.** `AisStreamCollectorService` is a hosted `BackgroundService` inside the same app that serves the API. As long as whatever runs `dotnet DarkVessel.Api.dll` keeps the process alive, the collector reconnects on its own.
- **Start/Stop is an in-memory flag (`CollectorState`)**, not a file (`runtime/state.json`).
- **Auth is a placeholder.** `/api/*` checks a shared `X-Api-Key` header against `Api:ApiKey` in config. Fine for one trusted operator on your own machine; not production auth.
- **Coverage is inferred, not read from a ledger.** The import carries positions only, so `HadCoverageAsync` approximates coverage by checking whether *any* AIS position exists near the requested moment — reasonable (nothing gets written while the collector is off), but coarser than a real ledger, which could tell "collector on, this region just quiet" apart from "collector off." A detection outside the archive's window (currently 31 Jul – 7 Aug 2026) therefore comes back `UnknownNoCoverage`, not `Dark`. `MongoAisSource` does record collector sessions in an `ais_coverage` collection going forward; `HttpAisSource`'s equivalents stay no-ops (logged once, not silent).

## Running it

```bash
cd backend/dark-vessel-detection
dotnet build                                    # whole solution
dotnet test tests/DarkVessel.Core.Tests         # 7 tests, no DB needed
```

### Configure secrets (never in appsettings.json — that file is committed)

```bash
cd src/DarkVessel.Api
dotnet user-secrets set "Mongo:ConnectionString" "mongodb+srv://user:password@cluster0.example.mongodb.net/?appName=Cluster0"
dotnet user-secrets set "AisStream:ApiKey" "your-aisstream.io-key"
dotnet user-secrets set "Api:ApiKey" "pick-any-random-string-for-yourself"
```

`user-secrets` writes to a JSON file **outside the repo** (`%APPDATA%\Microsoft\UserSecrets\<id>\secrets.json` on Windows) — this is the .NET equivalent of the leaked `config.php` problem: it exists specifically so a real credential never has to touch a file git tracks. `Mongo:ConnectionString` also falls back to the `MONGODB_URI` environment variable.

Everything else about the archive has a working default in `appsettings.json` (`Mongo:Database` `ships`, `Mongo:PositionsCollection` `ships_table`, `Mongo:TimeZone` `UTC`). `Baklava:ApiKey` is only needed for `Ais:Source` `http`, `ConnectionStrings:AisArchive` only for `mysql`.

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

Verified working (2026-08-15) against the MongoDB archive (1,048,575 position rows, 31 Jul – 7 Aug 2026) — returns a real `MatchResult` with `status` as a readable string (`"Matched"` / `"Dark"` / `"UnknownNoCoverage"`), not a bare number.

## Known gaps

- Coverage is inferred from position rows, not a ledger — see above.
- No real authentication yet (see above).
- Onboard detections still need a timestamp + heading before they can flow into `/api/match` for real.