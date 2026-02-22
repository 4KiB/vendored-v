#!/usr/bin/env python3
"""Vendor repositories from .vendor manifests.

This module is intentionally standalone from outer project build wiring.
"""

from __future__ import annotations

import argparse
import fnmatch
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent
DEFAULT_VENDOR_ROOT = REPO_ROOT / "src" / "vendor"


@dataclass
class VendorSpec:
    source: Path
    remote: str
    ref: str
    remove_rules: list[str]
    add_rules: list[str]


def _fatal(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def _run(
    cmd: list[str], cwd: Path | None = None, dry_run: bool = False
) -> None:
    where = f" (cwd={cwd})" if cwd else ""
    print(f"$ {' '.join(cmd)}{where}")
    if dry_run:
        return
    subprocess.run(cmd, cwd=cwd, check=True)


def _sanitize_relative_path(raw: str, *, context: str) -> Path:
    """Validate a repo-local relative path from a .vendor directive.

    >>> _sanitize_relative_path("patch/", context="+patch")
    PosixPath('patch')
    >>> _sanitize_relative_path("nested/file.txt", context="+nested/file.txt")
    PosixPath('nested/file.txt')
    >>> _sanitize_relative_path("../escape", context="+../escape")
    Traceback (most recent call last):
    ...
    SystemExit: 1
    """
    path = Path(raw.rstrip("/"))
    if str(path) in {"", "."}:
        _fatal(f"invalid empty path in {context}")
    if path.is_absolute() or any(part == ".." for part in path.parts):
        _fatal(f"path escapes repository root in {context}: {raw}")
    return path


def parse_vendor_file(vendor_file: Path) -> VendorSpec:
    """Parse a .vendor file into a structured spec.

    >>> from tempfile import TemporaryDirectory
    >>> from pathlib import Path
    >>> with TemporaryDirectory() as td:
    ...     base = Path(td) / "v"
    ...     base.mkdir()
    ...     vf = base / ".vendor"
    ...     _ = vf.write_text("https://example.invalid/repo.git@abc123\\n-test\\n+patch/\\n")
    ...     spec = parse_vendor_file(vf)
    ...     (spec.remote, spec.ref, spec.remove_rules, spec.add_rules, spec.source.name)
    ('https://example.invalid/repo.git', 'abc123', ['test'], ['patch/'], 'v')
    """
    header: str | None = None
    remove_rules: list[str] = []
    add_rules: list[str] = []

    for line_no, raw_line in enumerate(
        vendor_file.read_text().splitlines(), start=1
    ):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue

        if header is None:
            header = line
            continue

        if line.startswith("-"):
            rule = line[1:].strip()
            if not rule:
                _fatal(f"empty remove rule in {vendor_file}:{line_no}")
            remove_rules.append(rule)
            continue

        if line.startswith("+"):
            rule = line[1:].strip()
            if not rule:
                _fatal(f"empty add rule in {vendor_file}:{line_no}")
            add_rules.append(rule)
            continue

        _fatal(f"invalid directive in {vendor_file}:{line_no}: {line}")

    if header is None:
        _fatal(f"missing source@ref header in {vendor_file}")

    if "@" not in header:
        _fatal(f"expected source@ref header in {vendor_file}: {header}")
    remote, ref = header.rsplit("@", 1)
    if not remote or not ref:
        _fatal(f"expected source@ref header in {vendor_file}: {header}")

    return VendorSpec(
        source=vendor_file.parent,
        remote=remote,
        ref=ref,
        remove_rules=remove_rules,
        add_rules=add_rules,
    )


def _remove_path(path: Path, dry_run: bool = False) -> None:
    if not path.exists() and not path.is_symlink():
        return
    print(f"- remove {path}")
    if dry_run:
        return
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    else:
        path.unlink()


def _copy_file(src: Path, dst: Path, dry_run: bool = False) -> None:
    print(f"+ copy {src} -> {dst}")
    if dry_run:
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def _apply_patch_file(
    patch_file: Path, work_dir: Path, dry_run: bool = False
) -> None:
    _run(["git", "apply", str(patch_file)], cwd=work_dir, dry_run=dry_run)


def _copy_tree(src: Path, dst: Path, dry_run: bool = False) -> None:
    for child in src.iterdir():
        if child.name == ".git":
            continue
        target = dst / child.name
        if child.is_dir() and not child.is_symlink():
            if dry_run:
                print(f"+ copytree {child} -> {target}")
                continue
            shutil.copytree(child, target, dirs_exist_ok=True)
        else:
            _copy_file(child, target, dry_run=dry_run)


def _backup_overlay_sources(
    source_dir: Path,
    add_rules: list[str],
    backup_dir: Path,
    dry_run: bool = False,
) -> list[Path]:
    preserved: list[Path] = []
    seen: set[str] = set()
    for rule in add_rules:
        rel = _sanitize_relative_path(rule, context=f"+{rule}")
        key = rel.as_posix()
        if key in seen:
            continue
        seen.add(key)
        src = source_dir / rel
        if src.exists() or src.is_symlink():
            preserved.append(rel)
            if dry_run:
                print(f"~ preserve overlay source {src}")
                continue
            dst = backup_dir / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if src.is_dir() and not src.is_symlink():
                shutil.copytree(src, dst, dirs_exist_ok=True)
            else:
                shutil.copy2(src, dst)
    return preserved


def _restore_overlay_sources(
    source_dir: Path,
    preserved: list[Path],
    backup_dir: Path,
    dry_run: bool = False,
) -> None:
    for rel in preserved:
        src = backup_dir / rel
        dst = source_dir / rel
        if not src.exists() and not src.is_symlink():
            continue
        print(f"~ restore overlay source {dst}")
        if dry_run:
            continue
        if src.is_dir() and not src.is_symlink():
            shutil.copytree(src, dst, dirs_exist_ok=True)
        else:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)


