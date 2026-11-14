# Usage Monitor

A very small Windows tray app (C++ / plain Win32, about 1 MB, a few MB of RAM)
that checks websites and APIs on a timer and shows how much quota or balance is
left — for example Claude usage limits, the USD balance of Soniox,
DeepSeek and OpenRouter API accounts, and the IRT balance (and SMS left) of the MsgWay and
MihanSMS accounts.

- One table window, one row per service. Services are `.conf` files (INI) in
  `services\` next to the exe; no settings window.
- Secrets are read from Windows Credential Manager (`{{cred:NAME}}`), never
  written in the `.conf` files.
- Usage history: one log file per day in `history\` (last 30 days, older files
  are deleted). The report button of a row opens a chart page (today / this
  week / this month) in the browser. The page is made by a separate,
  short-lived process, so the tray app stays small.

## Build

Needs MinGW-w64 GCC (WinLibs): `winget install BrechtSanders.WinLibs.POSIX.UCRT`

```powershell
.\build.ps1        # build\usage-monitor.exe (+ copies sample services if missing)
.\build.ps1 -Run   # stop the running copy, build, start the new exe
```

## Docs

- [project-description.md](project-description.md) — what the app does.
- [plan.md](plan.md) — how it is built, the `.conf` format, history and report.
