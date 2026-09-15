/* Generated from captured `py sas4.py <cmd> --help` output (COLUMNS=80),
 * CRLF normalized to LF -- text-mode stdout reinserts \r on write, matching
 * Python's own text-mode stdout translation. Do not hand-edit; regenerate from
 * the goldens if sas4.py's help text ever changes. */
#ifndef SAS4_CLI_HELP_DATA_H
#define SAS4_CLI_HELP_DATA_H

static const char *CLI_HELP_TOP __attribute__((unused)) =
    "usage: sas4.py [-h] [--file FILE]\n"
    "               {where,view,list,kinds,get,set,give,mastery,contribute,level,items,verify,decode,encode,watch,session,graft} ...\n"
    "\n"
    "One tool for SAS4 profiles: read them, search them, and change values in place.\n"
    "\n"
    "The save is DGDATA-wrapped JSON -- `dgdata.py` holds the format, this holds everything\n"
    "built on top of it. It replaces the separate viewer and save-watcher.\n"
    "\n"
    "    py sas4.py where                                show the profiles found on this machine\n"
    "    py sas4.py view                                 the live profile, by section\n"
    "    py sas4.py view --section skills --slot 1\n"
    "    py sas4.py list --grep money                    every path whose name matches\n"
    "    py sas4.py list --type bool                     every on/off switch in the file\n"
    "    py sas4.py list --under Settings                everything below one path\n"
    "    py sas4.py kinds                                how many values of each type, and where\n"
    "    py sas4.py items                                fetch the item table, once, for names\n"
    "    py sas4.py give 129                             grant a finished item (the loot, no box)\n"
    "    py sas4.py level 40                             set the level, XP, points and rank together\n"
    "    py sas4.py get /Inventory/Profile0/Money        one value\n"
    "    py sas4.py set /Inventory/Profile0/Money 250000 change it, checksum and all\n"
    "    py sas4.py verify                               would the game accept this file\n"
    "    py sas4.py decode [out.json]                    plaintext JSON out\n"
    "    py sas4.py encode <in.json> <out.save>          and back again\n"
    "    py sas4.py watch                                diff the save each time the game writes\n"
    "    py sas4.py session                              read the login session (secrets hidden)\n"
    "    py sas4.py graft other.save --fields A,B        copy progress in, keeping your identity\n"
    "\n"
    "Paths may be written with or without a leading slash. Prefer without it in Git Bash: MSYS\n"
    "rewrites a leading-slash argument into a Windows path before Python ever sees it, and the\n"
    "error that comes back mentions `C:` for no visible reason.\n"
    "\n"
    "`set` is deliberately cautious, because getting this wrong once already made the game throw\n"
    "a character away:\n"
    "\n"
    "  * it refuses to run while SAS4 is running, since the game rewrites the save on its own\n"
    "    schedule and would overwrite the edit;\n"
    "  * it copies the file to a timestamped `backup-` directory first, every time;\n"
    "  * it edits the **bytes** of the one value rather than re-serialising the document, so\n"
    "    nothing else in 120 KB of JSON can shift underneath -- the game's JSON writer does not\n"
    "    format exactly the way Python's does, and a re-serialise rewrites the whole file;\n"
    "  * it re-reads the result from disk and verifies the checksum before reporting success.\n"
    "\n"
    "Anything that touches a value the server also tracks carries account risk that no amount of\n"
    "local care removes; `FINDINGS.md` has the detail.\n"
    "\n"
    "positional arguments:\n"
    "  {where,view,list,kinds,get,set,give,mastery,contribute,level,items,verify,decode,encode,watch,session,graft}\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --file FILE           save file to work on\n"
    "";

static const char *CLI_USAGE_TOP __attribute__((unused)) =
    "usage: sas4.py [-h] [--file FILE]\n"
    "               {where,view,list,kinds,get,set,give,mastery,contribute,level,items,verify,decode,encode,watch,session,graft} ...\n"
    "";

static const char *CLI_HELP_WHERE __attribute__((unused)) =
    "usage: sas4.py where [-h]\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "";

static const char *CLI_USAGE_WHERE __attribute__((unused)) =
    "usage: sas4.py where [-h]\n"
    "";

static const char *CLI_HELP_VIEW __attribute__((unused)) =
    "usage: sas4.py view [-h] [--slot SLOT]\n"
    "                    [--section {identity,currency,skills,equipment,weapons,boxes,global}]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --slot SLOT\n"
    "  --section {identity,currency,skills,equipment,weapons,boxes,global}\n"
    "";