def _replace_vendor_tree(
    source_dir: Path, clone_dir: Path, keep: set[str], dry_run: bool = False
) -> None:
    for child in source_dir.iterdir():
        if child.name in keep:
            continue
        _remove_path(child, dry_run=dry_run)
    _copy_tree(clone_dir, source_dir, dry_run=dry_run)


def _apply_remove_rules(
    source_dir: Path, remove_rules: list[str], dry_run: bool = False
) -> None:
    for rule in remove_rules:
        if "/" in rule:
            rel = _sanitize_relative_path(rule, context=f"-{rule}")
            _remove_path(source_dir / rel, dry_run=dry_run)
            continue

        matches = [
            path
            for path in source_dir.rglob("*")
            if fnmatch.fnmatch(path.name, rule)
        ]
        for match in sorted(
            matches, key=lambda item: len(item.parts), reverse=True
        ):
            _remove_path(match, dry_run=dry_run)


def _apply_add_file(
    source_dir: Path, rule_path: Path, dry_run: bool = False
) -> None:
    src = source_dir / rule_path
    if not src.exists() and not src.is_symlink():
        _fatal(f"add rule path does not exist: +{rule_path.as_posix()}")

    if src.suffix == ".patch":
        patch_cwd = source_dir / rule_path.parent
        _apply_patch_file(src, patch_cwd, dry_run=dry_run)
        return

    if len(rule_path.parts) == 1:
        dst_rel = rule_path
    else:
        dst_rel = rule_path
    _copy_file(src, source_dir / dst_rel, dry_run=dry_run)


def _apply_add_dir(
    source_dir: Path, rule_path: Path, dry_run: bool = False
) -> None:
    src_dir = source_dir / rule_path
    if not src_dir.is_dir():
        _fatal(f"add rule directory does not exist: +{rule_path.as_posix()}")

    for path in sorted(src_dir.rglob("*")):
        if path.is_dir():
            continue
        rel = path.relative_to(src_dir)
        if path.suffix == ".patch":
            patch_cwd = source_dir / rel.parent
            _apply_patch_file(path, patch_cwd, dry_run=dry_run)
            continue
        _copy_file(path, source_dir / rel, dry_run=dry_run)


def _apply_add_rules(
    source_dir: Path, add_rules: list[str], dry_run: bool = False
) -> None:
    for rule in add_rules:
        rel = _sanitize_relative_path(rule, context=f"+{rule}")
        path = source_dir / rel
        if path.is_dir():
            _apply_add_dir(source_dir, rel, dry_run=dry_run)
        else:
            _apply_add_file(source_dir, rel, dry_run=dry_run)


