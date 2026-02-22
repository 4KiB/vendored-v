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


@dataclass
class VendorSpec:
    source: Path
    remote: str
    ref: str
    remove_rules: list[str]
    add_rules: list[str]
    ignore_rules: list[str]


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


def _parse_ignore_rule(rule: str) -> Path:
    if any(ch in rule for ch in "*?[]"):
        _fatal(f"ignore rule does not support globbing: !{rule}")
    return _sanitize_relative_path(rule, context=f"!{rule}")


def _path_matches_ignored(rel: Path, ignored_paths: set[Path]) -> bool:
    return any(
        ignored == rel or ignored in rel.parents for ignored in ignored_paths
    )


def _path_intersects_ignored(rel: Path, ignored_paths: set[Path]) -> bool:
    return any(
        ignored == rel or ignored in rel.parents or rel in ignored.parents
        for ignored in ignored_paths
    )


def _resolve_ignored_paths(ignore_rules: list[str]) -> set[Path]:
    return {_parse_ignore_rule(rule) for rule in ignore_rules}


def parse_vendor_file(vendor_file: Path) -> VendorSpec:
    """Parse a .vendor file into a structured spec.

    >>> from tempfile import TemporaryDirectory
    >>> from pathlib import Path
    >>> with TemporaryDirectory() as td:
    ...     base = Path(td) / "v"
    ...     base.mkdir()
    ...     vf = base / ".vendor"
    ...     _ = vf.write_text("https://example.invalid/repo.git@abc123\\n-test\\n+patch/\\n!keep/\\n")
    ...     spec = parse_vendor_file(vf)
    ...     (spec.remote, spec.ref, spec.remove_rules, spec.add_rules, spec.ignore_rules, spec.source.name)
    ('https://example.invalid/repo.git', 'abc123', ['test'], ['patch/'], ['keep/'], 'v')
    """
    header: str | None = None
    remove_rules: list[str] = []
    add_rules: list[str] = []
    ignore_rules: list[str] = []

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

        if line.startswith("!"):
            rule = line[1:].strip()
            if not rule:
                _fatal(f"empty ignore rule in {vendor_file}:{line_no}")
            _parse_ignore_rule(rule)
            ignore_rules.append(rule)
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
        ignore_rules=ignore_rules,
    )


def _remove_path(
    path: Path,
    dry_run: bool = False,
    *,
    source_dir: Path | None = None,
    ignored_paths: set[Path] | None = None,
) -> None:
    if source_dir is not None and ignored_paths:
        rel = path.relative_to(source_dir)
        if _path_intersects_ignored(rel, ignored_paths):
            print(f"~ skip ignored path {path}")
            return

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


def _copy_tree(
    src: Path,
    dst: Path,
    dry_run: bool = False,
    *,
    root_dst: Path,
    ignored_paths: set[Path],
) -> None:
    for child in src.iterdir():
        if child.name == ".git":
            continue
        target = dst / child.name
        rel = target.relative_to(root_dst)
        if _path_matches_ignored(rel, ignored_paths):
            print(f"~ skip ignored path {target}")
            continue

        if child.is_dir() and not child.is_symlink():
            if not dry_run:
                target.mkdir(parents=True, exist_ok=True)
            _copy_tree(
                child,
                target,
                dry_run=dry_run,
                root_dst=root_dst,
                ignored_paths=ignored_paths,
            )
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
    source_dir: Path,
    clone_dir: Path,
    keep: set[str],
    dry_run: bool = False,
    *,
    ignored_paths: set[Path],
) -> None:
    for child in source_dir.iterdir():
        if child.name in keep:
            continue
        _remove_path(
            child,
            dry_run=dry_run,
            source_dir=source_dir,
            ignored_paths=ignored_paths,
        )
    _copy_tree(
        clone_dir,
        source_dir,
        dry_run=dry_run,
        root_dst=source_dir,
        ignored_paths=ignored_paths,
    )


