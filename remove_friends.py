#!/usr/bin/env python3
"""
Steam friend cleaner.

Paste the `steamLoginSecure` cookie from one of YOUR logged-in Steam accounts,
and this removes every friend on that account. Repeat for each account.

How to grab the cookie (do this while logged in on steamcommunity.com):
  1. Open https://steamcommunity.com in your browser (logged in).
  2. F12 -> Application/Storage -> Cookies -> https://steamcommunity.com
  3. Copy the VALUE of the cookie named `steamLoginSecure`.
     It looks like:  76561198XXXXXXXXX%7C%7Cabcdef0123...
  4. Run:  python remove_friends.py
     and paste it when asked (or pass it with --cookie).

Only use this on accounts you own / are allowed to manage.
"""

import argparse
import re
import sys
import time
import urllib.parse

import requests

HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36"
    ),
    "Referer": "https://steamcommunity.com/",
}

# Optional callback so a GUI can show "rate limited, waiting Ns..." messages.
status_cb = None


def _notify(msg: str):
    if status_cb:
        try:
            status_cb(msg)
        except Exception:
            pass


def _request(session, method, url, *, max_retries=5, **kwargs):
    """GET/POST wrapper that backs off and retries on HTTP 429 (rate limit)."""
    delay = 5
    for attempt in range(max_retries + 1):
        r = session.request(method, url, **kwargs)
        if r.status_code != 429:
            return r
        if attempt == max_retries:
            return r  # give up; caller will raise_for_status
        wait = int(r.headers.get("Retry-After", 0)) or delay
        _notify(f"Rate limited by Steam. Waiting {wait}s then retrying "
                f"({attempt + 1}/{max_retries})...")
        time.sleep(wait)
        delay = min(delay * 2, 60)
    return r


def normalize_cookie(raw: str) -> str:
    """Accept either the raw value or something pasted as `steamLoginSecure=...`."""
    raw = raw.strip().strip('"').strip("'")
    if raw.lower().startswith("steamloginsecure="):
        raw = raw.split("=", 1)[1]
    return raw.strip()


def _steamid_from_jwt(token: str) -> str | None:
    """Steam login tokens are JWTs whose payload `sub` is the SteamID64."""
    import base64
    import json
    parts = token.split(".")
    if len(parts) < 2:
        return None
    payload = parts[1]
    payload += "=" * (-len(payload) % 4)  # fix base64url padding
    try:
        data = json.loads(base64.urlsafe_b64decode(payload))
    except Exception:
        return None
    sub = str(data.get("sub", ""))
    return sub if re.fullmatch(r"7656\d{13}", sub) else None


def _jwt_payload(token: str) -> dict:
    import base64
    import json
    parts = token.split(".")
    if len(parts) < 2:
        return {}
    p = parts[1] + "=" * (-len(parts[1]) % 4)
    try:
        return json.loads(base64.urlsafe_b64decode(p))
    except Exception:
        return {}


def is_refresh_token(token: str) -> bool:
    """Refresh tokens carry 'renew'/'derive' audiences; web cookies are web-only."""
    aud = _jwt_payload(token).get("aud", [])
    return "renew" in aud or "derive" in aud


def parse_token(raw: str) -> tuple[str, str]:
    """
    Accept any of:
      - full cookie:        76561198...%7C%7CeyJ...   or  76561198...||eyJ...
      - nfa-style line:     76561198...----eyJ...
      - bare token (JWT):   eyJ...
    Returns (steamid64, token).
    """
    decoded = urllib.parse.unquote(raw).strip()

    # Split out an explicit steamid prefix if one is present.
    sid = ""
    token = decoded
    for sep in ("||", "----", "%7C%7C"):
        if sep in decoded:
            left, token = decoded.split(sep, 1)
            left = left.strip()
            if re.fullmatch(r"7656\d{13}", left):
                sid = left
            break

    token = token.strip()
    # If no usable steamid prefix, pull it out of the JWT itself.
    if not sid:
        sid = _steamid_from_jwt(token) or ""

    if not re.fullmatch(r"7656\d{13}", sid):
        raise ValueError(
            "Could not read a SteamID64 from that value. Paste either the full "
            "`steamLoginSecure` cookie, a `steamid----token` line, or the login "
            "token (it must be a real Steam token starting with 'eyJ')."
        )
    if not token:
        raise ValueError("No token found after the SteamID.")
    return sid, token


