# Usage Monitor — project description

## What it is

Usage Monitor is a very small Windows system-tray app. It checks websites and
APIs on a timer and shows how much quota or balance is left on many services,
for example:

- the home FTTH internet account (data left, days left),
- Claude usage (5-hour and weekly limits),
- API money balances (Soniox, DeepSeek — shown in USD),
- a prepaid SIM or any account with a money balance.

All results are shown in **one table window**: one row per service.

## Main goal: very low memory

The laptop that runs this app has very little RAM. So low memory use is the
most important goal, more important than nice looks or fast development.

Targets (measured with Task Manager / `Get-Process`):

| State | Private memory | Working set |
|---|---|---|
| Idle, window closed | under 3 MB | under 8 MB |
| Window open | under 5 MB | under 12 MB |
| During a fetch (short peak) | under 8 MB | under 16 MB |

To reach this, the app is written in **C++ with the plain Win32 API** — no
framework, no .NET, no browser engine, no Go/Python runtime. See `plan.md` for
the stack comparison.

## User interface

### Tray icon

- **Left-click** = toggle: opens the table window; when the window is already
  open, closes it to the tray (a minimized window is restored instead).
- **Right-click** = a menu with only 3 items:
  1. **Open** (default item, shown in bold)
  2. **Settings** — opens the folder with the `.conf` files in Explorer
  3. *(separator)*
  4. **Exit**
- Tooltip on hover: a short summary (only indicators marked `tray = 1`).

### Tray app behavior (answers to the desktop-app-rules questions)

| Question | Answer |
|---|---|
| Taskbar button while the window is open? | **No.** The window is owned by the hidden main window; no minimize button. |
| Window position | **Remember** the last position and monitor; size fits the data on each open; centered if that monitor is gone. |
| Where is it saved? | `usage-monitor.ini` next to the exe (`[window]`), portable. |
| Tray left-click | Open the table window. |
| Left-click while open | Closes it to the tray (toggle). |

### The table window (the only window)

```
+- Usage Monitor -----------------------------------------------------------------+
| Service        | Remained                                   | Updated |    |    |
|----------------+--------------------------------------------+---------+----+----|
| (i) Claude     | (o) 91% 5h (13:30) | (o) 8% Week (Tue 4:00) | 10:13   | [R]| [H]|
| (i) Home FTTH  | (o) 25 GB / 500 GB Data | 12 days          | 11/13 9:05 | [R]| [H]|
| (i) DeepSeek   | $3.89 left                                  | Error   | [R]| [H]|
+---------------------------------------------------------------------------------+
  (o) = small pie of the remaining part, [R] = refresh button, [H] = usage report
```

- One row per service. A service can have 1 or more **indicators**.
- The number of indicator columns = the largest indicator count of any service.
- The service icon (optional `.ico`) is shown in the first column.
- **Rule: the indicator columns (title "Remained") always show what is LEFT /
  available, never what is used.** A service whose API gives "used" must convert
  it (`invert = 1` for percent, `used` + `total` for ratio). This applies to every
  future service.
- Short cell text: `<value> <label> (<reset time>)`, e.g. `64% Fable (Tue 4:00)`.
  Keep labels short (`5h`, `Week`, `Data`).
- **Updated** column: time of the last successful update, green when the last
  run was OK, red when it failed (the error is in the tooltip). Format without
  year and without leading zeros except minutes: `10:13` today, `11/13 9:05`
  on another day.
- Time zones: each service has its own `timezone` (Claude: `Europe/Helsinki`,
  used to read API times). The app has one display zone for all times:
  `usage-monitor.ini` → `[app] timezone=Asia/Tehran`. Without it, reset times
  are shown in the service zone and Updated in the PC zone.
- Colors: normal text; orange when low (`warn_below`); red when very low
  (`alert_below`); grey when the value is old (last fetch failed).
- The window opens at its last position (see the table above), sized to fit its
  data each time it opens, and is fully destroyed when closed, so it uses no memory while closed.
- A small pie before each indicator shows the remaining part (same color as the
  text). Indicators of type `value` show a pie only when `max` is set.
- The last column has an icon-only **refresh** button per row (blue tone,
  tooltip "Refresh <name> now"); the icon spins while that row updates.
