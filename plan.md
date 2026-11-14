# Usage Monitor — implementation plan

Read `project-description.md` first. It says *what* the app does. This file says
*how* to build it.

## Status (2026-11-14)

- **Done (phases 0–4, most of 5):** tray app, table window, `.conf` parser,
  folder watch, worker thread, WinHTTP, steps (`http(s):`, `file:`), extractors
  (`body`, `status`, `header:`, `json:`, `regex:`, `between:`), `require.*`,
  variables and secrets, the 3 indicator types, colors, tray tooltip, log.
- **First service:** `services/claude.conf` — working with real data since
  2026-11-13 (cookie from keep-sessions, see 4.6): 5-hour, week, and the
  per-model weekly limit (`limits[kind=weekly_scoped]`).
- **Table UI (2026-11-14):** columns Service | Remained… | Updated (green/red,
  error in tooltip) | refresh button. Tray left-click toggles the window.
  Window is sized to the data on each open.
- **Table UI:** anti-aliased pies and per-row refresh buttons are drawn by
  `src/draw.cpp` (own 4x4 supersampling + `AlphaBlend`, no GDI+). Column 0
  (icon + name) is custom-drawn too: an image list loses the icon alpha here.
- **API balances (2026-11-14):** `services/soniox.conf` and
  `services/deepseek.conf` — USD balance with an API key from Credential
  Manager (copied once from Bitwarden with `tools\set-secret.ps1 -FromEnv`),
  every 30 min. `value` indicators with `prefix = $`, no pie.
- **SMS balances (2026-11-14):** `services/msgway.conf` (REST, IRT only) and
  `services/mihansms.conf` (SOAP `accountInfo` of the main account: IRT + SMS
  left as a 2nd column), every 60 min. Both APIs give Rial → `factor = 0.1`.
  The key / password is read with `{{file:...}}` from `my-own-token.txt` in
  `E:\www\=providers\sms\<provider>\` on every run, so the owner rotates it
  by editing that file. Tech refs are in the same folders.
- **OpenRouter (2026-11-14):** `services/openrouter.conf`, management key in
  Credential Manager (`UsageMonitor/OpenRouter`, from Bitwarden
  `openrouter-management`). Left = `total_credits − total_usage`: a `value`
  indicator can now take `total` + `used` instead of `value`.
- **Row order + no "left" (2026-11-14):** every service has `order`
  (Claude 10, Soniox 20, ElevenLabs 25, DeepSeek 30, OpenRouter 40, MihanSMS 50, MsgWay 60, Asale 70).
  Money indicators have no `label = left` (the column is already "Remained").
  The table draws very light gray grid lines (`paintGrid` in main.cpp).
- **ElevenLabs (2026-11-14):** `services/elevenlabs.conf`, API key in Credential
  Manager (`UsageMonitor/ElevenLabs`, from Bitwarden `Elevenlabs`). Credits left
  (`character_limit − character_count`, pie with `max`) + days left until the plan
  renews / ends (new indicator type `days`, see 4.5). No `label = left` on the days
  either (removed 2026-11-14): the cell is `22 days (12/7 4:05)`.
- **Asale (2026-11-14):** `services/asale.conf`, USDT wallet of the Asale client on
  deb21 (`E:\www\myLan\deb21.md`): `POST http://192.168.1.21:9700/rpc/wallet_overview`
  with header `X-Asale-Token` = `/root/.asale/daemon.token`, copied once to Credential
  Manager (`UsageMonitor/Asale`). `balance` is micro USDT → `factor = 0.000001`,
  shown `3.93 USDT`. Last row (`order = 70`). Port 9700 is open only while Asale is
  enabled on deb21 (`asale.sh` option 1); when it is off, the row shows an error.
- **Usage spike alarm (2026-11-14):** `spike_used` + `spike_minutes` on an indicator
  (see 4.5b). Set for Claude (5h, week), Soniox, ElevenLabs, DeepSeek, OpenRouter.
  Code: `src/spike.cpp`.
- **Service icons (2026-11-14):** real logos from the site favicons
  (`https://www.google.com/s2/favicons?domain=<site>&sz=256` -> `tools/logo_to_ico.py`).
  MihanSMS keeps a green dot: its home page is empty and it has no favicon.
- **Auto start (2026-11-14):** DelayedTaskRunner line `00:04:00,E:\www\usage-monitor\build\usage-monitor.exe`.
- **Usage history + report (2026-11-14):** daily log files `history\*.tsv`,
  auto-deleted after 30 days; per-row report button (purple, column-chart icon)
  starts `usage-monitor.exe --report <file>` in a separate process, which
  writes `reports\<service>.html` and opens the browser. See 4.8.
- **Measured (3 services, 2026-11-14; same with 5):** window open 4.3 MB private / 12 MB
  working set, back to 2.7 MB working set after close (private stays 4.3 MB).
  Earlier, with 1 service: idle 3.2 MB private / 1.7 MB working set.