static const char *CLI_USAGE_VIEW __attribute__((unused)) =
    "usage: sas4.py view [-h] [--slot SLOT]\n"
    "                    [--section {identity,currency,skills,equipment,weapons,boxes,global}]\n"
    "";

static const char *CLI_HELP_LIST __attribute__((unused)) =
    "usage: sas4.py list [-h] [--grep GREP] [--type {bool,float,int,null,str}]\n"
    "                    [--under UNDER]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --grep GREP           only paths whose name contains this\n"
    "  --type {bool,float,int,null,str}\n"
    "                        only values of this type -- `bool` is every on/off\n"
    "                        switch\n"
    "  --under UNDER         only paths under this one, e.g. Settings\n"
    "";

static const char *CLI_USAGE_LIST __attribute__((unused)) =
    "usage: sas4.py list [-h] [--grep GREP] [--type {bool,float,int,null,str}]\n"
    "                    [--under UNDER]\n"
    "";

static const char *CLI_HELP_KINDS __attribute__((unused)) =
    "usage: sas4.py kinds [-h]\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "";

static const char *CLI_USAGE_KINDS __attribute__((unused)) =
    "usage: sas4.py kinds [-h]\n"
    "";

static const char *CLI_HELP_GET __attribute__((unused)) =
    "usage: sas4.py get [-h] path\n"
    "\n"
    "positional arguments:\n"
    "  path\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "";

static const char *CLI_USAGE_GET __attribute__((unused)) =
    "usage: sas4.py get [-h] path\n"
    "";

static const char *CLI_HELP_SET __attribute__((unused)) =
    "usage: sas4.py set [-h] [--dry-run] [--force] path value\n"
    "\n"
    "positional arguments:\n"
    "  path\n"
    "  value\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "  --dry-run\n"
    "  --force     edit anyway with the game running or the checksum already wrong\n"
    "";

static const char *CLI_USAGE_SET __attribute__((unused)) =
    "usage: sas4.py set [-h] [--dry-run] [--force] path value\n"
    "";

static const char *CLI_HELP_GIVE __attribute__((unused)) =
    "usage: sas4.py give [-h] [--kind {auto,weapon,equipment}] [--grade GRADE]\n"
    "                    [--bonus BONUS] [--slot SLOT] [--slotprofile SLOTPROFILE]\n"
    "                    [--dry-run] [--force]\n"
    "                    item\n"
    "\n"
    "positional arguments:\n"
    "  item                  item id (see `items`)\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --kind {auto,weapon,equipment}\n"
    "                        weapon and equipment ids overlap; say which when an id\n"
    "                        is both\n"
    "  --grade GRADE\n"
    "  --bonus BONUS\n"
    "  --slot SLOT           equipped slot, for equipment\n"
    "  --slotprofile SLOTPROFILE\n"
    "                        which character slot\n"
    "  --dry-run\n"
    "  --force\n"
    "";

static const char *CLI_USAGE_GIVE __attribute__((unused)) =
    "usage: sas4.py give [-h] [--kind {auto,weapon,equipment}] [--grade GRADE]\n"
    "                    [--bonus BONUS] [--slot SLOT] [--slotprofile SLOTPROFILE]\n"
    "                    [--dry-run] [--force]\n"
    "                    item\n"
    "";

static const char *CLI_HELP_MASTERY __attribute__((unused)) =
    "usage: sas4.py mastery [-h] [--set SET | --all LEVEL] [--slot SLOT]\n"
    "                       [--dry-run] [--force]\n"
    "\n"
    "options:\n"
    "  -h, --help   show this help message and exit\n"
    "  --set SET    tracks to raise, e.g. 3=5,7=2\n"
    "  --all LEVEL  set every track on this character to LEVEL\n"
    "  --slot SLOT  which character slot\n"
    "  --dry-run\n"
    "  --force      write anyway with the game running or the checksum already\n"
    "               wrong\n"
    "";

static const char *CLI_USAGE_MASTERY __attribute__((unused)) =
    "usage: sas4.py mastery [-h] [--set SET | --all LEVEL] [--slot SLOT]\n"
    "                       [--dry-run] [--force]\n"
    "";

