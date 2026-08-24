"""
The pieces the windows share: dialogs that know which panel they belong to, and the
start-up path every one of them takes.

There are three panels -- quick, armoury, full editor -- and two ways to reach each: its own
`.bat`, which opens that one alone, and `sas4.bat`, which opens all three as tabs of a single
window. That second way is why the panels are `ttk.Frame` rather than `tk.Tk`: a frame can be
a tab or the only thing in a window of its own, a root cannot be either.

It is also why dialogs are parented. With one root per process an unparented `messagebox`
resolved against the only root there was. With three panels in one window there is more than
one place a modal could belong to, and an unparented one can surface behind the window or
attached to the wrong tab.
"""

import os
import sys

import tkinter as tk
from tkinter import filedialog, messagebox

SAVE_TYPES = [("SAS4 save", "*.save"), ("All files", "*.*")]


class Dialogs:
    """Mixed into every panel, so `self` is always the parent a dialog is raised over."""

    def _error(self, title, message):
        return messagebox.showerror(title, message, parent=self)

    def _warn(self, title, message):
        return messagebox.showwarning(title, message, parent=self)

    def _info(self, title, message):
        return messagebox.showinfo(title, message, parent=self)

    def _ask(self, title, message):
        return messagebox.askokcancel(title, message, parent=self)

    def _pick_save(self, title="Open a SAS4 save"):
        return filedialog.askopenfilename(title=title, parent=self, filetypes=SAVE_TYPES)


def missing_tkinter(alternatives):
    """The message for a Python built without tkinter, listing what still works without it."""
    return SystemExit(
        "This needs tkinter, which did not come with this Python.\n\n"
        "  Windows: re-run the Python installer, choose Modify, and tick\n"
        "           'tcl/tk and IDLE'.\n"
        "  Linux:   install the python3-tk package.\n\n"
        "The same things from a terminal:\n" + alternatives)


def choose_profile(path, what="This editor"):
    """The path to open, asking for one if nothing was found. None means give up.

    `sas4.LIVE` is None when no profile was discovered -- the ordinary case after unzipping
    on a machine without SAS4. Say what happened before showing a picker: a file dialog on
    its own asks someone who has just double-clicked an editor to choose a file they have
    never heard of, and cancelling it used to leave a blank console and no explanation.
    """
    if path and os.path.exists(path):
        return path
    reason = ("None was found on this machine." if not path
              else "The one that was found is gone:\n%s" % path)
    root = tk.Tk()
    root.withdraw()
    print("no profile opened automatically -- %s"
          % ("none found" if not path else "missing: %s" % path))
    try:
        if not messagebox.askokcancel(
                "No SAS4 profile found",
                "%s opens a SAS: Zombie Assault 4 profile.\n\n%s\n\n"
                "That is normal if SAS4 is not installed here, or has never been run and "
                "saved on this account.\n\n"
                "OK       pick a .save file yourself\n"
                "Cancel   quit" % (what, reason)):
            print("cancelled; nothing was opened.")
            return None
        chosen = filedialog.askopenfilename(title="Open a SAS4 save file",
                                            filetypes=SAVE_TYPES)
    finally:
        root.destroy()
    if not chosen:
        print("no file chosen; nothing was opened.")
        return None
    return chosen


def run_standalone(panel, title, geometry, minsize, path, what=None):
    """Open one panel as a window of its own -- what each individual `.bat` does."""
    path = choose_profile(path, what or title)
    if not path:
        return 1
    root = tk.Tk()
    root.title(title)
    root.geometry(geometry)
    root.minsize(*minsize)
    panel(root, path).pack(fill="both", expand=True)
    root.mainloop()
    return 0


def main():
    print(__doc__.strip())
    print("\nThis module is imported by the windows; it does not open one itself.")
    print("Run sas4.bat for all three as tabs, or one of sas4-quick.bat, sas4-gear.bat,")
    print("sas4-gui.bat for a single panel.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