def _apply_remove_rules(
    source_dir: Path,
    remove_rules: list[str],
    dry_run: bool = False,
    *,
    ignored_paths: set[Path],
) -> None:
    for rule in remove_rules:
        if "/" in rule:
            rel = _sanitize_relative_path(rule, context=f"-{rule}")
            _remove_path(
                source_dir / rel,
                dry_run=dry_run,
                source_dir=source_dir,
                ignored_paths=ignored_paths,
            )
            continue

        matches = [
            path
            for path in source_dir.rglob("*")
            if fnmatch.fnmatch(path.name, rule)
        ]
        for match in sorted(
            matches, key=lambda item: len(item.parts), reverse=True
        ):
            _remove_path(
                match,
                dry_run=dry_run,
                source_dir=source_dir,
                ignored_paths=ignored_paths,
            )


def _apply_add_file(
    source_dir: Path,
    rule_path: Path,
    dry_run: bool = False,
    *,
    ignored_paths: set[Path],
) -> None:
    src = source_dir / rule_path
    if not src.exists() and not src.is_symlink():
        _fatal(f"add rule path does not exist: +{rule_path.as_posix()}")

    if src.suffix == ".patch":
        patch_cwd = source_dir / rule_path.parent
        if _path_matches_ignored(rule_path.parent, ignored_paths):
            print(f"~ skip ignored path {patch_cwd}")
            return
        _apply_patch_file(src, patch_cwd, dry_run=dry_run)
        return

    if len(rule_path.parts) == 1:
        dst_rel = rule_path
    else:
        dst_rel = rule_path
    if _path_matches_ignored(dst_rel, ignored_paths):
        print(f"~ skip ignored path {source_dir / dst_rel}")
        return
    _copy_file(src, source_dir / dst_rel, dry_run=dry_run)


def _apply_add_dir(
    source_dir: Path,
    rule_path: Path,
    dry_run: bool = False,
    *,
    ignored_paths: set[Path],
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
            if _path_matches_ignored(rel.parent, ignored_paths):
                print(f"~ skip ignored path {patch_cwd}")
                continue
            _apply_patch_file(path, patch_cwd, dry_run=dry_run)
            continue
        if _path_matches_ignored(rel, ignored_paths):
            print(f"~ skip ignored path {source_dir / rel}")
            continue
        _copy_file(path, source_dir / rel, dry_run=dry_run)


def _apply_add_rules(
    source_dir: Path,
    add_rules: list[str],
    dry_run: bool = False,
    *,
    ignored_paths: set[Path],
) -> None:
    for rule in add_rules:
        rel = _sanitize_relative_path(rule, context=f"+{rule}")
        path = source_dir / rel
        if path.is_dir():
            _apply_add_dir(
                source_dir,
                rel,
                dry_run=dry_run,
                ignored_paths=ignored_paths,
            )
        else:
            _apply_add_file(
                source_dir,
                rel,
                dry_run=dry_run,
                ignored_paths=ignored_paths,
            )


def _checkout_with_shallow_fallback(clone_dir: Path, ref: str) -> None:
    try:
        _run(["git", "checkout", ref], cwd=clone_dir)
    except subprocess.CalledProcessError:
        try:
            _run(
                ["git", "fetch", "--depth", "1", "origin", ref],
                cwd=clone_dir,
            )
            _run(["git", "checkout", "FETCH_HEAD"], cwd=clone_dir)
        except subprocess.CalledProcessError:
            _run(["git", "fetch", "--unshallow", "origin"], cwd=clone_dir)
            _run(["git", "checkout", ref], cwd=clone_dir)


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
    ...     ignore_rules=['vc/'],
    ... )
    >>> process_vendor(spec, dry_run=True)  # doctest: +ELLIPSIS
    === /tmp/not-used ===
    $ git clone --depth 1 https://example.invalid/repo.git <temp>/clone
    $ git checkout deadbeef (cwd=<temp>/clone)
    - remove-rule test
    + add-rule patch/
    ! ignore-rule vc/
    ~ dry-run: skipped clone/checkout and all filesystem mutations
    """
    print(f"=== {spec.source} ===")
    if dry_run:
        _run(
            ["git", "clone", "--depth", "1", spec.remote, "<temp>/clone"],
            dry_run=True,
        )
        _run(
            ["git", "checkout", spec.ref],
            cwd=Path("<temp>/clone"),
            dry_run=True,
        )
        for rule in spec.remove_rules:
            print(f"- remove-rule {rule}")
        for rule in spec.add_rules:
            print(f"+ add-rule {rule}")
        for rule in spec.ignore_rules:
            print(f"! ignore-rule {rule}")
        print("~ dry-run: skipped clone/checkout and all filesystem mutations")
        return

    with tempfile.TemporaryDirectory(prefix="vendor-") as tmp_str:
        tmp = Path(tmp_str)
        clone_dir = tmp / "clone"
        overlays = tmp / "overlay"
        overlays.mkdir(parents=True, exist_ok=True)

        _run(["git", "clone", "--depth", "1", spec.remote, str(clone_dir)])
        _checkout_with_shallow_fallback(clone_dir, spec.ref)

        ignored_paths = _resolve_ignored_paths(spec.ignore_rules)

        preserved = _backup_overlay_sources(
            spec.source, spec.add_rules, overlays, dry_run=dry_run
        )
        _replace_vendor_tree(
            spec.source,
            clone_dir,
            keep={".vendor"},
            dry_run=dry_run,
            ignored_paths=ignored_paths,
        )
        _restore_overlay_sources(
            spec.source, preserved, overlays, dry_run=dry_run
        )

        _apply_remove_rules(
            spec.source,
            spec.remove_rules,
            dry_run=dry_run,
            ignored_paths=ignored_paths,
        )
        _apply_add_rules(
            spec.source,
            spec.add_rules,
            dry_run=dry_run,
            ignored_paths=ignored_paths,
        )
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
    >>> with TemporaryDirectory() as td:
    ...     root = Path(td)
    ...     _ = (root / '.vendor').write_text('u@r\\n')
    ...     [p.name for p in _find_vendor_sources(root / '.vendor')]  # doctest: +ELLIPSIS
    ['tmp...']
    """
    if vendor_root.is_file():
        if vendor_root.name != ".vendor":
            _fatal(
                f"vendor root must be a directory or .vendor file: {vendor_root}"
            )
        return [vendor_root.parent]

    if not vendor_root.is_dir():
        _fatal(f"vendor root not found: {vendor_root}")

    found: set[Path] = set()
    root_vendor = vendor_root / ".vendor"
    if root_vendor.is_file():
        found.add(vendor_root)
    for vendor_file in vendor_root.rglob(".vendor"):
        found.add(vendor_file.parent)
    return sorted(found)