def process_vendor(spec: VendorSpec, dry_run: bool = False) -> None:
    """Run vendoring for one source directory.

    In dry-run mode, this function must not perform any filesystem mutations
    or network operations.

    >>> spec = VendorSpec(
    ...     source=Path('/tmp/not-used'),
    ...     remote='https://example.invalid/repo.git',
    ...     ref='deadbeef',
    ...     remove_rules=['test'],
    ...     add_rules=['patch/'],
    ... )
    >>> process_vendor(spec, dry_run=True)  # doctest: +ELLIPSIS
    === /tmp/not-used ===
    $ git clone https://example.invalid/repo.git <temp>/clone
    $ git checkout deadbeef (cwd=<temp>/clone)
    - remove-rule test
    + add-rule patch/
    ~ dry-run: skipped clone/checkout and all filesystem mutations
    """
    print(f"=== {spec.source} ===")
    if dry_run:
        _run(["git", "clone", spec.remote, "<temp>/clone"], dry_run=True)
        _run(
            ["git", "checkout", spec.ref],
            cwd=Path("<temp>/clone"),
            dry_run=True,
        )
        for rule in spec.remove_rules:
            print(f"- remove-rule {rule}")
        for rule in spec.add_rules:
            print(f"+ add-rule {rule}")
        print("~ dry-run: skipped clone/checkout and all filesystem mutations")
        return

    with tempfile.TemporaryDirectory(prefix="vendor-") as tmp_str:
        tmp = Path(tmp_str)
        clone_dir = tmp / "clone"
        overlays = tmp / "overlay"
        overlays.mkdir(parents=True, exist_ok=True)

        _run(["git", "clone", spec.remote, str(clone_dir)])
        _run(["git", "checkout", spec.ref], cwd=clone_dir)

        preserved = _backup_overlay_sources(
            spec.source, spec.add_rules, overlays, dry_run=dry_run
        )
        _replace_vendor_tree(
            spec.source, clone_dir, keep={".vendor"}, dry_run=dry_run
        )
        _restore_overlay_sources(
            spec.source, preserved, overlays, dry_run=dry_run
        )

        _apply_remove_rules(spec.source, spec.remove_rules, dry_run=dry_run)
        _apply_add_rules(spec.source, spec.add_rules, dry_run=dry_run)
        _remove_path(spec.source / ".git", dry_run=dry_run)


def _find_vendor_sources(vendor_root: Path) -> list[Path]:
    """Find every vendor directory that contains a .vendor file.

    >>> from tempfile import TemporaryDirectory
    >>> with TemporaryDirectory() as td:
    ...     root = Path(td)
    ...     (root / 'a').mkdir()
    ...     (root / 'b').mkdir()
    ...     _ = (root / 'a' / '.vendor').write_text('u@r\\n')
    ...     _ = (root / 'b' / '.vendor').write_text('u@r\\n')
    ...     [p.name for p in _find_vendor_sources(root)]
    ['a', 'b']
    """
    if not vendor_root.is_dir():
        _fatal(f"vendor root not found: {vendor_root}")
    found: list[Path] = []
    for vendor_file in vendor_root.rglob(".vendor"):
        found.append(vendor_file.parent)
    return sorted(found)


def _resolve_targets(vendor_root: Path, targets: list[str]) -> list[Path]:
    """Resolve user targets relative to the vendor root.

    >>> from tempfile import TemporaryDirectory
    >>> with TemporaryDirectory() as td:
    ...     root = Path(td)
    ...     (root / 'v').mkdir()
    ...     _ = (root / 'v' / '.vendor').write_text('u@r\\n')
    ...     [p.name for p in _resolve_targets(root, ['v'])]
    ['v']
    """
    if not targets:
        return _find_vendor_sources(vendor_root)

    resolved: list[Path] = []
    for target in targets:
        candidate = Path(target)
        if not candidate.is_absolute():
            candidate = vendor_root / candidate
        candidate = candidate.resolve()
        vendor_file = candidate / ".vendor"
        if not vendor_file.is_file():
            _fatal(f"missing .vendor file: {vendor_file}")
        resolved.append(candidate)
    return sorted(resolved)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Vendor repositories based on .vendor directives"
    )
    parser.add_argument(
        "targets",
        nargs="*",
        help="Target vendor directories (default: all directories under src/vendor containing .vendor)",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=DEFAULT_VENDOR_ROOT,
        help=f"Vendor root directory (default: {DEFAULT_VENDOR_ROOT})",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print actions without modifying files",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    vendor_root = args.root.resolve()
    targets = _resolve_targets(vendor_root, args.targets)
    if not targets:
        _fatal(f"no .vendor files found under {vendor_root}")

    for target in targets:
        spec = parse_vendor_file(target / ".vendor")
        process_vendor(spec, dry_run=args.dry_run)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