- **Not built yet:** `exec:` steps, `cookie:` extractor, `last-values.ini`,
  `-Install` in `build.ps1`, unit-test exe (the engine was tested with test
  `.conf` files in `E:\tmp\umtest`), home FTTH service.

---

## 1. Stack choice (lowest memory)

Rough, typical memory of a small tray app with one table window
(numbers are estimates, not measured on this laptop):

| Stack | Private memory | Notes |
|---|---|---|
| **C++ + plain Win32 + WinHTTP** | **~1–3 MB** | Chosen. Only Windows DLLs, no runtime. |
| Rust + windows-sys | ~2–4 MB | Also good, but Rust is not installed and the toolchain is big. |
| Go + raw Win32 calls | ~6–12 MB | Go is installed, but the Go runtime and GC cost more memory. |
| Go + walk (like `claude-usage`) | ~15–25 MB | |
| C# WinForms (.NET 8) | ~30–60 MB | |
| Python + Qt/Tk | ~25–60 MB | |
| Tauri / Electron | ~40–150 MB | Browser engine. |

**Decision: C++17, plain Win32 API, no framework.**

- UI: `Shell_NotifyIconW` (tray), `SysListView32` (table), a popup menu.
- HTTP: **WinHTTP** (part of Windows). TLS comes from Windows (SChannel), so no
  OpenSSL inside the exe.
- JSON: vendored **cJSON** (MIT, one `.c` + one `.h`).
- Regex: `std::regex` (ECMAScript). Plus a simple `between:` extractor that
  needs no regex.
- No GDI+, no WIC, no WebView, no COM UI. Icons are `.ico` only (`LoadImageW`).
- Static link of the C++ runtime → one `.exe`, no extra DLLs.

### Toolchain

No C/C++ compiler is installed now (only Go and .NET). Install **one** of:

1. **WinLibs** (GCC + MinGW-w64, UCRT) — recommended:
   `winget install BrechtSanders.WinLibs.POSIX.UCRT`
   Then check: `g++ --version`, `windres --version`.
2. **w64devkit** (portable zip, no installer) — unzip to `E:\tools\w64devkit`
   and add its `bin` to PATH.

Do not install Visual Studio Build Tools (several GB, heavy for this laptop).

**Installed:** WinLibs GCC 16.2 (winget). `build.ps1` finds it in the winget
folder when it is not in PATH.

**Linker issue:** MinGW links its own `default-manifest.o` as manifest ID 1,
and this `ld` version fails ("multiple non-default manifests" or a silent
exit code 5) when we add a second ID 1 manifest. So `res/app.rc` puts our
manifest under **ID 2**, and `main.cpp` activates it at startup
(`CreateActCtxW` + `ActivateActCtx` → common controls v6) and calls
`SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`.

Build flags (in `build.ps1`):

```
g++ -std=c++17 -Os -s -municode -mwindows -static -ffunction-sections -fdata-sections
    -Wl,--gc-sections  src\*.cpp src\cJSON.c build\app.res.o
    -lwinhttp -lcomctl32 -lshell32 -ladvapi32 -lpsapi -luser32 -lgdi32
    -o build\usage-monitor.exe
```

(`-municode` = `wWinMain` entry point. `-mwindows` = no console window.)

---

## 2. Project layout

```
(The real layout. The first plan had more, smaller files; they were merged.)

usage-monitor/
  src/
    main.cpp            wWinMain, --report mode, single instance, hidden main window,
                        tray, table window (ListView, custom draw), worker thread
    config.cpp/.h       .conf (INI) parser -> Service structs, validation
    runner.cpp/.h       runs the steps of one service: variables, extractors, indicators
    http.cpp/.h         WinHTTP wrapper (session, cookies, redirects, proxy, timeout)
    json.cpp/.h         small JSON path reader (no tree kept)
    crypto.cpp/.h       keep-sessions decrypt (PBKDF2 + AES-GCM, Windows BCrypt)
    tz.cpp/.h           time zones (IANA via icu.dll), short time text
    draw.cpp/.h         anti-aliased pie, refresh and report buttons
    history.cpp/.h      [app] settings, daily history logs, cleanup
    spike.cpp/.h        usage spike alarm (readings in memory, worker thread only)
    report.cpp/.h       --report: reads history, fills res/report.html, opens browser
    util.cpp/.h         UTF-8/16, strings, files, log
  res/
    app.rc              icon, manifest (ID 2), report template (RCDATA 3), version info
    app.ico             tray + window icon (16/20/24/32/48 px inside)
    app.manifest        comctl32 v6, PerMonitorV2 DPI, asInvoker
    report.html         report page template (inline SVG charts, light/dark)
  services/             sample files, copied next to the exe by the build (never overwritten)
    claude.conf / .ico
    soniox.conf / .ico
    deepseek.conf / .ico
    elevenlabs.conf / .ico  (and openrouter, msgway, mihansms)
  tools/
    make_icons.py       makes app.ico and mihansms.ico (a dot: that site has no logo; no libraries)
    logo_to_ico.py      real service logo (site favicon PNG) -> .ico 16-48 px (Pillow)
    set-secret.ps1      saves a secret in Windows Credential Manager (hidden input or -FromEnv)
    uitest.ps1          opens the table, screenshot, centering and memory check
  build.ps1             build, -Run
  reference-docs/.gitkeep
  project-description.md
  plan.md
  .gitignore  .gitattributes
```

