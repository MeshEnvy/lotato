#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import re
import sys


START = "<!-- LOTATO:ROOT_README:START -->"
END = "<!-- LOTATO:ROOT_README:END -->"
NOTICE = "<!-- Auto-generated from /README.md by scripts/readme.py -->"


def build_block(source_text: str) -> str:
    src = source_text.rstrip() + "\n"
    return f"{START}\n{NOTICE}\n\n{src}{END}\n"


def upsert_top_block(target_text: str, block: str) -> str:
    pattern = re.compile(
        rf"^{re.escape(START)}\n.*?\n{re.escape(END)}\n?",
        flags=re.DOTALL,
    )

    if pattern.search(target_text):
        updated = pattern.sub(block, target_text, count=1)
    else:
        updated = block + "\n" + target_text

    return updated


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    source_path = repo_root / "README.md"
    target_paths = [
        repo_root / "meshcore" / "README.md",
        repo_root / "meshtastic" / "README.md",
    ]

    if not source_path.exists():
        print(f"Source README not found: {source_path}", file=sys.stderr)
        return 1

    source_text = source_path.read_text(encoding="utf-8")
    block = build_block(source_text)

    for target_path in target_paths:
        if not target_path.exists():
            print(f"Skip missing target: {target_path}")
            continue

        target_text = target_path.read_text(encoding="utf-8")
        updated_text = upsert_top_block(target_text, block)

        if updated_text != target_text:
            target_path.write_text(updated_text, encoding="utf-8")
            print(f"Updated {target_path.relative_to(repo_root)}")
        else:
            print(f"No change {target_path.relative_to(repo_root)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
