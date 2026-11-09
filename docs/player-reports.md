# Player reports (2026-11-09)

How a player tells us about a problem, and what the owner sets up once for it.

## What the player has

- `spikeout.log` in the data folder (next to settings.ini): everything the window prints, each line with its time;
  the run before kept as `spikeout-prev.log`; 20 MB at most. Its first lines: the version, the system, the CPU, the
  displays and every controller SDL sees (name, maker and product IDs, GUID, whether SDL can read it as a gamepad,
  its mapping), then each one as it comes and goes, and when the game takes it into its list of pads.
- A crash writes `crash-<time>.txt` (what, where as module+offset, the stack, the log's last 400 lines; Windows
  also `crash-<time>.dmp`, a minidump); the next start says so at the bottom.
- Esc > Game > **Report a problem** (the owner's design: GitHub for players logged in, a zip for the others):
  - **What happened**: free text (any language through the system's input; Steam Deck: its on-screen keyboard; the
    menu's font shows Latin letters only, other scripts are sent fine). Shift+Enter: a new line; Ctrl+V: paste.
  - **Detailed logging**: the network and timing lines (`SPIKEOUT_NET_DEBUG`) in the log too, from the next start.
  - **Log in to GitHub** (not logged in): GitHub's device flow, a code typed on github.com/login/device; the game
    never sees the password. Back to the report by itself once logged in.
  - **Send to GitHub**: chosen only logged in and with something written (else shown greyed: "log in first",
    "write what happened first"). A public issue on tastyrepublic/spikeout-recomp as the player: their words, the
    version, the computer and its controllers, the newest crash's first lines; the logs, the crash report and the
    settings as comments (each its last 60,000 characters). Internet addresses and the user name (in folder paths)
    are taken out; an `@` the player typed mentions nobody. A send that fails says why at the bottom.
  - **Save as a zip**: the same files, the words (what-happened.txt) and the minidump, in the data folder; a page of
    the game's own says its name and folder, **Copy its path**, and on a computer **Open its folder** (no window of
    the system's: the same on every device; a phone's or tablet's share sheet can come there later).
  - **Last report** (only once one was sent): its number; Enter opens its page.
  - **Log out of GitHub** (logged in): the login (github-login.txt in the data folder) forgotten.

Code: tools/player_report.hpp (log, crash reports, zip, the issue's text), src/net/report.{hpp,cpp} (logging in,
sending), src/ui/menu.cpp (the text page, Menu::replace), tools/spikeout.cpp (the pages).

## What the owner sets up once: a GitHub App (only for the login)

1. GitHub > Settings > Developer settings > GitHub Apps > **New GitHub App** (https://github.com/settings/apps/new):
   - Name: e.g. `Spikeout reports`. Homepage URL: https://github.com/tastyrepublic/spikeout-recomp
   - Callback URL: empty. **Expire user authorization tokens: off** (a player's login stays). **Enable Device
     Flow: on**. Webhook: **Active off**.
   - Permissions > Repository permissions > **Issues: Read and write** (nothing else).
   - Where can this GitHub App be installed: **Only on this account**. Create.
2. Copy the **Client ID** (Iv23...: public, not a secret). No private key is needed.
3. Install App (left menu) > your account > **Only select repositories: spikeout-recomp** (a player's login can
   open issues only where the app is installed).
4. Repository Settings > Secrets and variables > Actions > Variables > **REPORTS_APP_CLIENT_ID** = the Client ID
   (the release workflow builds it in; locally: `cmake -DSPIKEOUT_GITHUB_CLIENT_ID=...`). Without it the game offers
   no login: a zip only.

The lobby is not involved.

## Tests (no real GitHub)

- The window against a fake GitHub (2026-11-09): `node fake-github.mjs` (port 8791: users, issues, comments, the
  device flow, approving by itself on its second ask), then the window with `SPIKEOUT_GITHUB_CLIENT_ID=test-id
  SPIKEOUT_GITHUB_WEB=http://127.0.0.1:8791 SPIKEOUT_GITHUB_API=http://127.0.0.1:8791`: logged in by the device flow,
  the issue and its comments sent with the player's token; Save as a zip: the zip and the game's own page.
  Env switches: SPIKEOUT_LOG=0 (no log file), SPIKEOUT_REPORT_REPO.
- Measured (Windows, 3,600 frames each, twice): no log / log / log + detailed logging: CPU 25-27 / 24 / 24-25 s, every
  second at full speed with detailed logging, every recording 60 of 60 hashes in gamerun.