---

## 3. Runtime design

### 3.1 Threads

Only **two** threads:

1. **UI thread** — hidden main window, tray icon, table window, message loop.
2. **Worker thread** — runs all fetches **one after another** (never in parallel,
   so only one HTTP response is in memory at a time).

The worker waits with `WaitForMultipleObjects` on:

- `stopEvent` — app is exiting,
- `refreshEvent` — user pressed F5 / app resumed from sleep,
- the folder change handle (`FindFirstChangeNotificationW` on `services\`,
  filter: file name + last write) → reload configs after 500 ms (debounce),
- a timeout = time until the next service is due.

When a service is done, the worker writes the result into the shared result
array (protected by an `SRWLOCK`) and does
`PostMessageW(hiddenWnd, WM_APP_RESULT, serviceIndex, 0)`.
The UI thread then updates one ListView row and the tray tooltip.

Worker thread stack: reserve 4 MB (`CreateThread` stack size, reserve only —
not committed). `std::regex` uses recursion and can need a deep stack.

### 3.2 Hidden main window and exit

- Create a **hidden top-level window** (not `HWND_MESSAGE`). Reason:
  `stop-app.ps1` sends `WM_CLOSE` to top-level windows; message-only windows
  are not found by `EnumWindows`, so the app would be killed instead of closed.
- `WM_CLOSE` on it → same as menu **Exit**.
- Exit steps: set `stopEvent`, close the open WinHTTP request handle (this
  cancels a blocked call), wait for the worker up to 3 s, `NIM_DELETE` the tray
  icon, `PostQuitMessage(0)`.

### 3.3 Single instance

At the very start of `wWinMain`, before any UI:

```cpp
HANDLE m = CreateMutexW(nullptr, TRUE, L"Local\\UsageMonitorSingleInstance");
if (GetLastError() == ERROR_ALREADY_EXISTS) {
    MessageBoxW(nullptr, L"Usage Monitor is already running.", L"Usage Monitor",
                MB_OK | MB_ICONERROR | MB_TOPMOST);
    return 1;
}
```

### 3.4 Tray icon

- `NOTIFYICONDATAW` with `NOTIFYICON_VERSION_4`, callback `WM_APP_TRAY`.
- `WM_LBUTTONUP` / `NIN_SELECT` / `NIN_KEYSELECT` → Open.
- `WM_CONTEXTMENU` → popup menu: **Open** (default, `SetMenuDefaultItem`),
  **Settings**, separator, **Exit**. Call `SetForegroundWindow` before
  `TrackPopupMenu` and post `WM_NULL` after (otherwise the menu does not close).
- **Settings** → create `services\` if missing, then
  `ShellExecuteW(nullptr, L"open", servicesDir, …)`.
- Handle the `TaskbarCreated` message (`RegisterWindowMessageW`) → add the icon
  again after Explorer restarts.
- Tooltip (`szTip`, max 127 chars): `name: indicator text` for every indicator
  with `tray = 1`, cut to fit. If any service is in error, add `(1 error)`.
- Optional (phase 6): small colored dot drawn on the tray icon when any
  indicator is in `alert` state. Only if it costs nearly no memory.

### 3.5 The table window

- Created on **Open**, **destroyed** on close (`DestroyWindow`, not hide).
  The ImageList and fonts are destroyed with it. Data stays in the small
  result structs, so reopening is instant.
- If it is already open: `ShowWindow(SW_RESTORE)` + `SetForegroundWindow`.
- One `SysListView32` in report mode, fills the client area.
  Extended styles: `LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER`. Only the
  first column has an icon (small ImageList).
- Columns: `Service` | `Usage` | *(blank header)* × (N−1) | `Updated` | `Status`.
  N = the largest indicator count across enabled services (at least 1).
- Cell text for an indicator: `Label: value text` (label omitted if empty).
- Colors with `NM_CUSTOMDRAW` (`CDDS_ITEMPREPAINT | CDDS_SUBITEM`):
  normal / orange (`warn`) / red (`alert`) / grey (`stale`, last fetch failed).
- Icons: `ImageList_Create` at `GetSystemMetricsForDpi(SM_CXSMICON)`; each
  service `.ico` loaded with `LoadImageW(LR_LOADFROMFILE)` at that size; a
  default icon for services without one.
- Size: auto-size columns (`LVSCW_AUTOSIZE_USEHEADER`), then window width =
  sum of columns + borders, height = rows + header, limited to 90% of the work
  area. Remember the last size the user set in `last-values.ini` `[window]`.
- **Centering** (desktop rule): after the final size is known and **before**
  `ShowWindow`, move it to the center of the work area (`MONITORINFO.rcWork`)
  of the monitor under the cursor (`MonitorFromPoint(GetCursorPos)`).
- Keys: `F5` = refresh all now, `Esc` = close. Double-click a row = open the
  `site` URL of the service in the browser (`ShellExecuteW`).
- Font: the system message font (`SystemParametersInfoForDpi`
  `SPI_GETNONCLIENTMETRICS` → `lfMessageFont`). Handle `WM_DPICHANGED`.

### 3.6 Memory rules

- Read the HTTP body into one `std::string`, cap it at `max_body` (default
  2 MB). Free it right after the step's `get.*` extractions.
- New WinHTTP session per service run; close all handles at the end. No
  connection pool kept between runs.
- Do not keep cJSON trees; parse, extract, `cJSON_Delete`.
- After each full fetch round and after the window closes, call
  `SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1)` to
  give unused pages back to Windows.
- No logging of full response bodies (only with `debug = 1` in a service,
  and then only the first 2 KB).

### 3.7 Sleep / resume and network

- `WM_POWERBROADCAST` + `PBT_APMRESUMEAUTOMATIC` → refresh all after 30 s.
- On a network error: retry once after 60 s, then wait for the normal interval.
- Keep the last good values; mark them stale (grey) and show the error in
  `Status`.

---

## 4. Config file format

- Folder: `<exe folder>\services\`. Only `*.conf` files are loaded. Files that
  start with `_` are ignored.
- Encoding: UTF-8 (BOM allowed). Comments are lines that start with `;` or `#`.
  There are no comments after a value (a value can contain `;`, e.g. in a regex).
