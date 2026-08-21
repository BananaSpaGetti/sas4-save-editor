"""
A window for editing a SAS4 profile, over the same machinery as `sas4.py`.

Everything here delegates: `sas4.py` owns loading, the byte-level edit, the backup and the
checks, and this only draws them. So the two cannot drift apart, and a fix in one is a fix
in both.

    py sas4_gui.py                 the live profile
    py sas4_gui.py <some.save>     any other save

How it behaves, which is the same as `set` on the command line:

  * changes are **staged**, not written -- nothing touches the disk until Save is pressed,
    and Discard throws the staging away;
  * Save refuses while SAS4 is running, because the game rewrites the profile on its own
    schedule and would overwrite the edit;
  * every Save copies the file to a timestamped backup directory first;
  * each value is replaced at the byte level rather than by re-serialising the document;
  * the file is re-read and its checksum verified before the window reports success.

Tkinter ships with Python, so there is nothing to install. `sas4-gui.bat` starts it with a
double click.
"""

import importlib.util
import json
import os
import sys
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("sas4", os.path.join(HERE, "sas4.py"))
sas4 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sas4)
dgdata = sas4.dgdata


class Editor(tk.Tk):
    def __init__(self, path):
        super().__init__()
        self.title("SAS4 profile editor")
        self.geometry("1040x660")
        self.minsize(760, 460)

        self.path = path
        self.document = None
        self.rows = []                 # (path, value) for every scalar, in document order
        self.staged = {}               # path -> new value, not yet written

        self._build()
        self.reload()

    # --- layout --------------------------------------------------------------------------

    def _build(self):
        top = ttk.Frame(self, padding=(8, 8, 8, 4))
        top.pack(fill="x")
        ttk.Label(top, text="Filter").pack(side="left")
        self.filter_var = tk.StringVar()
        self.filter_var.trace_add("write", lambda *_: self.refresh())
        entry = ttk.Entry(top, textvariable=self.filter_var, width=40)
        entry.pack(side="left", padx=(6, 12))
        entry.focus()
        ttk.Label(top, text="Type").pack(side="left")
        self.type_var = tk.StringVar(value="all")
        types = ttk.Combobox(top, textvariable=self.type_var, width=7, state="readonly",
                             values=["all", "bool", "int", "str", "float", "null"])
        types.pack(side="left", padx=(6, 12))
        types.bind("<<ComboboxSelected>>", lambda _: self.refresh())
        ttk.Button(top, text="Open…", command=self.open_file).pack(side="left")
        ttk.Button(top, text="Reload", command=self.reload).pack(side="left", padx=4)
        ttk.Button(top, text="Restore backup…", command=self.restore).pack(side="left")

        columns = ("path", "current", "staged")
        self.tree = ttk.Treeview(self, columns=columns, show="headings", selectmode="browse")
        for name, title, width in (("path", "Path", 560), ("current", "Current", 200),
                                   ("staged", "New value", 200)):
            self.tree.heading(name, text=title)
            self.tree.column(name, width=width, anchor="w")
        self.tree.tag_configure("staged", background="#fff3cd")
        self.tree.pack(fill="both", expand=True, padx=8)
        self.tree.bind("<<TreeviewSelect>>", self.on_select)
        self.tree.bind("<Double-1>", lambda _: self.value_entry.focus())

        bar = ttk.Frame(self, padding=(8, 6))
        bar.pack(fill="x")
        self.selected_label = ttk.Label(bar, text="select a row", width=52)
        self.selected_label.pack(side="left")
        self.value_var = tk.StringVar()
        self.value_entry = ttk.Entry(bar, textvariable=self.value_var, width=28)
        self.value_entry.pack(side="left", padx=6)
        self.value_entry.bind("<Return>", lambda _: self.stage())
        ttk.Button(bar, text="Stage change", command=self.stage).pack(side="left")
        ttk.Button(bar, text="Discard staged", command=self.discard).pack(side="left", padx=6)
        self.save_button = ttk.Button(bar, text="Save to file", command=self.save)
        self.save_button.pack(side="right")

        self.status = tk.StringVar()
        status = ttk.Label(self, textvariable=self.status, relief="sunken", anchor="w",
                           padding=(8, 4))
        status.pack(fill="x", side="bottom")

    # --- data ----------------------------------------------------------------------------

    def reload(self):
        try:
            raw, self.document = sas4.load(self.path)
        except Exception as problem:
            messagebox.showerror("Cannot read this file", str(problem))
            self.status.set("failed to read %s" % self.path)
            return
        stored, computed, ok = dgdata.verify(raw)
        self.checksum_ok = ok
        self.rows = [(p, v) for p, v in sas4.scalars(self.document)]
        self.staged.clear()
        self.refresh()
        self.set_status("loaded %d values, checksum %s (%s)"
                        % (len(self.rows), stored, "valid" if ok else "MISMATCH"))

    def refresh(self):
        needle = self.filter_var.get().strip().lower()
        self.tree.delete(*self.tree.get_children())
        shown = 0
        wanted = self.type_var.get()
        for path, value in self.rows:
            if needle and needle not in path.lower():
                continue
            if wanted != "all" and sas4.kind_of(value) != wanted:
                continue
            staged = self.staged.get(path, "")
            self.tree.insert("", "end", iid=path,
                             values=(path, json.dumps(value), json.dumps(staged) if path in self.staged else ""),
                             tags=("staged",) if path in self.staged else ())
            shown += 1
        self.update_save_button()
        if needle:
            self.set_status("%d of %d values match %r" % (shown, len(self.rows), needle))

    def set_status(self, message):
        warning = ""
        if sas4.game_running():
            warning = "  |  SAS4 IS RUNNING - saving is blocked"
        pending = "  |  %d staged" % len(self.staged) if self.staged else ""
        self.status.set("%s%s%s" % (message, pending, warning))

    def update_save_button(self):
        blocked = sas4.game_running() or not self.staged
        self.save_button.state(["disabled"] if blocked else ["!disabled"])

    # --- actions -------------------------------------------------------------------------

    def on_select(self, _event=None):
        path = self.selection()
        if not path:
            return
        value = self.staged.get(path, dict(self.rows).get(path))
        self.selected_label.config(text=path)
        self.value_var.set(value if isinstance(value, str) else json.dumps(value))

    def selection(self):
        picked = self.tree.selection()
        return picked[0] if picked else None

    def stage(self):
        path = self.selection()
        if not path:
            return
        current = sas4.at_path(self.document, path)
        try:
            new = sas4.coerce(self.value_var.get(), current)
        except ValueError as problem:
            messagebox.showerror("Wrong type", "%s holds %s (%s).\n\n%s"
                                 % (path, json.dumps(current), type(current).__name__, problem))
            return
        if new == current:
            self.staged.pop(path, None)
        else:
            self.staged[path] = new
        self.refresh()
        self.tree.selection_set(path)
        self.tree.see(path)
        self.set_status("staged %s" % path if path in self.staged else "cleared %s" % path)

    def discard(self):
        if not self.staged:
            return
        self.staged.clear()
        self.refresh()
        self.set_status("staged changes discarded, file untouched")

    def save(self):
        if not self.staged:
            return
        if sas4.game_running():
            messagebox.showwarning(
                "The game is running",
                "SAS4 rewrites the profile on its own schedule and would overwrite this "
                "edit.\n\nClose the game, then press Save again.")
            return

        summary = "\n".join("  %s\n      %s  ->  %s"
                            % (p, json.dumps(sas4.at_path(self.document, p)), json.dumps(v))
                            for p, v in self.staged.items())
        if not messagebox.askokcancel("Write these changes?",
                                      "%s\n\nA backup is taken first."
                                      % summary):
            return

        saved = sas4.backup(self.path)
        try:
            raw = open(self.path, "rb").read()
            plain = dgdata.decode(raw)
            # One value at a time, re-parsing in between: each replacement changes the
            # document the next anchor is computed against.
            for path, new in self.staged.items():
                document = json.loads(plain)
                anchor, old_length = sas4.anchor_for(document, plain, path)
                replacement = anchor[:-old_length] + json.dumps(
                    new, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
                plain = plain.replace(anchor, replacement, 1)

            built = dgdata.encode(plain)
            _, _, ok = dgdata.verify(built)
            if not ok:
                raise ValueError("the rebuilt file does not verify; nothing was written")
            with open(self.path, "wb") as handle:
                handle.write(built)
        except Exception as problem:
            messagebox.showerror("Not written", "%s\n\nThe file is unchanged.\nBackup: %s"
                                 % (problem, saved))
            return

        check = open(self.path, "rb").read()
        stored, _, ok = dgdata.verify(check)
        count = len(self.staged)
        self.reload()
        messagebox.showinfo("Saved", "%d value%s written.\nChecksum %s (%s).\nBackup: %s"
                            % (count, "" if count == 1 else "s", stored,
                               "valid" if ok else "MISMATCH", saved))

    def open_file(self):
        chosen = filedialog.askopenfilename(title="Open a profile",
                                            filetypes=[("SAS4 save", "*.save"),
                                                       ("All files", "*.*")])
        if chosen:
            self.path = chosen
            self.reload()

    def restore(self):
        root_dir = sas4.BACKUPS if os.path.isdir(sas4.BACKUPS) else HERE
        backups = sorted((name for name in os.listdir(root_dir)
                          if name.startswith("backup-") and os.path.isdir(os.path.join(root_dir, name))),
                         reverse=True)
        candidates = []
        for name in backups:
            for root, _dirs, files in os.walk(os.path.join(root_dir, name)):
                for filename in files:
                    if filename.lower().endswith(".save"):
                        candidates.append(os.path.join(root, filename))
        if not candidates:
            messagebox.showinfo("No backups", "Nothing under %s yet." % root_dir)
            return
        chosen = filedialog.askopenfilename(title="Restore which backup?",
                                            initialdir=os.path.dirname(candidates[0]),
                                            filetypes=[("SAS4 save", "*.save")])
        if not chosen:
            return
        if sas4.game_running():
            messagebox.showwarning("The game is running", "Close SAS4 first.")
            return
        raw = open(chosen, "rb").read()
        stored, computed, ok = dgdata.verify(raw)
        if not ok and not messagebox.askokcancel(
                "That backup does not verify",
                "stored %s, computed %s.\n\nRestore it anyway?" % (stored, computed)):
            return
        if not messagebox.askokcancel("Restore?", "Overwrite\n  %s\nwith\n  %s"
                                      % (self.path, chosen)):
            return
        sas4.backup(self.path)
        with open(self.path, "wb") as handle:
            handle.write(raw)
        self.reload()
        messagebox.showinfo("Restored", "Restored from %s" % chosen)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else sas4.LIVE
    # sas4.LIVE is None when no profile was discovered -- the case after unzipping on a
    # machine without SAS4. os.path.exists(None) raises, so guard it and let the person pick
    # a save by hand rather than dying with a traceback.
    if not path or not os.path.exists(path):
        root = tk.Tk()
        root.withdraw()
        chosen = filedialog.askopenfilename(
            title="No profile found automatically -- open a save",
            filetypes=[("SAS4 save", "*.save"), ("All files", "*.*")])
        root.destroy()
        if not chosen:
            return 1
        path = chosen
    Editor(path).mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
