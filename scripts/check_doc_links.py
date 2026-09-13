"""Checks every Markdown cross-reference in this repository.

    python scripts/check_doc_links.py

WHY THIS EXISTS. The documentation is deliberately cross-referenced: the READMEs point at each
other and at the wiring sheets, the wiring sheets point back, and everything points into
AGENTS.md by section. That is what keeps one fact in one place instead of three - but it also
means a renamed file or a reworded heading silently breaks links that nobody reads until they
are needed. A rename of one wiring sheet already invalidated four links in one commit.

Two kinds of reference are verified:

  * file links, resolved relative to the document that contains them,
  * #anchors, matched against the headings actually present in the target document.

The anchor rules approximate the ones GitHub applies: lowercase, drop anything that is not a
letter, digit, space, hyphen or underscore, then turn spaces into hyphens. Diacritics survive,
which matters here because half the documentation is in Polish. The approximation is close
enough to catch a stale link and loose enough not to cry wolf.

Exit status is non-zero when something is broken, so it can be used as a check rather than read.
"""
import re
import sys
import unicodedata
from pathlib import Path

DOCS = [
    "README.md",
    "README.pl.md",
    "AGENTS.md",
    "THIRD-PARTY.md",
    "AGENTS.local.example.md",
    "docs/WIRING-USB.md",
    "docs/WIRING-USB.pl.md",
]

LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
HEADING = re.compile(r"^#{1,6}\s+(.*)")


def slug(heading):
    """Approximate GitHub's anchor generation for a heading."""
    s = heading.strip().lower()
    s = "".join(ch for ch in s if ch.isalnum() or ch in " -_" or unicodedata.combining(ch))
    return s.replace(" ", "-")


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()

    anchors = {}
    for d in DOCS:
        p = root / d
        if p.exists():
            anchors[d] = {
                slug(m.group(1))
                for m in (HEADING.match(line) for line in p.read_text(encoding="utf-8").splitlines())
                if m
            }

    problems = []
    checked = 0

    for d in DOCS:
        p = root / d
        if not p.exists():
            problems.append("missing document: " + d)
            continue

        for target in LINK.findall(p.read_text(encoding="utf-8")):
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            checked += 1
            path_part, _, anchor = target.partition("#")

            if path_part:
                resolved = (p.parent / path_part).resolve()
                if not resolved.exists():
                    problems.append(f"{d}: broken file link -> {target}")
                    continue
                try:
                    rel = resolved.relative_to(root).as_posix()
                except ValueError:
                    rel = None
            else:
                rel = d  # anchor within the same document

            if anchor and rel in anchors and anchor not in anchors[rel]:
                problems.append(f"{d}: anchor not found -> {target}")

    for line in problems:
        print(line)
    print("\nchecked %d links in %d documents, %d problem(s)" % (checked, len(DOCS), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