static const char *CLI_HELP_CONTRIBUTE __attribute__((unused)) =
    "usage: sas4.py contribute [-h] [--slot SLOT] [--print]\n"
    "\n"
    "options:\n"
    "  -h, --help   show this help message and exit\n"
    "  --slot SLOT  which character slot\n"
    "  --print      print the report and write no file\n"
    "";

static const char *CLI_USAGE_CONTRIBUTE __attribute__((unused)) =
    "usage: sas4.py contribute [-h] [--slot SLOT] [--print]\n"
    "";

static const char *CLI_HELP_LEVEL __attribute__((unused)) =
    "usage: sas4.py level [-h] [--slot SLOT] [--dry-run] [--force] level\n"
    "\n"
    "positional arguments:\n"
    "  level        the level to set (1-100)\n"
    "\n"
    "options:\n"
    "  -h, --help   show this help message and exit\n"
    "  --slot SLOT  which character slot\n"
    "  --dry-run\n"
    "  --force      write anyway with the game running or the checksum already\n"
    "               wrong\n"
    "";

static const char *CLI_USAGE_LEVEL __attribute__((unused)) =
    "usage: sas4.py level [-h] [--slot SLOT] [--dry-run] [--force] level\n"
    "";

static const char *CLI_HELP_ITEMS __attribute__((unused)) =
    "usage: sas4.py items [-h] [--catalog [CATALOG]]\n"
    "\n"
    "options:\n"
    "  -h, --help           show this help message and exit\n"
    "  --catalog [CATALOG]  also write a readable list of every grantable item\n"
    "";

static const char *CLI_USAGE_ITEMS __attribute__((unused)) =
    "usage: sas4.py items [-h] [--catalog [CATALOG]]\n"
    "";

static const char *CLI_HELP_VERIFY __attribute__((unused)) =
    "usage: sas4.py verify [-h]\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "";

static const char *CLI_USAGE_VERIFY __attribute__((unused)) =
    "usage: sas4.py verify [-h]\n"
    "";

static const char *CLI_HELP_DECODE __attribute__((unused)) =
    "usage: sas4.py decode [-h] [out]\n"
    "\n"
    "positional arguments:\n"
    "  out\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "";

static const char *CLI_USAGE_DECODE __attribute__((unused)) =
    "usage: sas4.py decode [-h] [out]\n"
    "";

static const char *CLI_HELP_ENCODE __attribute__((unused)) =
    "usage: sas4.py encode [-h] json out\n"
    "\n"
    "positional arguments:\n"
    "  json\n"
    "  out\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "";

static const char *CLI_USAGE_ENCODE __attribute__((unused)) =
    "usage: sas4.py encode [-h] json out\n"
    "";

static const char *CLI_HELP_WATCH __attribute__((unused)) =
    "usage: sas4.py watch [-h] [--archive]\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "  --archive   also copy each version into the saves directory as it appears\n"
    "";

static const char *CLI_USAGE_WATCH __attribute__((unused)) =
    "usage: sas4.py watch [-h] [--archive]\n"
    "";

static const char *CLI_HELP_SESSION __attribute__((unused)) =
    "usage: sas4.py session [-h] [--session SESSION] [--set SET] [--yes] [--force]\n"
    "\n"
    "options:\n"
    "  -h, --help         show this help message and exit\n"
    "  --session SESSION  the current.session file\n"
    "  --set SET          one field to change, as name=value\n"
    "  --yes              skip the credential confirmation\n"
    "  --force            edit with the game running\n"
    "";

static const char *CLI_USAGE_SESSION __attribute__((unused)) =
    "usage: sas4.py session [-h] [--session SESSION] [--set SET] [--yes] [--force]\n"
    "";

static const char *CLI_HELP_GRAFT __attribute__((unused)) =
    "usage: sas4.py graft [-h] [--fields FIELDS] [--apply] [--force] source\n"
    "\n"
    "positional arguments:\n"
    "  source           the save to copy progress from\n"
    "\n"
    "options:\n"
    "  -h, --help       show this help message and exit\n"
    "  --fields FIELDS  comma-separated paths to copy\n"
    "  --apply          write the changes (default: preview)\n"
    "  --force          write with the game running\n"
    "";

static const char *CLI_USAGE_GRAFT __attribute__((unused)) =
    "usage: sas4.py graft [-h] [--fields FIELDS] [--apply] [--force] source\n"
    "";

#endif