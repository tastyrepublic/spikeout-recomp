# Player reports (2026-11-09)

How a player tells us about a problem, and what the owner sets up once for it.

## What the player has

- `spikeout.log` in the data folder (next to settings.ini): everything the window prints, each line with its time;
  the run before kept as `spikeout-prev.log`; 20 MB at most. Its first lines: the version, the system, the CPU, the
  displays and every controller SDL sees (name, maker and product IDs, GUID, whether SDL can read it as a gamepad,
  its mapping), then each one as it comes and goes, and when the game takes it into its list of pads.
- A crash writes `crash-<time>.txt` (what, where as module+offset, the stack, the log's last 400 lines; Windows
  also `crash-<time>.dmp`, a minidump); the next start says so at the bottom.
- Esc > Game > Detailed logging: the network and timing lines too (`SPIKEOUT_NET_DEBUG`), from the next start.
- Esc > Game > Report a problem:
  - **What happened**: free text (any language through the system's input; Steam Deck: its on-screen keyboard; the
    menu's font shows Latin letters only, other scripts are sent fine).
  - **GitHub**: log in (GitHub's device flow: a code typed on github.com/login/device; the game never sees the
    password). Logged in, the report is the player's own issue: they get the answers and can add to it.
  - **Your GitHub name** (not logged in, optional): mentioned in the issue, so they're told of the answers. The
    lobby checks the name exists.
  - **Send the report**: a public issue on tastyrepublic/spikeout-recomp, label `player report`: the player's words,
    the version, the computer and its controllers, the newest crash's first lines; the logs, the crash report and the
    settings as comments (each its last 60,000 characters). Internet addresses and the user name (in folder paths)
    are taken out; an `@` the player typed mentions nobody.
  - **Save as a zip instead**: the same files and the minidump in one zip, and GitHub's bug form opened
    (.github/ISSUE_TEMPLATE/bug_report.yml) to attach it by hand.

Logged in: the game sends to GitHub itself. Not logged in: to the lobby (`/v1/report`, lobby/src/index.js), which
opens the issue as the project's GitHub App (shown as its bot) and keeps nothing; 5 reports an hour from one
address, 300 a day in all, 1 MB at most, only the game's format.

Code: tools/player_report.hpp (log, crash reports, zip, the issue's text), src/net/report.{hpp,cpp} (sending,
logging in), src/ui/menu.cpp (the text page), lobby/src/index.js (`report`, `appToken`, `Reports`).

## What the owner sets up once

One GitHub App does both: the players' login, and the lobby's issues.

1. GitHub > Settings > Developer settings > GitHub Apps > **New GitHub App**:
   - Name: e.g. `Spikeout reports`. Homepage URL: https://github.com/tastyrepublic/spikeout-recomp
   - Callback URL: leave empty. **Enable Device Flow**: on.
   - Webhook: **Active off**.
   - Permissions > Repository permissions > **Issues: Read and write** (nothing else; Metadata read-only comes with it).
   - Where can this GitHub App be installed: **Only on this account**. Create.
2. On the app's page (General):
   - Optional features: **User-to-server token expiration: Opt-out** (a player's login then stays; they can log out
     in the game, or remove the app from their GitHub settings).
   - Copy the **Client ID** (Iv23...: public, not a secret).
   - Private keys > **Generate a private key**: a .pem file downloads (a secret: never into the repo or a chat).
   - Note the **App ID** (a number).
3. Install App (left menu) > your account > **Only select repositories: spikeout-recomp**.
4. The lobby's secrets, in `lobby/` (each asks for the value):
   ```
   npx wrangler secret put APP_ID            (the number)
   npx wrangler secret put APP_PRIVATE_KEY   (paste the whole .pem file's text)
   ```
   Then the lobby deployed (`npx wrangler deploy` in lobby/: owner's OK each time).
5. The builds: repository Settings > Secrets and variables > Actions > Variables > **REPORTS_APP_CLIENT_ID** = the
   Client ID (the release workflow builds it in; locally: `cmake -DSPIKEOUT_GITHUB_CLIENT_ID=...`). Without it the
   game offers no login (reports still go through the lobby).

## Tests (no real GitHub)

- `npm test` in lobby/: a fake GitHub; a report with a plain token and as the app (a real RSA key, the signature
  checked), a GitHub name checked, the format, sizes and the hourly limit.
- The window against the lobby and a fake GitHub (2026-11-09, both ways passed): `node fake-github.mjs` (port
  8791: users, issues, comments, the device flow), `npx wrangler dev --port 8790 --var GITHUB_API:http://127.0.0.1:8791
  --var REPORT_TOKEN:x`, then the window with `SPIKEOUT_REPORT_URL=http://127.0.0.1:8790/v1/report`
  (`SPIKEOUT_GITHUB_CLIENT_ID=test-id SPIKEOUT_GITHUB_WEB=http://127.0.0.1:8791 SPIKEOUT_GITHUB_API=...` for the
  login). Env switches: SPIKEOUT_LOG=0 (no log file), SPIKEOUT_REPORT_REPO.
- Measured (Windows, 3,600 frames each, twice): no log / log / log + detailed logging: CPU 25-27 / 24 / 24-25 s, every
  second at full speed with detailed logging, every recording 60 of 60 hashes in gamerun.