- A value in double quotes keeps its spaces: `suffix = " days"`.
- `key = value`. Spaces around `=` and at line ends are trimmed.
- Section order matters: steps run in the order they appear.
- Unknown keys → a warning in the log (to catch typos), not an error.
- Parse errors → the service shows in the table with `Status = Config error:
  line 12: ...`. It never crashes the app.

### 4.1 `[service]`

| Key | Default | Meaning |
|---|---|---|
| `name` | file name | Name in the table. |
| `timezone` | PC zone | The **service** zone: IANA (`Europe/Helsinki`) or Windows name (`FLE Standard Time`). API times **without** an offset are read in it. Times are shown in it only when the app has no display zone (see 4.1b). IANA → Windows via `icu.dll` (`ucal_getWindowsTimeZoneID`, loaded only for the lookup). |
| `icon` | — | `.ico` file, path relative to the `services\` folder. |
| `enabled` | `1` | `0` = not shown and not fetched. |
| `order` | `100` | Row order (then by name). |
| `interval` | `60` | Minutes between fetches. |
| `timeout` | `20` | Seconds per request. |
| `proxy` | `system` | `system`, `none`, or `host:port`. |
| `user_agent` | `UsageMonitor/0.1 (Windows)` | Cloudflare blocks WinHTTP that claims to be Chrome. |
| `site` | — | URL opened on double-click. |
| `max_body` | `2097152` | Max response bytes read. |
| `debug` | `0` | `1` = log status, headers and first 2 KB of each body. |

### 4.1b App settings: `usage-monitor.ini` next to the exe

```ini
[app]
timezone=Asia/Tehran     ; display zone for ALL times (reset times, Updated, report); read at startup
history_days=30          ; keep (and read in the report) this many days of history logs; default 30
week_start=sat           ; first day of the week in the report: sun..sat; default sat
spike_sound=             ; .wav file for a usage spike; empty = the Windows "Exclamation" sound
[window]                 ; written by the app: last window position
left=... top=... right=... bottom=... maximized=0
```

Time handling has exactly two steps, so nothing is converted twice:
1. **Parse to UTC** (`parseTime`): epoch → UTC; ISO with `Z`/`+hh:mm` → that
   offset; ISO without offset → the service `timezone`.
2. **Show** (`shortTime`): UTC → `[app] timezone` if set, else the service
   zone (reset times) or the PC zone (Updated). Reset times are rounded to the
   minute, Updated is truncated.

### 4.2 `[step.<id>]` — one request or read

Steps share one **cookie jar** (one WinHTTP session per run), so a login step
followed by a data step works.

| Key | Meaning |
|---|---|
| `url` | `https://…`, `http://…`, `file:<path>` (env vars expanded), or `exec:<command line>` (stdout is the body, 30 s limit, hidden window — **not built yet**). |
| `method` | `GET` (default), `POST`, `PUT`. |
| `header.<Name>` | A request header. Many allowed. |
| `body` | Raw request body. |
| `form.<name>` | Form fields → `application/x-www-form-urlencoded` body (values are URL-encoded). |
| `json.<path>` | (later, if needed) build a JSON body. |
| `follow_redirects` | `1` (default) / `0`. |
| `expect_status` | e.g. `200` or `200,302`. Default: any 2xx/3xx. Other → error. |
| `get.<var>` | Read a value from the response into variable `<var>` (see 4.3). |
| `fail.<status>` | Status text to show for this HTTP status, e.g. `fail.403 = Session expired`. `fail.notfound` = text when a `get.*` finds nothing. |
| `decrypt` / `passphrase` | `decrypt = keep-sessions`: the body is a keep-sessions blob; it is decrypted before `get.*` (a blob without `enc` passes unchanged). |
| `require.<var>` | Check a variable. If the check fails → stop the run, `Status = Blocked: <var>=<value>`, keep old values, do **not** run later steps. |

