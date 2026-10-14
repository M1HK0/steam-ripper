# Steam Ripper

A simple desktop tool to bulk-clean your own Steam accounts: remove **all friends**,
**leave all groups**, **decline pending invites**, **unblock everyone**, and **leave
your Steam Family** — one account at a time.

> ⚠️ Use this only on accounts you own or are authorized to manage.

## Features

- **Three login methods**
  - Username + password (handles Steam Guard email / mobile codes)
  - `steamLoginSecure` web cookie
  - `steamid----token` refresh token (logs in via Steam's CM protocol, like NFA-style tools)
- **Five cleanup targets**: Friends, Groups, Pending invites, Blocked, Steam Family
- Progress bar + live log, runs on a background thread so the UI never freezes
- Randomised delay between requests + automatic back-off/retry on Steam rate limits (HTTP 429)

## Install & run

```bash
pip install -r requirements.txt
python gui.py
```

Or use the CLI (friends only):

```bash
python remove_friends.py --cookie "<steamLoginSecure value>" --delay 2
```

## Build a standalone .exe

```bash
pip install pyinstaller
pyinstaller --noconfirm --onefile --windowed --name "SteamFriendCleaner" ^
  --collect-all steam --collect-submodules Cryptodome --hidden-import requests gui.py
```

The exe lands in `dist/`.

## How to get each credential

- **steamLoginSecure cookie**: log into steamcommunity.com in a browser on this PC,
  press F12 → Application/Storage → Cookies → `https://steamcommunity.com`, copy the
  value of the cookie named `steamLoginSecure`.
- **Refresh token** (`steamid----token`): from an NFA-style token tool. Must be a
  *fresh, unused* token — refresh tokens get rotated once an account logs in, after
  which Steam returns `AccessDenied`.

## Notes & gotchas

- **Steam Family has a 1-year cooldown** before you can join/create another family.
  Leaving is irreversible; child accounts can't self-remove.
- Pending invites / blocked / family endpoints are best-effort and may change as Steam
  updates its site.
- Keep the delay around 2s to avoid rate limits on large accounts.

## Files

| File | What it is |
|------|-----------|
| `gui.py` | Tkinter GUI (main app) |
| `remove_friends.py` | Core logic: login, token exchange, all cleanup actions |
| `requirements.txt` | Python dependencies |
| `src/`, `CMakeLists.txt`, `third_party/` | Earlier C++/ImGui prototype |

## Security

Never share or commit your `steamLoginSecure` cookie or refresh tokens — they grant
full access to the account. `cookie.txt` is gitignored for this reason. Delete it when
you're done.