def steamid_from_cookie(cookie_value: str) -> str:
    """Back-compat: return just the SteamID64 for any supported input format."""
    return parse_token(cookie_value)[0]


def _machine_id(ident: str) -> bytes:
    import hashlib
    import io
    b = io.BytesIO()
    b.write(b"\x00"); b.write(b"MessageObject\x00")
    for key in ("BB3", "FF2", "3B3"):
        h = hashlib.sha1(f"SteamUser Hash {key} {ident}".encode()).hexdigest()
        b.write(b"\x01"); b.write(key.encode() + b"\x00"); b.write(h.encode() + b"\x00")
    b.write(b"\x08\x08")
    return b.getvalue()


def web_token_from_refresh(steamid: str, refresh_token: str) -> str:
    """
    Log into Steam's CM with a refresh token and exchange it for a short-lived
    web access token (the value that goes in steamLoginSecure). This is the only
    way a 'steamid----token' refresh token can become a web session.
    """
    from steam.client import SteamClient
    from steam.core.msg import MsgProto
    from steam.enums.emsg import EMsg
    from steam.enums import EResult, EOSType
    from steam.utils import ip4_to_int  # noqa: F401 (kept for parity)

    client = SteamClient()
    try:
        _notify("Connecting to Steam...")
        if not client.connect(retry=3):
            raise RuntimeError("Could not connect to a Steam CM server.")
        client._pre_login()

        m = MsgProto(EMsg.ClientLogon)
        m.header.steamid = int(steamid)
        m.body.protocol_version = 65580
        m.body.client_os_type = EOSType.Windows10
        m.body.client_language = "english"
        m.body.should_remember_password = True
        m.body.supports_rate_limit_response = True
        m.body.chat_mode = 2
        m.body.machine_name = "unsub-all"
        m.body.obfuscated_private_ip.v4 = 0xA1A2A3A4
        m.body.machine_id = _machine_id(str(steamid))
        m.body.access_token = refresh_token

        _notify("Logging in with token...")
        client.send(m)
        resp = client.wait_msg(EMsg.ClientLogOnResponse, timeout=25)
        if not resp:
            raise RuntimeError("No logon response from Steam (timed out).")
        er = EResult(resp.body.eresult)
        if er != EResult.OK:
            if er == EResult.AccessDenied:
                raise RuntimeError(
                    "Steam rejected the token (AccessDenied). Refresh tokens get "
                    "used up / rotated once an account logs in. Generate a FRESH "
                    "token and paste that, or use the steamLoginSecure cookie."
                )
            raise RuntimeError(f"Token login failed: {er.name}.")

        _notify("Exchanging for web session...")
        r = client.send_um_and_wait(
            "Authentication.GenerateAccessTokenForApp#1",
            {"refresh_token": refresh_token, "steamid": int(steamid)},
            timeout=25,
        )
        if not r or not r.body.access_token:
            raise RuntimeError("Steam did not return a web access token.")
        return r.body.access_token
    finally:
        try:
            client.disconnect()
        except Exception:
            pass


def make_session(cookie_value: str) -> requests.Session:
    s = requests.Session()
    s.headers.update(HEADERS)
    steamid, token = parse_token(cookie_value)
    if is_refresh_token(token):
        # steamid----token refresh token: trade it for a web token over CM.
        token = web_token_from_refresh(steamid, token)
    # Steam expects steamLoginSecure = "<steamid>||<token>", URL-encoded.
    full = urllib.parse.quote(steamid, safe="") + "%7C%7C" + token
    # We supply our own sessionid; Steam just requires the POST param to match the cookie.
    sessionid = "a1b2c3d4e5f6a7b8"
    s.cookies.set("steamLoginSecure", full, domain="steamcommunity.com", path="/")
    s.cookies.set("sessionid", sessionid, domain="steamcommunity.com", path="/")
    s._sessionid = sessionid  # stash for later
    return s