### 4.3 Extractors (for `get.*`)

| Form | Reads |
|---|---|
| `body` | whole body, trimmed |
| `status` | HTTP status code |
| `json:a.b[0].c` | JSON path. Also `list[kind=session].percent` (first item where `kind == "session"`). |
| `regex:<pattern>` | first capture group of the first match (or the whole match if no group). |
| `between:<start>\|<end>` | text between two plain strings. Safe for big HTML. |
| `header:<Name>` | response header |
| `cookie:<Name>` | cookie value from `Set-Cookie` (**not built yet**) |

If a `get.*` finds nothing → error `Value not found: <var>` (unless the key is
`get?.<var>` = optional, then the variable is empty).

### 4.4 Variables and secrets

Used in `url`, `header.*`, `body`, `form.*`, and indicator keys:

| Form | Value |
|---|---|
| `{{var}}` | a variable from `get.*` |
| `{{env:NAME}}` | environment variable |
| `{{cred:TARGET}}` | password of a Windows Credential Manager generic credential (`CredReadW`) |
| `{{cred.user:TARGET}}` | user name of that credential |
| `{{file:PATH}}` | content of a local file, trimmed |
| `{{x\|url}}` | URL-encode the value |

Store a secret once, not in the `.conf` file:
`powershell -File tools\set-secret.ps1 -Target UsageMonitor/ISP -User myname`
(hidden input; `{{cred.user:...}}` reads the user name).

### 4.5 `[indicator.<id>]`

**Rule (all services): an indicator shows what is LEFT / available** — the
column title is "Remained". If the API gives "used", convert it (`invert = 1`
for percent; `used` + `total` for ratio). **Never write `left` / `remaining`
in a cell** (`label`, `text`, `suffix`): the column title already says it.

| Key | Used by | Meaning |
|---|---|---|
| `label` | all | Short text **after** the value: cell = `<value> <label> (<reset>)`, e.g. `64% Fable (Tue 4:00)`. |
| `type` | all | `ratio`, `value`, `percent`, or `days`. |
| `date` | days | A time (ISO 8601 or epoch) when the plan ends / renews. Shows `23 days (12/7 4:05)`, `5 h` on the last day, `expired` after it. Colors (`warn_below` …) compare the days left. Not written to the history log. |
| `remain` / `used` | ratio | Give one. If `used` is given: remain = total − used. |
| `total` | ratio | Total amount. |
| `value` | value | The number. Or give `total` + `used`: value = total − used. |
| `percent` | percent | 0–100. Or leave empty and give `remain` + `total`. |
| `invert` | percent | `1` → show `100 − percent` (e.g. API gives "used", we show "left"). |
| `factor` | all | Multiply input numbers (e.g. `0.01` for cents). |
| `scale` | ratio, value | `bytes` / `kb` / `mb` / `gb` = input unit; output is auto-scaled to the best unit (`25.3 GB`). |
| `unit` / `prefix` / `suffix` | all | Plain text, e.g. `prefix = $`, `suffix = " days"`. |
| `thousands` | value | `1` = thousands separators: `184,387`. |
| `decimals` | all | Digits after the point. Default: 0 for percent, 1 for bytes, 2 for value. |
| `text` | all | Free text after the value. Not `left` (see the rule above). |
| `warn_below` / `alert_below` | all | Color the cell. Compared with: remaining % (ratio), shown % (percent), the number (value). |
| `warn_above` / `alert_above` | all | Same, for "high is bad". |
| `reset` | all | A time (ISO 8601 or epoch). Shown as `(resets 19:30)` or `(resets Fri 19:30)`. |
| `tray` | all | `1` = include in the tray tooltip. |
| `max` | value, days | Maximum for the pie (pie = value / max; for `days`: the plan length in days). Without it there is no pie. |
| `spike_used` / `spike_minutes` | all | Usage spike alarm, see 4.5b. |
| `pie` | all | `0` = no pie for this indicator. |
| `optional` | all | `1` = skip the indicator when its value is empty (e.g. a limit the plan does not have). |

