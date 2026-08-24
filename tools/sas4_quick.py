"""
The short way to change a SAS4 profile: level, money, and granting an item.

`sas4_gui.py` lists every value in the file behind a filter box. That is the right shape
for finding out what is in a profile, and the wrong shape for the three things people
actually came to do -- you end up scrolling a few thousand paths hunting for the one whose
name you have to already know.

This is the other window. Three panels, each doing one job, all of them going through the
same machinery as `sas4.py`: the same refusal while the game is running, the same automatic
backup, the same byte-level replacement, the same verify afterwards. Nothing here writes
anything the command line could not.

    py sas4_quick.py                 the live profile
    py sas4_quick.py <some.save>     any other save

Levels write four fields at once, because a level is not one number: the XP, the granted
skill points and the highest rank all have to agree with it or the profile describes a
character that could not have been played into existence.

Item names come from the table `sas4.py items` downloads. Without it the picker is empty
and the button says so -- the fetch is one click here.
"""

import importlib.util
import json
import os
import sys

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
except ImportError:
    raise SystemExit(
        "This editor needs tkinter, which did not come with this Python.\n\n"
        "  Windows: re-run the Python installer, choose Modify, and tick\n"
        "           'tcl/tk and IDLE'.\n"
        "  Linux:   install the python3-tk package.\n\n"
        "Everything this window does, `sas4.py` also does from a terminal:\n"
        "  py sas4.py level 40\n"
        "  py sas4.py set Inventory/Profile0/Money 250000\n"
        "  py sas4.py give 129")

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("sas4", os.path.join(HERE, "sas4.py"))
sas4 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sas4)
_uspec = importlib.util.spec_from_file_location("sas4_ui", os.path.join(HERE, "sas4_ui.py"))
sas4_ui = importlib.util.module_from_spec(_uspec)
_uspec.loader.exec_module(sas4_ui)
dgdata = sas4.dgdata
model = sas4.sas4_model

PAD = {"padx": 8, "pady": 4}