class SteamGuardNeeded(Exception):
    """Raised when login needs a Steam Guard code. `kind` is 'email' or 'mobile'."""

    def __init__(self, kind: str):
        self.kind = kind
        super().__init__(kind)


def login_with_password(username: str, password: str,
                        guard_code: str = "") -> tuple[requests.Session, str]:
    """
    Log in with username + password (+ Steam Guard code if required).

    Returns (session, steamid64). Raises SteamGuardNeeded('email'|'mobile')
    if a code is required, so the caller can prompt and call again with it.
    """
    from steam.webauth import (WebAuth, EmailCodeRequired,
                               TwoFactorCodeRequired, LoginIncorrect,
                               CaptchaRequired)

    wa = WebAuth(username)
    try:
        wa.login(password,
                 email_code=guard_code if guard_code else "",
                 twofactor_code=guard_code if guard_code else "")
    except EmailCodeRequired:
        raise SteamGuardNeeded("email")
    except TwoFactorCodeRequired:
        raise SteamGuardNeeded("mobile")
    except CaptchaRequired:
        raise RuntimeError(
            "Steam is asking for a CAPTCHA (too many attempts). "
            "Wait a while, or log in via the cookie method instead."
        )
    except LoginIncorrect:
        raise RuntimeError("Wrong username or password.")

    session = wa.session
    session.headers.update(HEADERS)
    session._sessionid = wa.session_id
    return session, str(wa.steam_id.as_64)


def get_friend_ids(session: requests.Session, steamid: str) -> list[str]:
    url = f"https://steamcommunity.com/profiles/{steamid}/friends/"
    r = _request(session, "GET", url, timeout=30)
    r.raise_for_status()
    if "g_steamID = false" in r.text or "login" in r.url:
        raise RuntimeError(
            "Steam did not accept the cookie (not logged in). "
            "The cookie may be expired — grab a fresh one."
        )
    # Friend entries carry data-steamid="76561198..."
    ids = re.findall(r'data-steamid="(7656\d{13})"', r.text)
    # de-dupe while preserving order
    seen, out = set(), []
    for i in ids:
        if i not in seen and i != steamid:
            seen.add(i)
            out.append(i)
    return out


def remove_friend(session: requests.Session, friend_id: str) -> bool:
    r = _request(session, "POST",
        "https://steamcommunity.com/actions/RemoveFriendAjax",
        data={"sessionid": session._sessionid, "steamid": friend_id},
        timeout=30,
    )
    return r.status_code == 200 and r.text.strip() in ("true", "1", "")


def get_pending_invites(session: requests.Session, steamid: str) -> list[str]:
    """Incoming friend invites waiting on this account."""
    r = _request(session, "GET", "https://steamcommunity.com/my/friends/pending",
                 timeout=30, allow_redirects=True)
    r.raise_for_status()
    ids = re.findall(r'data-steamid="(7656\d{13})"', r.text)
    seen, out = set(), []
    for i in ids:
        if i not in seen and i != steamid:
            seen.add(i)
            out.append(i)
    return out


def decline_invite(session: requests.Session, inviter_id: str) -> bool:
    r = _request(session, "POST",
        "https://steamcommunity.com/actions/IgnoreFriendInviteAjax",
        data={"sessionID": session._sessionid, "steamid": inviter_id,
              "perform": "ignore"},
        timeout=30,
    )
    return r.status_code == 200


def get_blocked_ids(session: requests.Session, steamid: str) -> list[str]:
    url = f"https://steamcommunity.com/profiles/{steamid}/friends/blocked"
    r = _request(session, "GET", url, timeout=30)
    r.raise_for_status()
    ids = re.findall(r'data-steamid="(7656\d{13})"', r.text)
    seen, out = set(), []
    for i in ids:
        if i not in seen and i != steamid:
            seen.add(i)
            out.append(i)
    return out


def unblock_user(session: requests.Session, blocked_id: str) -> bool:
    r = _request(session, "POST",
        "https://steamcommunity.com/actions/BlockUserAjax",
        data={"sessionID": session._sessionid, "steamid": blocked_id, "block": "0"},
        timeout=30,
    )
    return r.status_code == 200