`label` may use variables, e.g. `label = {{model_name}} week`.

### 4.5b Usage spike alarm

```ini
spike_used = 5        ; the shown number went DOWN by 5 or more (5 %, $5, 5 IRT, 5 credits ...)
spike_minutes = 1     ; ... within at least 1 minute (default 1)
```

- After each successful run the worker compares the new number with the newest
  reading that is **at least** `spike_minutes` old. So when the check interval is
  longer than `spike_minutes` (e.g. 30 min), the drop over that longer time is
  used. Readings are kept in memory only: the first run after start is a baseline.
- On a spike: one sound (`[app] spike_sound` .wav, else the Windows "Exclamation"
  sound), and a red **!** badge next to the Updated time of that row. The tooltip
  says what dropped (`5h: 80% -> 72% in 2 min`). The badge stays until the user
  clicks the Updated cell. The tray tooltip line starts with `(!)` meanwhile.
- After a spike the readings start again from the new number, so one big drop
  rings only once.

Number parsing (from HTML text): take the first number, accept Persian and
Arabic-Indic digits (`۰–۹`, `٠–٩`), Persian decimal/thousand separators, and
remove `,` thousand separators.

### 4.6 Example: Claude (web session)

The OAuth token in `%USERPROFILE%\.claude\.credentials.json` is rejected by
`api.anthropic.com/api/oauth/usage` (HTTP 401, tested 2026-11-13; the user
uses a 1-year token). So the app reads the same numbers that the claude.ai
usage page reads, with the browser **session cookie** `sessionKey`:

1. `GET https://claude.ai/api/organizations` → `[0].uuid`
2. `GET https://claude.ai/api/organizations/<uuid>/usage` → `five_hour`,
   `seven_day`, … (`utilization` 0–100, `resets_at`)

claude.ai answers a plain WinHTTP client with JSON (no Cloudflare browser
challenge), so **Chrome is not needed at runtime**. Chrome is used only once,
to copy the cookie. The cookie is stored in Windows Credential Manager:

```
powershell -ExecutionPolicy Bypass -File tools\set-secret.ps1
```

(`cmdkey ... /pass` with no value saves an **empty** password — tested.)

**Cloudflare:** WinHTTP gets a "Just a moment..." challenge when the
User-Agent claims to be Chrome, but passes with an honest User-Agent
(`UsageMonitor/0.1 (Windows)`, now the default). Python urllib and curl's
default UA are challenged too. If Cloudflare starts to block the app, the
fallback is an `exec:` step or reading the page through a real browser.

Reading the cookie from Chrome's cookie database by code is **not** done:
Chrome encrypts it with app-bound encryption, and reading another program's
cookies is what password-stealing malware does.

**Where the cookie comes from (chosen 2026-11-13): keep-sessions.** The
user's own extension `E:\www\keep-sessions` already pushes the claude.ai
cookies from Chrome to `https://sessionman.peppasoft.com`, encrypted with an
E2E passphrase. The `[step.session]` step pulls `/api/pull?domain=claude.ai`
(Bearer token) and decrypts it with `decrypt = keep-sessions` +
`passphrase = {{cred:...}}` (PBKDF2-SHA256 150k → AES-256-GCM, Windows BCrypt,
`src/crypto.cpp`), then `get.sk = json:cookies[name=sessionKey].value`.
So the key refreshes by itself while Chrome is used, and the app still works
(with the last pushed cookie) when Chrome is closed. The app never depends on
Chrome. Secrets: `UsageMonitor/KeepSessionsToken`, `UsageMonitor/KeepSessionsPass`.
The other option (the extension calls claude.ai and hands the numbers to the
app) was rejected because it makes the app depend on Chrome.

A 403 with a `cf-mitigated` header is reported as "blocked by a Cloudflare
browser check", not as "session expired".

The full file is `services/claude.conf`. It keeps the country gate of
`claude-usage` (`require.country = oneof:Finland,Germany`).

`require.*` check forms: `regex:<pattern>`, `oneof:a,b,c` (case-insensitive,
trimmed), `notempty`.

### 4.7 Example: home FTTH (template — needs the real portal)

