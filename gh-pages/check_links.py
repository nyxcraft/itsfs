#!/usr/bin/env python3
"""Fail the build if the site links to something it does not contain.

Every internal href in the built site must resolve to a file that exists, AND every #fragment
must resolve to an id on the page it names. The two ways a doc can legitimately point outside
the site -- an absolute URL, and a repo file rewritten to a GitHub blob URL by build_site.py --
are both absolute and therefore skipped here.

This exists because the failure it catches is silent: a doc is renamed or a section removed,
the site still builds, and the only symptom is a 404 that nobody clicks until much later.

THE FRAGMENT HALF WAS ADDED AFTER IT MISSED ONE. The original stripped `#...` before checking
and skipped same-page links entirely, so a link to a heading that had been RENAMED passed --
which is the commonest way one of these rots, since renaming a heading leaves the file itself
in place. A checker that reports success on something it never looked at is worse than none,
because it is believed. Fragments are matched against the `id=` attributes in the built HTML
rather than against a reimplementation of the slug rules, so what is checked is what the site
actually serves.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "public"


def main() -> int:
    if not ROOT.is_dir():
        print(f"no built site at {ROOT} -- run build_site.py first", file=sys.stderr)
        return 2

    broken: list[tuple[str, str]] = []
    pages = sorted(ROOT.rglob("*.html"))

    # Every id on every page, read once.  `name=` too: older anchors use it.
    ids: dict[Path, set[str]] = {}
    for page in pages:
        html = page.read_text(encoding="utf-8")
        ids[page.resolve()] = set(re.findall(r'(?:id|name)="([^"]+)"', html))

    nfrag = 0
    for page in pages:
        html = page.read_text(encoding="utf-8")
        for href in re.findall(r'(?:href|src)="([^"]+)"', html):
            if "://" in href or href.startswith(("mailto:", "data:")):
                continue

            target, _, frag = href.partition("#")
            if target:
                resolved = (page.parent / target).resolve()
                if not resolved.exists():
                    broken.append((str(page.relative_to(ROOT)), href))
                    continue
            else:
                resolved = page.resolve()      # a same-page #fragment

            if not frag:
                continue

            # Only pages built here can be checked; anything else is out of scope.
            if resolved in ids:
                nfrag += 1
                if frag not in ids[resolved]:
                    broken.append((str(page.relative_to(ROOT)), href))

    if broken:
        print(f"{len(broken)} broken link(s) in the built site:", file=sys.stderr)
        for page, href in broken:
            print(f"  {page}: {href}", file=sys.stderr)
        return 1

    print(f"link check: {len(pages)} pages, no broken internal links "
          f"({nfrag} fragments resolved)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