def get_webapi_token(session: requests.Session) -> str:
    """Pull a WebAPI access token from the logged-in community session."""
    r = _request(session, "GET",
                 "https://steamcommunity.com/chat/clientjstoken", timeout=30)
    r.raise_for_status()
    token = r.json().get("token", "")
    if not token:
        raise RuntimeError("Could not get a WebAPI token (session not logged in?).")
    return token


def get_family_groupid(session: requests.Session, steamid: str,
                       token: str) -> str | None:
    """Return the family_groupid this account belongs to, or None."""
    r = _request(session, "GET",
        "https://api.steampowered.com/IFamilyGroupsService/GetFamilyGroupForUser/v1/",
        params={"access_token": token, "steamid": steamid},
        timeout=30,
    )
    r.raise_for_status()
    resp = r.json().get("response", {})
    gid = str(resp.get("family_groupid", "0"))
    if gid and gid != "0":
        return gid
    return None


def leave_family(session: requests.Session, steamid: str, family_groupid: str,
                 token: str) -> bool:
    """Leave a family by removing yourself from it."""
    r = _request(session, "POST",
        "https://api.steampowered.com/IFamilyGroupsService/RemoveFromFamilyGroup/v1/",
        params={"access_token": token},
        data={"family_groupid": family_groupid, "steamid_to_remove": steamid},
        timeout=30,
    )
    return r.status_code == 200


def get_group_ids(session: requests.Session, steamid: str) -> list[str]:
    """Steam exposes a group list as XML with <groupID64> entries."""
    url = f"https://steamcommunity.com/profiles/{steamid}/groups/?xml=1"
    r = _request(session, "GET", url, timeout=30)
    r.raise_for_status()
    ids = re.findall(r"<groupID64>(\d+)</groupID64>", r.text)
    seen, out = set(), []
    for i in ids:
        if i not in seen:
            seen.add(i)
            out.append(i)
    return out


def leave_group(session: requests.Session, steamid: str, group_id: str) -> bool:
    """Leave a group via the profile's home_process action endpoint."""
    r = _request(session, "POST",
        f"https://steamcommunity.com/profiles/{steamid}/home_process",
        data={
            "sessionID": session._sessionid,
            "action": "leaveGroup",
            "groupId": group_id,
        },
        timeout=30,
    )
    return r.status_code == 200


def main() -> int:
    ap = argparse.ArgumentParser(description="Remove all Steam friends for one account.")
    ap.add_argument("--cookie", help="steamLoginSecure cookie value")
    ap.add_argument("--delay", type=float, default=1.0,
                    help="seconds between removals (default 1.0, be gentle)")
    ap.add_argument("--yes", action="store_true", help="skip the confirmation prompt")
    args = ap.parse_args()

    cookie = args.cookie or input("Paste steamLoginSecure cookie value: ")
    cookie = normalize_cookie(cookie)

    try:
        steamid = steamid_from_cookie(cookie)
    except ValueError as e:
        print(f"[!] {e}")
        return 1

    session = make_session(cookie)

    print(f"[*] Account SteamID64: {steamid}")
    print("[*] Fetching friends list...")
    try:
        friends = get_friend_ids(session, steamid)
    except Exception as e:
        print(f"[!] {e}")
        return 1

    if not friends:
        print("[*] No friends found (or list is private). Nothing to do.")
        return 0

    print(f"[*] Found {len(friends)} friend(s).")
    if not args.yes:
        ans = input(f"Remove ALL {len(friends)} friends from {steamid}? [y/N]: ")
        if ans.strip().lower() not in ("y", "yes"):
            print("Aborted.")
            return 0

    ok = fail = 0
    for n, fid in enumerate(friends, 1):
        try:
            if remove_friend(session, fid):
                ok += 1
                print(f"  [{n}/{len(friends)}] removed {fid}")
            else:
                fail += 1
                print(f"  [{n}/{len(friends)}] FAILED {fid}")
        except Exception as e:
            fail += 1
            print(f"  [{n}/{len(friends)}] error on {fid}: {e}")
        time.sleep(args.delay)

    print(f"\n[*] Done. Removed {ok}, failed {fail}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
