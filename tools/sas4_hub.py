"""
All three windows as tabs of one, so editing a profile is a single double-click.

    py sas4_hub.py                   the live profile
    py sas4_hub.py <some.save>       any other save

The three panels also open on their own -- `sas4-quick.bat`, `sas4-gear.bat`,
`sas4-gui.bat` -- and nothing here changes what they do. This adds one window that holds
all three, because wanting to grant a gun and then set a level meant two windows and two
file pickers.

One save is open at a time, shared by every tab: the path field at the top belongs to the
window, not to a panel, so opening a profile opens it everywhere. Tabs are built the first
time they are selected, not up front -- each one loads the profile and walks the item
catalogue, and doing that three times before the window paints is a visible wait for two
panels the person may not touch. Selecting a tab also reloads it, so an edit made in one is
showing in the others by the time they are looked at.
"""

import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


sas4 = _load("sas4")
sas4_ui = _load("sas4_ui")

try:
    import tkinter as tk
    from tkinter import ttk
except ImportError:
    raise sas4_ui.missing_tkinter(
        "  py sas4.py where\n"
        "  py sas4.py level 40\n"
        "  py sas4.py give 129\n"
        "  py sas4.py set Inventory/Profile0/Money 250000")

# (tab label, module, class, what it is) in the order they should be met.
PANELS = [
    ("Quick", "sas4_quick", "Quick", "level, money, grant an item"),
    ("Armoury", "sas4_gear", "Armoury", "browse and grant weapons and equipment"),
    ("Everything", "sas4_gui", "Editor", "every value in the profile"),
]


class Hub(tk.Tk):
    def __init__(self, path):
        super().__init__()
        self.title("SAS4 save editor")
        self.geometry("1140x720")
        self.minsize(900, 580)

        self.path = tk.StringVar(value=path or "")
        self.panels = {}                    # tab index -> the panel built into it
        self._build()
        # Build the first tab now; the rest wait until they are selected.
        self.ensure(0)
        self.after(0, lambda: self.on_tab_changed())

    def _build(self):
        top = ttk.Frame(self, padding=(8, 8, 8, 0))
        top.pack(fill="x")
        ttk.Label(top, text="Save file").pack(side="left")
        ttk.Entry(top, textvariable=self.path).pack(side="left", fill="x", expand=True, padx=6)
        ttk.Button(top, text="Browse…", command=self.browse).pack(side="left")
        ttk.Button(top, text="Open", command=self.reload_all).pack(side="left", padx=4)

        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill="both", expand=True, padx=8, pady=8)
        for label, _module, _cls, hint in PANELS:
            holder = ttk.Frame(self.notebook)
            self.notebook.add(holder, text="  %s  " % label)
            holder.hint = hint
        self.notebook.bind("<<NotebookTabChanged>>", self.on_tab_changed)

        self.status = tk.StringVar(value="")
        ttk.Label(self, textvariable=self.status, relief="sunken", anchor="w",
                  padding=(8, 4)).pack(fill="x", side="bottom")

    # --- tabs ----------------------------------------------------------------------------

    def ensure(self, index):
        """Build tab `index` if it has not been built yet, and return its panel."""
        if index in self.panels:
            return self.panels[index]
        label, module_name, class_name, _hint = PANELS[index]
        holder = self.notebook.winfo_children()[index]
        try:
            module = _load(module_name)
            panel = getattr(module, class_name)(
                holder, self.path,
                on_advanced=lambda: self.notebook.select(len(PANELS) - 1),
                on_items_changed=self.reload_built)
        except Exception as problem:                      # a broken panel must not take the
            ttk.Label(holder, padding=16, foreground="#a00",  # whole window down with it
                      text="%s could not be opened:\n\n%s\n\nThe other tabs still work."
                           % (label, problem)).pack(anchor="w")
            self.panels[index] = None
            return None
        panel.pack(fill="both", expand=True)
        self.panels[index] = panel
        return panel

    def on_tab_changed(self, _event=None):
        index = self.notebook.index(self.notebook.select())
        panel = self.ensure(index)
        # Reload on the way in: another tab may have written since this one last looked.
        if panel is not None and hasattr(panel, "reload"):
            panel.reload()
        label, _m, _c, hint = PANELS[index]
        self.status.set("%s -- %s" % (label, hint))

    def reload_built(self):
        """Reload every tab that exists. The item table is module-wide, so a download in
        one tab leaves the others showing the table as it was."""
        for panel in self.panels.values():
            if panel is not None and hasattr(panel, "reload"):
                panel.reload()

    # --- the file, which belongs to the window rather than to a panel --------------------

    def browse(self):
        chosen = sas4_ui.filedialog.askopenfilename(
            title="Open a SAS4 save", parent=self, filetypes=sas4_ui.SAVE_TYPES)
        if chosen:
            self.path.set(chosen)
            self.reload_all()

    def reload_all(self):
        self.reload_built()
        self.status.set("opened %s" % os.path.basename(self.path.get()))


def main():
    path = sas4_ui.choose_profile(sys.argv[1] if len(sys.argv) > 1 else sas4.LIVE,
                                  "This editor")
    if not path:
        return 1
    Hub(path).mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