```ini
[service]
name = Home FTTH
icon = ftth.ico
interval = 60
; set enabled = 1 after filling the real URLs and patterns
enabled = 0
site = https://my.isp.example/

[step.login]
method = POST
url = https://my.isp.example/login
form.username = {{cred.user:UsageMonitor/ISP}}
form.password = {{cred:UsageMonitor/ISP}}
expect_status = 200,302

[step.dashboard]
url = https://my.isp.example/dashboard
get.remain = regex:Remaining\s*:\s*([\d.,۰-۹]+)\s*GB
get.total  = regex:Total\s*:\s*([\d.,۰-۹]+)\s*GB
get?.days  = regex:(\d+)\s*days left

[indicator.data]
label = Data
type = ratio
remain = {{remain}}
total = {{total}}
scale = gb
warn_below = 20
alert_below = 5
tray = 1

[indicator.days]
type = value
value = {{days}}
suffix = " days"
decimals = 0
```

If the portal has a CSRF token: add `get.csrf = regex:name="_token" value="([^"]+)"`
on a first GET step and send `form._token = {{csrf}}` in the login step.

### 4.8 Usage history and report

**History log** (`src/history.cpp`). After each successful run the worker
appends one line per indicator to `history\YYYY-MM-DD.tsv` (date in the display
zone; UTF-8, tab separated, append-only, opened only for the write):

```
<utc epoch> TAB <service file> TAB <indicator id> TAB <number> TAB <pre> TAB <post> TAB <label>
1794655421	claude.conf	five	73		%	5h
1794655422	deepseek.conf	balance	3.89	$		left
```

`<number>` is what the cell shows: the percent (after `invert`), the remaining
amount of a ratio, or the value. `<post>` = `B` means bytes. One file per day
for all services (Claude every 15 min = about 300 lines, 10 KB per day).

**Cleanup:** files whose date is older than `history_days` (default 30) are
deleted once per day by the worker and at the start of every report.

**Report** (`src/report.cpp`, `res/report.html`). The row button runs
`usage-monitor.exe --report <file>` with `CreateProcessW`: a **separate
process**, so the tray app never loads history or HTML. That process (no
mutex, no window):

1. reads `[app]` settings, deletes old logs,
2. reads only the files of the last `history_days` days, keeps the lines of
   that service, converts each time to a "local epoch" (UTC clock = display
   zone wall time, so the page needs no time zone code),
3. puts the data as JSON into the template (resource `RCDATA 3`, mark
   `/*DATA*/null`), writes `reports\<service>.html` (overwritten),
4. opens it with `ShellExecuteW` and exits. `--no-open` (3rd argument) only
   writes the file (tests).

The page: **Today** (now / used today / line by hour), **This week** (line,
bars of use per day, one button per day: that day by hour), **This month**
(line + daily bars). "Used" = sum of the drops of the remaining value; rises
(resets, top-ups) are not counted. Series with the same unit share a chart;
different units get separate charts (never two y-scales). A line breaks when
two points are more than 2 h apart (PC off). Light and dark mode, hover
tooltips, a "Show as table" view for week and month.

### 4.9 Example: API balance (Soniox / DeepSeek)

```ini
[service]
name = DeepSeek
interval = 30
site = https://platform.deepseek.com/usage

[step.balance]
url = https://api.deepseek.com/user/balance
header.Authorization = Bearer {{cred:UsageMonitor/DeepSeek}}
get.balance = json:balance_infos[currency=USD].total_balance

[indicator.balance]
type = value
value = {{balance}}
prefix = $
decimals = 2
warn_below = 2
alert_below = 0.5
tray = 1
```

Soniox: `GET https://api.soniox.com/v1/organization/balance` returns
`available_balance_usd`. It needs an **organization** API key (`snx_org_...`);
a project key does not work. Keys come from Bitwarden (folder "AI Providers")
and are copied once to Credential Manager, without showing them:

```
bwprx-client run --query Soniox-organization --env SECRET=password -- powershell -ExecutionPolicy Bypass -File tools\set-secret.ps1 -Target UsageMonitor/Soniox -User api-key -FromEnv SECRET
```

The app does not call Bitwarden at runtime: every Bitwarden read needs a
user approval, which does not fit a 30-minute timer.

---

## 5. Build phases

Each phase ends with a build, a run, and a memory check
(`tools\memcheck.ps1`).

### Phase 0 — Setup
- Install WinLibs (see 1). Check `g++` and `windres`.
- Create the folders, `.gitignore` (`build/`, `*.exe`, `*.o`, `*.log`,
  `last-values.ini`, `.claude/settings.local.json`), `.gitattributes`
  (`reference-docs/ export-ignore`, `*.md export-ignore`),
  `reference-docs/.gitkeep`.
- `git init` only if the user wants it.