- Next to it, an icon-only **report** button (purple tone, column-chart icon,
  tooltip "Usage report of <name>") opens the usage history of that service in
  the default browser (see "Usage history and report" below).
- Money balances (Soniox, DeepSeek) are `value` indicators with `prefix = $` and
  no `max`, so they show only the amount (`$3.89 left`), no pie and no percent.
- Small extras: `F5` = refresh all, `Esc` = close, double-click a row = open the
  service website (if `site` is set). Rows can not be selected (read-only table).

### Indicator types

| Type | Shows | Example |
|---|---|---|
| `ratio` | remaining / total | `25 GB / 500 GB` |
| `value` | one number (balance, days, count) | `$24.56`, `12 days` |
| `percent` | a percent | `25%` |

## Services are defined in `.conf` files

- Folder: `services\` next to the exe. **Settings** opens this folder.
- One service = one `.conf` file (INI style, UTF-8).
- A service has: name, optional icon, refresh interval, a list of **steps**
  (HTTP requests, local file reads, or an external command), and a list of
  **indicators**.
- Steps can log in (cookies are kept between steps), read values from JSON,
  from HTML (regex / text-between), from headers or cookies, and save them in
  variables. Indicators use these variables.
- Secrets do not need to be written in the file: use `{{env:NAME}}`, Windows
  Credential Manager (`{{cred:NAME}}`) or a local file.
- The app sees changes in the folder and reloads the files by itself.

Full format: see `plan.md`, section "Config file format".

Services now: `claude.conf` (web session via keep-sessions), `soniox.conf` and
`deepseek.conf` (API keys, every 30 min, no browser extension needed).

## Usage history and report

- After each successful update, the app adds one line per indicator to a
  **daily log file**: `history\YYYY-MM-DD.tsv` next to the exe (the date is in
  the display time zone). A line is tiny, so a day is a few KB.
- Files older than **30 days** are deleted automatically (once a day, and each
  time a report is made). The number of days is `[app] history_days` in
  `usage-monitor.ini`.
- The **report button** of a row starts a **separate, short-lived process**
  (`usage-monitor.exe --report <service>.conf`). It reads only the last 30 days
  of log files for that service, writes `reports\<service>.html`, opens it in
  the default browser and exits. So the tray app never keeps the history or the
  charts in memory.
- The report page has three parts, top to bottom:
  1. **Today** — the remaining value now, how much was used today, and a line
     chart of the remaining value by hour.
  2. **This week** — line chart of the week, a bar per day (how much was used
     that day), and one button per day of the week: click a day (or its bar) to
     see that day by hour. The week starts on `[app] week_start` (default `sat`).
  3. **This month** — line chart of the calendar month and a bar per day.
- "Used" = the sum of the drops of the remaining value. A rise (a 5-hour reset,
  a top-up) is not counted as use.
- The page is one self-contained HTML file (inline SVG charts, no internet
  needed), with light and dark mode and a hover tooltip on every chart.

## What the app does NOT do

- No browser engine. Websites that need JavaScript to show the numbers, or a
  captcha at login, can not be read directly. For them, a step can run an
  external command (for example a PowerShell script) and read its output.
- No notifications. No charts inside the app window (the report opens in the
  browser, in a separate process).
- No settings window — settings are the `.conf` files.

## Platform and files

- Windows 10 / 11, 64-bit. Portable: one `.exe` plus the `services\` folder.
- Files next to the exe: `usage-monitor.exe`, `services\*.conf`, `services\*.ico`,
  `usage-monitor.log` (small, rotated), `usage-monitor.ini` (settings and window
  position), `history\*.tsv` (one usage log per day, last 30 days),
  `reports\*.html` (the last report of each service, overwritten each time).
  Planned: `last-values.ini` (last results, shown at start-up until the first
  fetch ends).

## Desktop app rules this app follows

- Only one instance: a second copy shows "Usage Monitor is already running."
  and exits.
- Every window opens centered on the monitor under the mouse.
- The tray menu has `Exit` as the last item, after a separator.
- The app closes cleanly on `WM_CLOSE` (it has a hidden top-level window).
