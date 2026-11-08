#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shader catalogue: which native program draws what, per game, shared through the repo.

Each game has one file, tools/nx2/catalog/<program id>.json, listing the programs people have
identified (what they draw, how that was found). Programs are named by their native program key
(the shader dump and shader lab key: a hash of the game's code, the same in every build) and,
when known, by their shader hash (the frame dump "shaders" value and reject_shaders hash: a hash
of the lowered code, so it changes when the lowering of that program changes).

The shader lab keeps a local copy per user that carries over between builds and checkouts
(EDEN_NX2_CATALOG_DIR, default %APPDATA%/eglantine/nx2-catalog or
~/.local/share/eglantine/nx2-catalog). It merges the repo copy into it at start and saves new
labels there. Before committing, compare the two and bring new entries into the repo:

    python3 -I tools/nx2/catalog.py status        # exit 1 when the local copy has more
    python3 -I tools/nx2/catalog.py merge         # local -> repo (and repo -> local)
    python3 -I tools/nx2/catalog.py add --title <id> --key <k> --object "..." [--stage ...]
    python3 -I tools/nx2/catalog.py find <key or hash>
    python3 -I tools/nx2/catalog.py show --title <id>   # Markdown table

Merging never drops an entry: the same program in both copies keeps the newer text field by
field, and fields one side left empty are filled from the other.
"""

import argparse
import datetime
import json
import os
import re
import sys

HEX16 = re.compile(r"^[0-9a-f]{16}$")
TITLE = re.compile(r"^[0-9A-F]{16}$")
HERE = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.join(HERE, "catalog")
FIELDS = ("key", "hash", "stage", "object", "group", "how", "notes", "source", "updated")
TEXT_FIELDS = ("stage", "object", "group", "how", "notes", "source")


def local_dir():
    """The per-user catalogue folder (EDEN_NX2_CATALOG_DIR overrides)."""
    override = os.environ.get("EDEN_NX2_CATALOG_DIR")
    if override:
        return override
    if os.name == "nt":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.join(base, "eglantine", "nx2-catalog")


def now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def normalize_title(title):
    title = (title or "").strip().upper()
    return title if TITLE.match(title) else ""


def clean(entry):
    """An entry with only known fields, lower-case hex ids and string values."""
    result = {}
    for field in FIELDS:
        value = entry.get(field)
        if value is None:
            continue
        value = str(value).strip()
        if field in ("key", "hash"):
            value = value.lower()
            if not HEX16.match(value) or value == "0" * 16:
                continue
        if value:
            result[field] = value
    return result


def empty(title="", game=""):
    return {"title": title, "game": game, "notes": "", "programs": []}


def load(path):
    try:
        with open(path, encoding="utf-8") as file:
            data = json.load(file)
    except (OSError, ValueError):
        return None
    catalog = empty(normalize_title(data.get("title")), str(data.get("game") or ""))
    catalog["notes"] = str(data.get("notes") or "")
    catalog["programs"] = [entry for entry in (clean(item) for item in data.get("programs") or [])
                           if entry.get("key") or entry.get("hash")]
    return catalog


def sort_key(entry):
    return (entry.get("group", "~"), entry.get("object", "~").lower(), entry.get("key", ""),
            entry.get("hash", ""))


def dumps(catalog):
    """JSON with one program per line, so a commit's diff shows one line per changed entry."""
    head = {"title": catalog.get("title", ""), "game": catalog.get("game", ""),
            "notes": catalog.get("notes", "")}
    lines = ["{"]
    for name, value in head.items():
        lines.append(f" {json.dumps(name)}: {json.dumps(value, ensure_ascii=False)},")
    programs = sorted(catalog.get("programs", []), key=sort_key)
    lines.append(' "programs": [')
    for index, entry in enumerate(programs):
        ordered = {field: entry[field] for field in FIELDS if field in entry}
        comma = "," if index + 1 < len(programs) else ""
        lines.append("  " + json.dumps(ordered, ensure_ascii=False) + comma)
    lines.append(" ]")
    lines.append("}")
    return "\n".join(lines) + "\n"


def save(path, catalog):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8", newline="\n") as file:
        file.write(dumps(catalog))
    os.replace(temporary, path)


def same_program(a, b):
    if a.get("key") and b.get("key"):
        return a["key"] == b["key"]
    return bool(a.get("hash") and a.get("hash") == b.get("hash"))


def find(programs, key="", hash_value=""):
    """The entry for a program: by key first, then by shader hash."""
    if key:
        for entry in programs:
            if entry.get("key") == key:
                return entry
    if hash_value:
        for entry in programs:
            if entry.get("hash") == hash_value and (not key or not entry.get("key")):
                return entry
    return None


def combine(a, b):
    """One entry from two entries of the same program: newer non-empty fields win."""
    newer, older = (a, b) if a.get("updated", "") >= b.get("updated", "") else (b, a)
    result = dict(older)
    for field, value in newer.items():
        if value:
            result[field] = value
    return result


def merge(*catalogs):
    """Union of catalogues of one game. Never drops a program."""
    result = empty()
    programs = []
    for catalog in catalogs:
        if not catalog:
            continue
        result["title"] = result["title"] or catalog.get("title", "")
        result["game"] = result["game"] or catalog.get("game", "")
        if len(catalog.get("notes", "")) > len(result["notes"]):
            result["notes"] = catalog["notes"]
        for entry in catalog.get("programs", []):
            matches = [index for index, other in enumerate(programs) if same_program(entry, other)]
            if not matches:
                programs.append(dict(entry))
                continue
            merged = entry
            for index in matches:
                merged = combine(merged, programs[index])
            programs[matches[0]] = merged
            for index in reversed(matches[1:]):
                del programs[index]
    result["programs"] = programs
    return result


def compare(local, repo):
    """(new, updated, repo_only) entry lists: what the local copy would add to the repo copy."""
    local_programs = (local or empty())["programs"]
    repo_programs = (repo or empty())["programs"]
    new, updated = [], []
    for entry in local_programs:
        others = [other for other in repo_programs if same_program(entry, other)]
        if not others:
            new.append(entry)
            continue
        combined = combine(entry, others[0])
        if any(combined.get(field) != others[0].get(field) for field in FIELDS if field != "updated"):
            updated.append(entry)
    repo_only = [entry for entry in repo_programs
                 if not any(same_program(entry, other) for other in local_programs)]
    return new, updated, repo_only


def titles(*folders):
    found = set()
    for folder in folders:
        try:
            for name in os.listdir(folder):
                if name.endswith(".json") and TITLE.match(name[:-5].upper()):
                    found.add(name[:-5].upper())
        except OSError:
            pass
    return sorted(found)


def path_for(folder, title):
    return os.path.normpath(os.path.join(folder, f"{title}.json"))


def load_title(folder, title):
    return load(path_for(folder, title)) or empty(title)


def describe(entry):
    ids = " ".join(filter(None, [entry.get("key") and f"key {entry['key']}",
                                 entry.get("hash") and f"hash {entry['hash']}"]))
    return f"{ids}: {entry.get('stage', '?')} {entry.get('object', '(no name)')}"


# --- commands ---------------------------------------------------------------------------------

def command_status(args):
    upload = False
    selected = [args.title] if args.title else titles(args.local, args.repo)
    if not selected:
        print(f"no catalogues in {args.local} or {args.repo}")
        return 0
    for title in selected:
        local, repo = load_title(args.local, title), load_title(args.repo, title)
        new, updated, repo_only = compare(local, repo)
        game = repo.get("game") or local.get("game") or "?"
        print(f"{title} ({game})")
        print(f"  repo  {path_for(args.repo, title)}: {len(repo['programs'])} programs")
        print(f"  local {path_for(args.local, title)}: {len(local['programs'])} programs")
        print(f"  {len(new)} new and {len(updated)} updated locally, {len(repo_only)} only in the repo")
        for entry in (new + updated)[:args.limit]:
            print("    " + ("new " if entry in new else "upd ") + describe(entry))
        if new or updated:
            upload = True
            print(f"  -> run `catalog.py merge`, then commit tools/nx2/catalog/{title}.json")
    return 1 if upload else 0


def command_merge(args):
    selected = [args.title] if args.title else titles(args.local, args.repo)
    for title in selected:
        local, repo = load_title(args.local, title), load_title(args.repo, title)
        new, updated, _ = compare(local, repo)
        merged = merge(repo, local)
        merged["title"] = title
        if new or updated or not os.path.exists(path_for(args.repo, title)):
            if merged["programs"]:
                save(path_for(args.repo, title), merged)
        if not args.repo_only:
            save(path_for(args.local, title), merged)
        print(f"{title}: {len(new)} new, {len(updated)} updated -> {len(merged['programs'])} programs")
    return 0


def command_pull(args):
    for title in [args.title] if args.title else titles(args.repo):
        merged = merge(load_title(args.repo, title), load_title(args.local, title))
        merged["title"] = title
        save(path_for(args.local, title), merged)
        print(f"{title}: local copy has {len(merged['programs'])} programs")
    return 0


def command_add(args):
    title = normalize_title(args.title)
    if not title:
        sys.exit("--title needs the 16-digit program id")
    entry = clean({"key": args.key, "hash": args.hash, "stage": args.stage, "object": args.object,
                   "group": args.group, "how": args.how, "notes": args.notes, "source": args.source,
                   "updated": now()})
    if not entry.get("key") and not entry.get("hash"):
        sys.exit("give --key (native program key) and/or --hash (shader hash), 16 hex digits")
    folder = args.repo if args.to_repo else args.local
    # The local copy always carries the repo's entries too, so it can replace a stale checkout's.
    base = load_title(folder, title) if args.to_repo else merge(load_title(args.repo, title),
                                                                load_title(folder, title))
    catalog = merge(base, {"programs": [entry]})
    catalog["title"] = title
    if args.game:
        catalog["game"] = args.game
    save(path_for(folder, title), catalog)
    print(f"saved to {path_for(folder, title)}: " + describe(find(catalog["programs"],
                                                                 entry.get("key", ""),
                                                                 entry.get("hash", ""))))
    return 0


def command_find(args):
    value = args.id.lower()
    found = False
    for folder, where in ((args.local, "local"), (args.repo, "repo")):
        for title in titles(folder):
            for entry in load_title(folder, title)["programs"]:
                if value in (entry.get("key"), entry.get("hash")):
                    found = True
                    print(f"{where} {title}: {describe(entry)}")
                    for field in ("group", "how", "notes", "source", "updated"):
                        if entry.get(field):
                            print(f"    {field}: {entry[field]}")
    return 0 if found else 1


def command_show(args):
    title = normalize_title(args.title)
    catalog = merge(load_title(args.repo, title), load_title(args.local, title))
    print(f"# {title} {catalog.get('game', '')}\n")
    if catalog.get("notes"):
        print(catalog["notes"] + "\n")
    group = None
    for entry in sorted(catalog["programs"], key=sort_key):
        if entry.get("group", "") != group:
            group = entry.get("group", "")
            print(f"\n## {group or 'Other'}\n")
            print("| Key | Hash | Stage | Object | How | Notes |")
            print("|---|---|---|---|---|---|")
        cells = [entry.get(field, "").replace("|", "/") for field in
                 ("key", "hash", "stage", "object", "how", "notes")]
        print("| " + " | ".join(cells) + " |")
    return 0


def main():
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    parser.add_argument("--local", default=local_dir(), help="local catalogue folder")
    parser.add_argument("--repo", default=REPO_DIR, help="the repo's catalogue folder")
    commands = parser.add_subparsers(dest="command", required=True)

    status = commands.add_parser("status", help="what the local copy has that the repo copy lacks")
    status.add_argument("--title")
    status.add_argument("--limit", type=int, default=20, help="entries to list per game")
    status.set_defaults(func=command_status)

    merge_parser = commands.add_parser("merge", help="bring local entries into the repo copy")
    merge_parser.add_argument("--title")
    merge_parser.add_argument("--repo-only", action="store_true", help="leave the local copy as it is")
    merge_parser.set_defaults(func=command_merge)

    pull = commands.add_parser("pull", help="bring the repo's entries into the local copy")
    pull.add_argument("--title")
    pull.set_defaults(func=command_pull)

    add = commands.add_parser("add", help="add or update one program (local copy by default)")
    add.add_argument("--title", required=True, help="16-digit program id of the game")
    add.add_argument("--game", help="game name, for a new catalogue")
    add.add_argument("--key", help="native program key (shader dump / shader lab key)")
    add.add_argument("--hash", help="shader hash (frame dump, reject_shaders)")
    for field in TEXT_FIELDS:
        add.add_argument(f"--{field}")
    add.add_argument("--to-repo", action="store_true", help="write the repo copy directly")
    add.set_defaults(func=command_add)

    find_parser = commands.add_parser("find", help="look a key or hash up in every catalogue")
    find_parser.add_argument("id")
    find_parser.set_defaults(func=command_find)

    show = commands.add_parser("show", help="print a game's catalogue as Markdown")
    show.add_argument("--title", required=True)
    show.set_defaults(func=command_show)

    args = parser.parse_args()
    if getattr(args, "title", None):
        args.title = normalize_title(args.title) or sys.exit("--title needs the 16-digit program id")
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