### Phase 1 — Skeleton tray app
- `wWinMain`, single-instance mutex + `MessageBoxW`.
- Hidden top-level main window, `WM_CLOSE` → exit.
- Tray icon, left-click, menu (Open / Settings / — / Exit), `TaskbarCreated`.
- Settings opens `services\`.
- `res/app.rc` with icon + manifest. `build.ps1`.
- **Check:** idle memory < 2 MB private. Second instance shows the error.
  `stop-app.ps1` prints `closed gracefully`.

### Phase 2 — Table window
- ListView window with fake data (3 services, 1–3 indicators).
- Centering, destroy on close, `Esc`, `F5`, double-click, DPI, custom-draw colors.
- **Check:** window rect centered on the work area (EnumWindows +
  GetWindowRect). Memory goes back down after the window is closed.

### Phase 3 — Config
- INI parser, model structs, validation, error messages with line numbers.
- Load `services\*.conf`, icons, ordering. Folder watch + reload.
- Table shows real service rows (values empty, Status = `Waiting`).
- Unit tests: parser, section order, comments, BOM, bad lines.

### Phase 4 — Fetch engine
- Worker thread + scheduler (intervals, refresh, stop, folder change).
- `http.cpp`: WinHTTP session, proxy, timeout, redirects, cookies, headers,
  body cap, cancel on exit.
- `file:` and `exec:` step types.
- Extractors (json path with `[i]` and `[key=value]`, regex, between, header,
  cookie, body, status), variables, `{{env:}}`, `{{cred:}}`, `{{file:}}`, `|url`.
- `require.*` checks.
- Unit tests with `tests/fixtures` (JSON and HTML files read through `file:`).
- Integration test: `python -m http.server` in `tests/fixtures` for plain GETs.

### Phase 5 — Indicators and polish
- Number parsing (Persian digits), units, bytes auto-scale, decimals.
- ratio / value / percent text, warn/alert/stale colors.
- Tray tooltip. `last-values.ini` (show old values at start-up as stale).
- Log file (256 KB max, 1 backup). Sleep/resume refresh. Retry once.

### Phase 6 — Real services
- `claude.conf` (check real API fields first).
- `home-ftth.conf` with the real ISP portal (needs info from the user — see 7).
- Other services the user names.
- Optional: alert dot on the tray icon.

### Phase 7 — Install and final checks
- `build.ps1 -Install` copies the exe (and `services\` only if missing — never
  overwrite the user's `.conf` files) to `E:\appServices\usage-monitor\`
  (confirm the path with the user).
- Auto start at login: only if the user asks, through DelayedTaskRunner (see
  the `autostart-app` skill), not Run keys or the Startup folder.
- Run `memcheck.ps1` for 1 hour with all services; compare with the targets in
  `project-description.md`.

---

## 6. Testing checklist (before "done")

- [ ] Second instance shows "Usage Monitor is already running." and exits with code 1
- [ ] Table window opens centered on the monitor under the mouse
- [ ] Tray right-click menu: Open (bold) / Settings / separator / Exit
- [ ] Left-click opens the window; Settings opens `services\` in Explorer
- [ ] Exit removes the tray icon and the process ends within 3 s, also during a slow fetch
- [ ] `stop-app.ps1` prints `closed gracefully` (not `killed`)
- [ ] Old exe stopped, new build compiled, new version started
- [ ] Editing a `.conf` file updates the table without restart
- [ ] A broken `.conf` shows `Config error` in its row, other services still work
- [ ] Network off → old values grey, error in Status; network on → values back
- [ ] Explorer restart → tray icon comes back
- [ ] Memory targets met (idle, window open, during fetch)

Update cycle for every rebuild (desktop rule):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "$HOME\.claude\skills\desktop-app-rules\stop-app.ps1" -Name usage-monitor
.\build.ps1
Start-Process .\build\usage-monitor.exe
```

---

## 7. Open questions for the user

1. **Home FTTH**: which ISP / portal URL? Does the login page have a captcha?
   Are the numbers in the HTML, or loaded by JavaScript (then we need the API
   URL the page calls, from the browser DevTools Network tab)?
2. **Other services**: which ones (SIM balance, VPS, other APIs)? Any of them
   with a public API is the easy case.
3. **Claude**: keep the country gate (Finland / Germany) like `claude-usage`?
   Is the token in `%USERPROFILE%\.claude\.credentials.json` good enough
   (it exists on this PC), or should it come from `token_url` like the server
   setup of `claude-usage`?
4. **Install path**: `E:\appServices\usage-monitor\`? Auto start at login?
5. **Icons**: will you provide `.ico` files, or should the build make simple
   ones?

---

## 8. Risks

| Risk | Plan |
|---|---|
| Portal needs JavaScript or captcha | Use the API the page calls, or an `exec:` step with a script. |
| `std::regex` slow / deep recursion on big HTML | 2 MB body cap, 4 MB worker stack, prefer `between:` for big pages. |
| Portal blocks frequent logins | Default interval 60 min for login-based services. |
| Claude OAuth token expires | Claude Code refreshes the file when it runs. On HTTP 401 → Status `Token expired?`. App never writes the file. |
| Calling Anthropic from a blocked country | `require.country` gate in `claude.conf`. |
| WinHTTP + SChannel DLLs add working set | Counted in the targets; they are shared Windows DLLs. Trim working set after each round. |
