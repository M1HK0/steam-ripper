#!/usr/bin/env python3
"""
Simple GUI for the Steam friend cleaner.

Run:  python gui.py
Paste a `steamLoginSecure` cookie, click "Load friends", then "Remove all".
Only use this on accounts you own / are allowed to manage.
"""

import queue
import threading
import tkinter as tk
from tkinter import messagebox, scrolledtext, simpledialog, ttk

import remove_friends as core


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("Steam Friend Cleaner")
        root.geometry("720x540")
        root.minsize(640, 480)

        self.session = None
        self.steamid = None
        self.target = "friends"
        self.items: list[str] = []
        self.family_token = ""
        self.family_gid = None
        self.log_q: queue.Queue[str] = queue.Queue()
        self.worker: threading.Thread | None = None

        pad = {"padx": 10, "pady": 6}

        # --- login method tabs ---
        self.tabs = ttk.Notebook(root)
        self.tabs.pack(fill="x", **pad)

        # Password tab
        pw_tab = tk.Frame(self.tabs)
        self.tabs.add(pw_tab, text="Username + Password")
        tk.Label(pw_tab, text="Steam username (not email):",
                 anchor="w").pack(fill="x", padx=8, pady=(8, 0))
        self.user_entry = tk.Entry(pw_tab)
        self.user_entry.pack(fill="x", padx=8)
        tk.Label(pw_tab, text="Password:", anchor="w").pack(fill="x", padx=8, pady=(6, 0))
        self.pass_entry = tk.Entry(pw_tab, show="•")
        self.pass_entry.pack(fill="x", padx=8, pady=(0, 8))
        tk.Label(pw_tab, text="(You'll be asked for a Steam Guard code if needed.)",
                 anchor="w", fg="#777").pack(fill="x", padx=8, pady=(0, 8))

        # Cookie tab
        ck_tab = tk.Frame(self.tabs)
        self.tabs.add(ck_tab, text="Cookie")
        tk.Label(ck_tab, text="Paste the steamLoginSecure cookie, a steamid----token "
                 "line, or just the login token:",
                 anchor="w").pack(fill="x", padx=8, pady=(8, 0))
        self.cookie_entry = tk.Entry(ck_tab, show="•")
        self.cookie_entry.pack(fill="x", padx=8, pady=(0, 8))

        row = tk.Frame(root)
        row.pack(fill="x", **pad)
        self.show_var = tk.BooleanVar(value=False)
        tk.Checkbutton(row, text="Show secrets", variable=self.show_var,
                       command=self._toggle_show).pack(side="left")
        tk.Label(row, text="   Delay (s):").pack(side="left")
        self.delay_var = tk.StringVar(value="1.0")
        tk.Spinbox(row, from_=0.0, to=10.0, increment=0.5, width=5,
                   textvariable=self.delay_var).pack(side="left")

        # --- target selector ---
        trow = tk.Frame(root)
        trow.pack(fill="x", **pad)
        tk.Label(trow, text="Clean:").pack(side="left")
        self.target_var = tk.StringVar(value="friends")
        for value, text in (("friends", "Friends"), ("groups", "Groups"),
                            ("invites", "Pending invites"), ("blocked", "Blocked"),
                            ("family", "Steam Family")):
            tk.Radiobutton(trow, text=text, variable=self.target_var,
                           value=value, command=self._update_labels).pack(side="left")

        # --- buttons ---
        btns = tk.Frame(root)
        btns.pack(fill="x", **pad)
        self.load_btn = tk.Button(btns, text="Load friends", command=self.on_load)
        self.load_btn.pack(side="left")
        self.remove_btn = tk.Button(btns, text="Remove all", state="disabled",
                                    command=self.on_remove)
        self.remove_btn.pack(side="left", padx=8)

        # --- status + progress ---
        self.status = tk.Label(root, text="Ready.", anchor="w", fg="#333")
        self.status.pack(fill="x", padx=10)
        self.progress = ttk.Progressbar(root, mode="determinate")
        self.progress.pack(fill="x", padx=10, pady=4)

        # --- log ---
        self.log = scrolledtext.ScrolledText(root, height=14, state="disabled",
                                             font=("Consolas", 9))
        self.log.pack(fill="both", expand=True, padx=10, pady=(4, 10))

        # Let core report rate-limit waits into our log.
        core.status_cb = lambda m: (self.log_line(f"[~] {m}"), self.set_status(m))

        self.root.after(100, self._drain_log)

    # target -> (noun, load verb, action verb, past tense)
    TARGETS = {
        "friends": ("friends", "Load friends", "Remove all", "removed"),
        "groups":  ("groups", "Load groups", "Leave all", "left"),
        "invites": ("pending invites", "Load invites", "Decline all", "declined"),
        "blocked": ("blocked users", "Load blocked", "Unblock all", "unblocked"),
        "family":  ("Steam Family", "Check family", "Leave family", "left"),
    }

    # ---------- helpers ----------
    def _update_labels(self):
        noun, load_lbl, action_lbl, _ = self.TARGETS[self.target_var.get()]
        self.load_btn.config(text=load_lbl)
        self.remove_btn.config(text=action_lbl)

    def _toggle_show(self):
        ch = "" if self.show_var.get() else "•"
        self.cookie_entry.config(show=ch)
        self.pass_entry.config(show=ch)

    def log_line(self, text: str):
        self.log_q.put(text)

    def _drain_log(self):
        while not self.log_q.empty():
            line = self.log_q.get_nowait()
            self.log.config(state="normal")
            self.log.insert("end", line + "\n")
            self.log.see("end")
            self.log.config(state="disabled")
        self.root.after(100, self._drain_log)

    def set_status(self, text: str):
        self.root.after(0, lambda: self.status.config(text=text))

    def set_buttons(self, load: bool, remove: bool):
        self.root.after(0, lambda: (
            self.load_btn.config(state="normal" if load else "disabled"),
            self.remove_btn.config(state="normal" if remove else "disabled"),
        ))

    # ---------- actions ----------
    def _ask_guard_code(self, kind: str) -> str | None:
        prompt = ("Enter the Steam Guard code from your email:"
                  if kind == "email"
                  else "Enter the Steam Guard code from your mobile authenticator:")
        # simpledialog must run on the main thread
        result = {}
        done = threading.Event()

        def ask():
            result["code"] = simpledialog.askstring("Steam Guard", prompt, parent=self.root)
            done.set()

        self.root.after(0, ask)
        done.wait()
        return result.get("code")

    def on_load(self):
        if self.worker and self.worker.is_alive():
            return
        using_cookie = self.tabs.index(self.tabs.select()) == 1
        if using_cookie:
            cookie = core.normalize_cookie(self.cookie_entry.get())
            if not cookie:
                messagebox.showwarning("Missing cookie",
                                       "Paste your steamLoginSecure cookie first.")
                return
            args = ("cookie", cookie, "")
        else:
            user = self.user_entry.get().strip()
            pw = self.pass_entry.get()
            if not user or not pw:
                messagebox.showwarning("Missing login",
                                       "Enter both username and password.")
                return
            args = ("password", user, pw)
        self.set_buttons(load=False, remove=False)
        self.worker = threading.Thread(target=self._load_worker, args=args, daemon=True)
        self.worker.start()

    def _load_worker(self, method: str, a: str, b: str):
        if method == "cookie":
            try:
                self.steamid = core.steamid_from_cookie(a)
                self.session = core.make_session(a)
            except ValueError as e:
                self.log_line(f"[!] {e}")
                self.set_status("Invalid cookie.")
                self.set_buttons(load=True, remove=False)
                return
        else:
            self.set_status("Logging in...")
            self.log_line(f"[*] Logging in as {a} ...")
            code = ""
            while True:
                try:
                    self.session, self.steamid = core.login_with_password(a, b, code)
                    break
                except core.SteamGuardNeeded as g:
                    code = self._ask_guard_code(g.kind)
                    if not code:
                        self.log_line("[!] Login cancelled (no Steam Guard code).")
                        self.set_status("Login cancelled.")
                        self.set_buttons(load=True, remove=False)
                        return
                    self.log_line("[*] Submitting Steam Guard code...")
                except Exception as e:
                    self.log_line(f"[!] Login failed: {e}")
                    self.set_status("Login failed.")
                    self.set_buttons(load=True, remove=False)
                    return

        self.target = self.target_var.get()
        noun = self.TARGETS[self.target][0]
        self.set_status(f"Account {self.steamid} - loading {noun}...")
        self.log_line(f"[*] Account SteamID64: {self.steamid}")
        try:
            if self.target == "friends":
                self.items = core.get_friend_ids(self.session, self.steamid)
            elif self.target == "groups":
                self.items = core.get_group_ids(self.session, self.steamid)
            elif self.target == "invites":
                self.items = core.get_pending_invites(self.session, self.steamid)
            elif self.target == "blocked":
                self.items = core.get_blocked_ids(self.session, self.steamid)
            elif self.target == "family":
                token = core.get_webapi_token(self.session)
                self.family_token = token
                self.family_gid = core.get_family_groupid(self.session,
                                                           self.steamid, token)
                self.items = [self.family_gid] if self.family_gid else []
        except Exception as e:
            self.log_line(f"[!] {e}")
            self.set_status(f"Failed to load {noun}.")
            self.set_buttons(load=True, remove=False)
            return

        if self.target == "family":
            if self.items:
                self.log_line(f"[*] This account is in family group {self.family_gid}.")
            else:
                self.log_line("[*] This account is not in any Steam Family.")
        else:
            self.log_line(f"[*] Found {len(self.items)} {noun}.")
        self.set_status(f"Account {self.steamid} - {len(self.items)} {noun} loaded.")
        self.set_buttons(load=True, remove=bool(self.items))

    def on_remove(self):
        if self.worker and self.worker.is_alive():
            return
        if not self.items:
            return
        noun, _, action_lbl, _ = self.TARGETS[self.target]
        if self.target == "family":
            msg = ("Leave your Steam Family?\n\n"
                   "WARNING: Steam enforces a 1-YEAR cooldown before you can "
                   "join or create another family after leaving. This cannot be "
                   "undone. Child accounts cannot leave on their own.\n\nContinue?")
        else:
            msg = f"{action_lbl} {len(self.items)} {noun} from {self.steamid}?"
        if not messagebox.askyesno("Confirm", msg, icon="warning"):
            return
        try:
            delay = max(0.0, float(self.delay_var.get()))
        except ValueError:
            delay = 1.0
        self.set_buttons(load=False, remove=False)
        self.worker = threading.Thread(
            target=self._remove_worker, args=(delay,), daemon=True
        )
        self.worker.start()

    def _do_action(self, iid: str) -> bool:
        t = self.target
        if t == "friends":
            return core.remove_friend(self.session, iid)
        if t == "groups":
            return core.leave_group(self.session, self.steamid, iid)
        if t == "invites":
            return core.decline_invite(self.session, iid)
        if t == "blocked":
            return core.unblock_user(self.session, iid)
        if t == "family":
            return core.leave_family(self.session, self.steamid, iid,
                                     self.family_token)
        return False

    def _reload_items(self) -> list[str]:
        t = self.target
        if t == "friends":
            return core.get_friend_ids(self.session, self.steamid)
        if t == "groups":
            return core.get_group_ids(self.session, self.steamid)
        if t == "invites":
            return core.get_pending_invites(self.session, self.steamid)
        if t == "blocked":
            return core.get_blocked_ids(self.session, self.steamid)
        if t == "family":
            gid = core.get_family_groupid(self.session, self.steamid,
                                          self.family_token)
            self.family_gid = gid
            return [gid] if gid else []
        return []

    def _remove_worker(self, delay: float):
        import time
        import random
        past = self.TARGETS[self.target][3]
        total = len(self.items)
        self.root.after(0, lambda: self.progress.config(maximum=total, value=0))
        ok = fail = 0
        for n, iid in enumerate(self.items, 1):
            try:
                if self._do_action(iid):
                    ok += 1
                    self.log_line(f"  [{n}/{total}] {past} {iid}")
                else:
                    fail += 1
                    self.log_line(f"  [{n}/{total}] FAILED {iid}")
            except Exception as e:
                fail += 1
                self.log_line(f"  [{n}/{total}] error on {iid}: {e}")
            self.root.after(0, lambda v=n: self.progress.config(value=v))
            self.set_status(f"Working... {n}/{total}")
            # randomised pause so requests don't look like a fixed-rate bot
            time.sleep(random.uniform(delay, delay * 2.5))

        self.log_line(f"\n[*] Done. {past.capitalize()} {ok}, failed {fail}.")
        self.set_status(f"Done. {past.capitalize()} {ok}, failed {fail}.")
        # refresh remaining list
        try:
            self.items = self._reload_items()
        except Exception:
            self.items = []
        self.set_buttons(load=True, remove=bool(self.items))


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
