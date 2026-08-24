"""
The armoury: grant weapons and equipment, and see what the profile already holds.

`sas4_quick.py` has a one-line item picker, which is enough when you know what you want.
This is the window for when you do not: the catalogue on the left with the filters that
matter -- weapon or equipment, which tier, which category -- and on the right, the things
this character already owns, read back out of the save rather than assumed.

    py sas4_gear.py                  the live profile
    py sas4_gear.py <some.save>      any other save

Two things it does that the one-line picker cannot:

  * **Several at once.** Claimed is a single value in the file, so granting five items is
    one replacement over one backup, not five of each. Ctrl-click or Shift-click the
    catalogue and grant the lot.
  * **Take things back.** Anything granted can be removed again, which matters because a
    granted item is not an undo away once the game has written the profile back.

Grade and bonus apply to everything granted in that press. Equipment also carries a slot;
weapons ignore it.

Item names come from the table `sas4.py items` downloads. Without it the catalogue is empty
and the button says so -- fetching is one click.

Everything writes through `sas4.py`, so it is the same refusal while the game is running,
the same automatic backup, the same byte-level replacement, the same verify afterwards.
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
        "This window needs tkinter, which did not come with this Python.\n\n"
        "  Windows: re-run the Python installer, choose Modify, and tick\n"
        "           'tcl/tk and IDLE'.\n"
        "  Linux:   install the python3-tk package.\n\n"
        "The same thing from a terminal:\n"
        "  py sas4.py items --catalog     write the list of everything grantable\n"
        "  py sas4.py give <id> --grade 12 --bonus 10")

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("sas4", os.path.join(HERE, "sas4.py"))
sas4 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sas4)
_uspec = importlib.util.spec_from_file_location("sas4_ui", os.path.join(HERE, "sas4_ui.py"))
sas4_ui = importlib.util.module_from_spec(_uspec)
_uspec.loader.exec_module(sas4_ui)
dgdata = sas4.dgdata
model = sas4.sas4_model

ANY = "(any)"


class Armoury(sas4_ui.Dialogs, ttk.Frame):
    """The catalogue and what this character owns. A tab of `sas4.bat`, or `sas4-gear.bat`."""

    def __init__(self, parent, path, on_advanced=None, on_items_changed=None):
        super().__init__(parent)
        self.on_advanced = on_advanced
        self.on_items_changed = on_items_changed

        self.path = path if isinstance(path, tk.StringVar) else tk.StringVar(value=path or "")
        self.document = None
        self.catalogue = []            # rows currently listed, parallel to the left tree
        self.owned = []                # rows currently listed, parallel to the right tree
        self._build()
        self.reload()

    # --- layout --------------------------------------------------------------------------

    def _build(self):
        top = ttk.LabelFrame(self, text="Save file", padding=8)
        top.pack(fill="x", padx=8, pady=(8, 4))
        ttk.Entry(top, textvariable=self.path).pack(side="left", fill="x", expand=True)
        ttk.Button(top, text="Browse…", command=self.browse).pack(side="left", padx=(6, 0))
        ttk.Button(top, text="Open", command=self.reload).pack(side="left", padx=4)

        panes = ttk.Frame(self)
        panes.pack(fill="both", expand=True, padx=8)

        # --- left: the catalogue ---
        left = ttk.LabelFrame(panes, text="Catalogue", padding=8)
        left.pack(side="left", fill="both", expand=True)

        filters = ttk.Frame(left)
        filters.pack(fill="x")
        self.domain_var = tk.StringVar(value="weapon")
        for text, value in (("Weapons", "weapon"), ("Equipment", "equipment")):
            ttk.Radiobutton(filters, text=text, value=value, variable=self.domain_var,
                            command=self.on_domain).pack(side="left")
        ttk.Label(filters, text="Tier").pack(side="left", padx=(12, 4))
        self.tier_var = tk.StringVar(value=ANY)
        self.tier_box = ttk.Combobox(filters, textvariable=self.tier_var, width=10,
                                     state="readonly")
        self.tier_box.pack(side="left")
        self.tier_box.bind("<<ComboboxSelected>>", lambda _e: self.refresh_catalogue())
        ttk.Label(filters, text="Type").pack(side="left", padx=(12, 4))
        self.category_var = tk.StringVar(value=ANY)
        self.category_box = ttk.Combobox(filters, textvariable=self.category_var, width=16,
                                         state="readonly")
        self.category_box.pack(side="left")
        self.category_box.bind("<<ComboboxSelected>>", lambda _e: self.refresh_catalogue())

        search = ttk.Frame(left)
        search.pack(fill="x", pady=(6, 0))
        ttk.Label(search, text="Name contains").pack(side="left")
        self.search_var = tk.StringVar()
        self.search_var.trace_add("write", lambda *_: self.refresh_catalogue())
        ttk.Entry(search, textvariable=self.search_var).pack(side="left", fill="x",
                                                             expand=True, padx=6)

        self.cat_tree = self._tree(left, (("tier", "Tier", 70), ("category", "Type", 120),
                                          ("id", "ID", 60), ("name", "Name", 260)),
                                   "extended")
        self.cat_tree.bind("<Double-1>", lambda _e: self.grant())
        self.cat_count = tk.StringVar()
        ttk.Label(left, textvariable=self.cat_count, foreground="#666").pack(anchor="w")

        # --- middle: the grant controls, between the two lists on purpose ---
        middle = ttk.Frame(panes, padding=(10, 40))
        middle.pack(side="left", fill="y")
        box = ttk.LabelFrame(middle, text="Grant as", padding=8)
        box.pack()
        ttk.Label(box, text="Grade").grid(row=0, column=0, sticky="w")
        self.grade_var = tk.StringVar(value="0")
        ttk.Spinbox(box, from_=0, to=12, width=6,
                    textvariable=self.grade_var).grid(row=0, column=1, pady=2)
        ttk.Label(box, text="Bonus").grid(row=1, column=0, sticky="w")
        self.bonus_var = tk.StringVar(value="0")
        ttk.Spinbox(box, from_=0, to=10, width=6,
                    textvariable=self.bonus_var).grid(row=1, column=1, pady=2)
        self.slot_label = ttk.Label(box, text="Slot")
        self.slot_label.grid(row=2, column=0, sticky="w")
        self.slot_var = tk.StringVar(value="2")
        self.slot_spin = ttk.Spinbox(box, from_=0, to=5, width=6, textvariable=self.slot_var)
        self.slot_spin.grid(row=2, column=1, pady=2)
        ttk.Button(middle, text="Grant  ▶", command=self.grant).pack(pady=(12, 4), fill="x")
        ttk.Button(middle, text="◀  Remove", command=self.remove).pack(fill="x")
        ttk.Button(middle, text="Get item names",
                   command=self.fetch_items).pack(pady=(18, 0), fill="x")

        # --- right: what this character owns ---
        right = ttk.LabelFrame(panes, text="Already owned", padding=8)
        right.pack(side="left", fill="both", expand=True)
        self.owned_tree = self._tree(right, (("kind", "Kind", 90), ("id", "ID", 60),
                                             ("name", "Name", 230), ("grade", "Grade", 60),
                                             ("bonus", "Bonus", 60), ("slot", "Slot", 50)),
                                     "extended")
        self.owned_count = tk.StringVar()
        ttk.Label(right, textvariable=self.owned_count, foreground="#666").pack(anchor="w")

        self.status = tk.StringVar()
        ttk.Label(self, textvariable=self.status, relief="sunken", anchor="w",
                  padding=(8, 4)).pack(fill="x", side="bottom")

    @staticmethod
    def _tree(parent, columns, selectmode):
        holder = ttk.Frame(parent)
        holder.pack(fill="both", expand=True, pady=6)
        tree = ttk.Treeview(holder, columns=[c[0] for c in columns], show="headings",
                            selectmode=selectmode)
        for name, title, width in columns:
            tree.heading(name, text=title)
            tree.column(name, width=width, anchor="w")
        bar = ttk.Scrollbar(holder, orient="vertical", command=tree.yview)
        tree.configure(yscrollcommand=bar.set)
        tree.pack(side="left", fill="both", expand=True)
        bar.pack(side="left", fill="y")
        return tree

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
        stored, _c, ok = dgdata.verify(raw)
        self.on_domain()
        self.refresh_owned()
        self.say("loaded %s -- checksum %s (%s)"
                 % (os.path.basename(path), stored, "valid" if ok else "MISMATCH"))

    def on_domain(self):
        """Domain drives which tiers and categories exist, so rebuild those first."""
        domain = self.domain_var.get()
        rows = [r for r in sas4.item_catalog() if r[0] == domain]
        tiers = sorted({r[1] for r in rows})
        categories = sorted({r[2] for r in rows})
        self.tier_box["values"] = [ANY] + tiers
        self.category_box["values"] = [ANY] + categories
        if self.tier_var.get() not in self.tier_box["values"]:
            self.tier_var.set(ANY)
        if self.category_var.get() not in self.category_box["values"]:
            self.category_var.set(ANY)
        # A slot is an equipment idea; weapons carry no EquippedSlot.
        state = "normal" if domain == "equipment" else "disabled"
        self.slot_spin.configure(state=state)
        self.slot_label.configure(foreground="black" if domain == "equipment" else "#999")
        self.refresh_catalogue()

    def refresh_catalogue(self):
        domain = self.domain_var.get()
        tier, category = self.tier_var.get(), self.category_var.get()
        needle = self.search_var.get().strip().lower()
        rows = sas4.item_catalog()
        self.catalogue = [r for r in rows
                          if r[0] == domain
                          and (tier == ANY or r[1] == tier)
                          and (category == ANY or r[2] == category)
                          and (not needle or needle in r[4].lower())]
        self.cat_tree.delete(*self.cat_tree.get_children())
        for i, (_d, tier_, cat_, item_id, name) in enumerate(self.catalogue):
            self.cat_tree.insert("", "end", iid=str(i), values=(tier_, cat_, item_id, name))
        if not rows:
            self.cat_count.set("no item table yet -- press Get item names")
        else:
            self.cat_count.set("%d of %d %s shown"
                               % (len(self.catalogue),
                                  sum(1 for r in rows if r[0] == domain), domain))

    def refresh_owned(self):
        self.owned = sas4.claimed_items(self.document) if self.document else []
        self.owned_tree.delete(*self.owned_tree.get_children())
        for i, (_idx, kind, item_id, name, grade, bonus, slot) in enumerate(self.owned):
            self.owned_tree.insert("", "end", iid=str(i),
                                   values=(kind, item_id, name, grade, bonus,
                                           "-" if slot is None else slot))
        self.owned_count.set("%d item(s) in Strongboxes/Claimed" % len(self.owned))

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
            return False
        changes = [(p, n) for p, _o, n in sas4.pending(self.document, plan)]
        if not changes:
            self.say("%s: nothing would change" % what)
            return False
        if sas4.game_running():
            self._warn(
                "The game is running",
                "SAS4 rewrites the profile on its own schedule and would overwrite this "
                "edit.\n\nClose the game, then try again.")
            return False
        if not self._ask("Write this?", "%s\n\nA backup is taken first." % what):
            return False

        ok, saved, message = sas4.apply_edits(self.path.get(), changes)
        if not ok:
            self._error("Not written", "%s\n\nBackup: %s" % (message, saved))
            self.reload()
            return False
        problems = model.check(sas4.load(self.path.get())[1])
        self.reload()
        if problems:
            self._warn(
                "Written, but not consistent",
                "%s\n\n%s\n\nBackup: %s"
                % (message, "\n".join("  - " + p for p in problems), saved))
        else:
            self.say("%s -- %s   (backup %s)" % (what, message, os.path.basename(saved)))
        return True

    def grant(self):
        if self.document is None:
            self.say("load a save first")
            return
        picked = self.cat_tree.selection()
        if not picked:
            self.say("pick one or more items in the catalogue -- Ctrl-click for several")
            return
        try:
            grade, bonus = int(self.grade_var.get()), int(self.bonus_var.get())
            slot = int(self.slot_var.get())
        except ValueError:
            self._error("Not a number", "Grade, bonus and slot have to be numbers.")
            return

        requests = []
        for iid in picked:
            domain, _tier, _cat, item_id, _name = self.catalogue[int(iid)]
            requests.append((item_id, domain, grade, bonus, slot))
        try:
            plan, labels = sas4.grant_plan(self.document, requests)
        except ValueError as problem:
            self._error("Cannot grant that", str(problem))
            return

        what = ("grant %d items at grade %d, bonus %d" % (len(labels), grade, bonus)
                if len(labels) > 1 else "grant %s" % labels[0])
        if len(labels) > 1:
            what += "\n\n" + "\n".join("  " + l for l in labels)
        self.write(plan, what)

    def remove(self):
        if self.document is None:
            self.say("load a save first")
            return
        picked = self.owned_tree.selection()
        if not picked:
            self.say("pick what to remove on the right")
            return
        rows = [self.owned[int(i)] for i in picked]
        indexes = [r[0] for r in rows]
        names = "\n".join("  %s (%s)" % (r[3], r[1]) for r in rows)
        plan = sas4.drop_claimed(self.document, indexes)
        self.write(plan, "remove %d item(s)\n\n%s" % (len(rows), names))

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
                                 "%s\n\nFrom: %s" % (problem, sas4.ITEMS_URL))
            self.say("item table download failed")
            return
        sas4._ITEM_CACHE = None                # drop the memoised empty table
        self.on_domain()
        self.refresh_owned()                   # names resolve now, so redraw those too
        self.say("item table downloaded")
        if self.on_items_changed:
            self.on_items_changed()            # the cache is module-wide; other tabs stale


def main():
    return sas4_ui.run_standalone(Armoury, "SAS4 armoury -- weapons and equipment",
                                  "1120x680", (900, 560),
                                  sys.argv[1] if len(sys.argv) > 1 else sas4.LIVE)


if __name__ == "__main__":
    sys.exit(main())