class Quick(sas4_ui.Dialogs, ttk.Frame):
    """Level, money and one item. A tab of `sas4.bat`, or the whole of `sas4-quick.bat`.

    `on_advanced` is what the "Advanced editor…" button does: in the combined window it
    selects the editor tab, and standalone it starts the editor in its own process. The
    panel does not need to know which.
    """

    def __init__(self, parent, path, on_advanced=None, on_items_changed=None):
        super().__init__(parent)
        self.on_advanced = on_advanced
        self.on_items_changed = on_items_changed

        self.path = path if isinstance(path, tk.StringVar) else tk.StringVar(value=path or "")
        self.document = None
        self.items = []                # (label, domain, id) for the picker, filtered view
        self._build()
        self.reload()

    # --- layout --------------------------------------------------------------------------

    def _build(self):
        # File. The path is an editable field on purpose: paste one in, or point Browse at
        # it, and Open re-reads. Switching profiles should not mean restarting the window.
        box = ttk.LabelFrame(self, text="Save file", padding=8)
        box.pack(fill="x", **PAD)
        ttk.Entry(box, textvariable=self.path).pack(side="left", fill="x", expand=True)
        ttk.Button(box, text="Browse…", command=self.browse).pack(side="left", padx=(6, 0))
        ttk.Button(box, text="Open", command=self.reload).pack(side="left", padx=4)

        self.summary = ttk.LabelFrame(self, text="This character", padding=8)
        self.summary.pack(fill="x", **PAD)
        self.summary_text = tk.StringVar(value="nothing loaded")
        ttk.Label(self.summary, textvariable=self.summary_text,
                  justify="left", font=("TkDefaultFont", 10)).pack(anchor="w")

        actions = ttk.Frame(self)
        actions.pack(fill="x", **PAD)

        lvl = ttk.LabelFrame(actions, text="Level", padding=8)
        lvl.pack(side="left", fill="both", expand=True)
        ttk.Label(lvl, text="Set to (1-%d)" % model.MAX_LEVEL).pack(anchor="w")
        self.level_var = tk.StringVar(value="1")
        ttk.Spinbox(lvl, from_=1, to=model.MAX_LEVEL, textvariable=self.level_var,
                    width=8).pack(anchor="w", pady=4)
        ttk.Label(lvl, text="also sets XP, skill points\nand highest rank",
                  foreground="#666").pack(anchor="w")
        ttk.Button(lvl, text="Apply level", command=self.apply_level).pack(anchor="w", pady=6)

        cash = ttk.LabelFrame(actions, text="Money", padding=8)
        cash.pack(side="left", fill="both", expand=True, padx=(8, 0))
        ttk.Label(cash, text="Set to").pack(anchor="w")
        self.money_var = tk.StringVar(value="0")
        ttk.Entry(cash, textvariable=self.money_var, width=14).pack(anchor="w", pady=4)
        ttk.Label(cash, text="one number, 0 to 2^31-1", foreground="#666").pack(anchor="w")
        ttk.Button(cash, text="Apply money", command=self.apply_money).pack(anchor="w", pady=6)

        give = ttk.LabelFrame(self, text="Grant an item", padding=8)
        give.pack(fill="both", expand=True, **PAD)

        row = ttk.Frame(give)
        row.pack(fill="x")
        ttk.Label(row, text="Search").pack(side="left")
        self.search_var = tk.StringVar()
        self.search_var.trace_add("write", lambda *_: self.refresh_items())
        ttk.Entry(row, textvariable=self.search_var, width=30).pack(side="left", padx=6)
        ttk.Label(row, text="Grade").pack(side="left", padx=(12, 0))
        self.grade_var = tk.StringVar(value="0")
        ttk.Spinbox(row, from_=0, to=12, textvariable=self.grade_var,
                    width=5).pack(side="left", padx=4)
        ttk.Label(row, text="Bonus").pack(side="left", padx=(8, 0))
        self.bonus_var = tk.StringVar(value="0")
        ttk.Spinbox(row, from_=0, to=10, textvariable=self.bonus_var,
                    width=5).pack(side="left", padx=4)

        listing = ttk.Frame(give)
        listing.pack(fill="both", expand=True, pady=6)
        self.item_list = tk.Listbox(listing, height=10, activestyle="dotbox")
        bar = ttk.Scrollbar(listing, orient="vertical", command=self.item_list.yview)
        self.item_list.configure(yscrollcommand=bar.set)
        self.item_list.pack(side="left", fill="both", expand=True)
        bar.pack(side="left", fill="y")
        self.item_list.bind("<Double-1>", lambda _e: self.grant())

        buttons = ttk.Frame(give)
        buttons.pack(fill="x")
        self.grant_button = ttk.Button(buttons, text="Grant selected", command=self.grant)
        self.grant_button.pack(side="left")
        ttk.Button(buttons, text="Download item names",
                   command=self.fetch_items).pack(side="left", padx=6)
        ttk.Button(buttons, text="Advanced editor…",
                   command=self.open_advanced).pack(side="right")

        self.status = tk.StringVar()
        ttk.Label(self, textvariable=self.status, relief="sunken", anchor="w",
                  padding=(8, 4)).pack(fill="x", side="bottom")

    # --- data ----------------------------------------------------------------------------

    def reload(self):
        path = self.path.get().strip('"').strip()
        if not path:
            self.say("no file chosen -- Browse for a .save")
            return
        if not os.path.isfile(path):
            self.say("not a file: %s" % path)
            return
        try:
            raw, self.document = sas4.load(path)
        except Exception as problem:
            self._error("Cannot read this file", str(problem))
            self.say("failed to read %s" % path)
            return
        self.path.set(path)
        stored, _computed, ok = dgdata.verify(raw)

        profile = (self.document.get("Inventory") or {}).get("Profile0") or {}
        skills = profile.get("Skills") or {}
        level = skills.get("PlayerLevel")
        self.summary_text.set(
            "name        %s\nlevel       %s      XP %s\n"
            "money       %s\nskill points %s available\nchecksum    %s (%s)"
            % (profile.get("Name", "?"), level,
               "{:,}".format(skills.get("PlayerTotalXp") or 0),
               "{:,}".format(profile.get("Money") or 0),
               skills.get("AvailableSkillPoints"),
               stored, "valid" if ok else "MISMATCH"))
        if isinstance(level, int):
            self.level_var.set(str(level))
        if isinstance(profile.get("Money"), int):
            self.money_var.set(str(profile["Money"]))
        self.refresh_items()
        self.say("loaded %s" % os.path.basename(path))

    def refresh_items(self):
        rows = sas4.item_catalog()
        needle = self.search_var.get().strip().lower()
        self.items = []
        for domain, tier, category, item_id, name in rows:
            if domain not in ("weapon", "equipment"):
                continue                      # only these two are grantable as finished items
            label = "%-10s %-9s %-16s %5d  %s" % (domain, tier, category, item_id, name)
            if needle and needle not in label.lower():
                continue
            self.items.append((label, domain, item_id))
        self.item_list.delete(0, "end")
        for label, _domain, _item_id in self.items:
            self.item_list.insert("end", label)
        if not rows:
            self.item_list.insert("end", "  no item table yet -- press Download item names")

    def say(self, message):
        warning = "   |   THE GAME IS RUNNING -- writing is blocked" if sas4.game_running() else ""
        self.status.set(message + warning)

    # --- actions -------------------------------------------------------------------------

    def browse(self):
        chosen = self._pick_save()
        if chosen:
            self.path.set(chosen)
            self.reload()

    def write(self, plan, what):
        """One place for every write: the guards, the confirmation, the backup, the verify."""
        if self.document is None:
            self.say("load a save first")
            return
        changes = [(p, n) for p, _o, n in sas4.pending(self.document, plan)]
        if not changes:
            self.say("%s: already that -- nothing to write" % what)
            return
        if sas4.game_running():
            self._warn(
                "The game is running",
                "SAS4 rewrites the profile on its own schedule and would overwrite this "
                "edit.\n\nClose the game, then try again.")
            return
        summary = "\n".join("  %s\n      %s  ->  %s" % (p, json.dumps(o), json.dumps(n))
                            for p, o, n in sas4.pending(self.document, plan))
        if not self._ask("Write these changes?",
                                      "%s\n\n%s\n\nA backup is taken first." % (what, summary)):
            return

        ok, saved, message = sas4.apply_edits(self.path.get(), changes)
        if not ok:
            self._error("Not written", "%s\n\nBackup: %s" % (message, saved))
            self.reload()
            return
        problems = model.check(sas4.load(self.path.get())[1])
        self.reload()
        if problems:
            self._warn(
                "Written, but not consistent",
                "%s\n\nThe file was written and the checksum is valid, but a plausibility "
                "check still objects:\n\n%s\n\nBackup: %s"
                % (message, "\n".join("  - " + p for p in problems), saved))
        else:
            self._info("Done", "%s\n%s\nBackup: %s"
                                % (what, message, saved))

    def apply_level(self):
        if self.document is None:
            self.say("load a save first")
            return
        try:
            level = int(self.level_var.get())
            plan, spent = sas4.level_plan(self.document, level)
        except ValueError as problem:
            self._error("Cannot set that level", str(problem))
            return
        note = "level %d" % level
        if spent:
            note += "  (%d point(s) already spent, so %d are granted)" % (
                spent, max(0, level - spent))
        self.write(plan, note)

    def apply_money(self):
        if self.document is None:
            self.say("load a save first")
            return
        try:
            money = int(self.money_var.get().replace(",", ""))
        except ValueError:
            self._error("Not a number", "Money has to be a plain whole number.")
            return
        if not 0 <= money < 2 ** 31:
            self._error("Out of range",
                                 "Money is a 32-bit value: 0 to %d." % (2 ** 31 - 1))
            return
        self.write([("Inventory/Profile0/Money", money)], "money {:,}".format(money))

    def grant(self):
        if self.document is None:
            self.say("load a save first")
            return
        picked = self.item_list.curselection()
        if not picked or picked[0] >= len(self.items):
            self.say("pick an item from the list first")
            return
        _label, domain, item_id = self.items[picked[0]]
        try:
            plan, label = sas4.give_plan(self.document, item_id, kind=domain,
                                         grade=int(self.grade_var.get()),
                                         bonus=int(self.bonus_var.get()))
        except ValueError as problem:
            self._error("Cannot grant that", str(problem))
            return
        self.write(plan, "grant %s" % label)

    def fetch_items(self):
        """Download the community item table, the same way `sas4.py items` does."""
        self.say("downloading the item table…")
        self.update_idletasks()
        try:
            import urllib.request
            os.makedirs(os.path.dirname(sas4.ITEMS_CACHE), exist_ok=True)
            with urllib.request.urlopen(sas4.ITEMS_URL, timeout=30) as response:
                parsed = json.loads(response.read())
            with open(sas4.ITEMS_CACHE, "w", encoding="utf-8") as handle:
                json.dump(parsed, handle, indent=2, ensure_ascii=False)
        except Exception as problem:
            self._error("Download failed",
                                 "%s\n\nFrom: %s\n\nThe rest of the window works without "
                                 "it; only item names need it."
                                 % (problem, sas4.ITEMS_URL))
            self.say("item table download failed")
            return
        sas4._ITEM_CACHE = None               # drop the memoised empty table
        self.refresh_items()
        self.say("item table downloaded: %d grantable items" % len(self.items))
        if self.on_items_changed:
            self.on_items_changed()           # the cache is module-wide; other tabs stale

    def open_advanced(self):
        """Hand off to the full editor for anything these three panels do not cover."""
        if self.on_advanced:
            self.on_advanced()
            return
        import subprocess
        target = os.path.join(HERE, "sas4_gui.py")
        if not os.path.isfile(target):
            self._info("Not here", "sas4_gui.py is not next to this file.")
            return
        subprocess.Popen([sys.executable, target, self.path.get()])


def main():
    return sas4_ui.run_standalone(Quick, "SAS4 quick edit", "820x620", (700, 560),
                                  sys.argv[1] if len(sys.argv) > 1 else sas4.LIVE)


if __name__ == "__main__":
    sys.exit(main())