def _resolve_vendor_source(candidate: Path) -> Path:
    if candidate.is_file():
        if candidate.name != ".vendor":
            _fatal(f"expected .vendor file or directory target: {candidate}")
        return candidate.parent

    vendor_file = candidate / ".vendor"
    if not vendor_file.is_file():
        _fatal(f"missing .vendor file: {vendor_file}")
    return candidate


def _resolve_targets(vendor_root: Path, targets: list[str]) -> list[Path]:
    """Resolve user targets relative to the vendor root.

    >>> from tempfile import TemporaryDirectory
    >>> with TemporaryDirectory() as td:
    ...     root = Path(td)
    ...     (root / 'v').mkdir()
    ...     _ = (root / 'v' / '.vendor').write_text('u@r\\n')
    ...     [p.name for p in _resolve_targets(root, ['v'])]
    ['v']
    >>> with TemporaryDirectory() as td:
    ...     root = Path(td)
    ...     (root / 'v').mkdir()
    ...     _ = (root / 'v' / '.vendor').write_text('u@r\\n')
    ...     [p.name for p in _resolve_targets(root, ['v/.vendor'])]
    ['v']
    """
    if not targets:
        return _find_vendor_sources(vendor_root)

    base_dir = vendor_root if vendor_root.is_dir() else vendor_root.parent
    resolved: list[Path] = []
    for target in targets:
        candidate = Path(target)
        if not candidate.is_absolute():
            candidate = base_dir / candidate
        candidate = candidate.resolve()
        resolved.append(_resolve_vendor_source(candidate))
    return sorted(resolved)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Vendor repositories based on .vendor directives"
    )
    parser.add_argument(
        "targets",
        nargs="*",
        help="Target vendor directories or .vendor files (default: discover all .vendor files under --root)",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=None,
        help="Root directory or .vendor file used for discovery (default: current working directory)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print actions without modifying files",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    vendor_root = (args.root or Path.cwd()).resolve()
    targets = _resolve_targets(vendor_root, args.targets)
    if not targets:
        _fatal(f"no .vendor files found under {vendor_root}")

    for target in targets:
        spec = parse_vendor_file(target / ".vendor")
        process_vendor(spec, dry_run=args.dry_run)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
