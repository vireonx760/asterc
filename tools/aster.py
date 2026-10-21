#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""AsterASM package/build driver (AsterBuild).

A small, dependency-free package layer around asterc. It keeps AsterASM's
freestanding/cross-compilation model explicit instead of pretending that raw
load images are hosted executables or that a registry/linker already exists.
"""
from __future__ import annotations

import argparse
import dataclasses
import glob
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import textwrap
import time
from typing import Any, Iterable

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover
    print("aster: Python 3.11+ is required (tomllib is part of the standard library)", file=sys.stderr)
    raise SystemExit(2)

TOOL_VERSION = "0.1.0"
MANIFEST_NAME = "aster.toml"


class AsterError(RuntimeError):
    pass


@dataclasses.dataclass
class Dependency:
    alias: str
    path: str | None = None
    git: str | None = None
    rev: str | None = None
    subdir: str | None = None


@dataclasses.dataclass
class Package:
    manifest_path: pathlib.Path
    data: dict[str, Any]
    name: str
    version: str
    kind: str
    root_file: pathlib.Path
    source_roots: list[pathlib.Path]
    dependencies: dict[str, Dependency]
    source: str = "root"
    git_commit: str | None = None

    @property
    def directory(self) -> pathlib.Path:
        return self.manifest_path.parent


@dataclasses.dataclass
class Graph:
    root: Package
    packages: list[Package]  # dependency-first topological order; root last
    edges: dict[pathlib.Path, list[pathlib.Path]]


@dataclasses.dataclass
class BuildConfig:
    target: str
    abi: str | None
    emit: str
    obj_format: str | None
    base: str | None
    features: list[str]
    no_red_zone: bool
    no_promote: bool
    jobs: int | None
    profile: str
    extra_args: list[str]


def eprint(*args: object) -> None:
    print(*args, file=sys.stderr)


def read_toml(path: pathlib.Path) -> dict[str, Any]:
    try:
        with path.open("rb") as f:
            return tomllib.load(f)
    except FileNotFoundError:
        raise AsterError(f"manifest not found: {path}")
    except tomllib.TOMLDecodeError as e:
        raise AsterError(f"invalid TOML in {path}: {e}")


def find_manifest(explicit: str | None) -> pathlib.Path:
    if explicit:
        p = pathlib.Path(explicit).expanduser()
        if p.is_dir():
            p = p / MANIFEST_NAME
        return p.resolve()
    cur = pathlib.Path.cwd().resolve()
    for directory in (cur, *cur.parents):
        p = directory / MANIFEST_NAME
        if p.is_file():
            return p
    raise AsterError(f"could not find {MANIFEST_NAME} in current directory or its parents")


def _as_str_list(value: Any, where: str) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list) or not all(isinstance(x, str) for x in value):
        raise AsterError(f"{where} must be an array of strings")
    return list(value)


def parse_dependencies(data: dict[str, Any], manifest: pathlib.Path) -> dict[str, Dependency]:
    raw = data.get("dependencies", {})
    if raw is None:
        return {}
    if not isinstance(raw, dict):
        raise AsterError(f"[dependencies] must be a table in {manifest}")
    out: dict[str, Dependency] = {}
    for alias, value in raw.items():
        if isinstance(value, str):
            out[alias] = Dependency(alias=alias, path=value)
        elif isinstance(value, dict):
            dep = Dependency(
                alias=alias,
                path=value.get("path"),
                git=value.get("git"),
                rev=value.get("rev"),
                subdir=value.get("subdir"),
            )
            if bool(dep.path) == bool(dep.git):
                raise AsterError(f"dependency '{alias}' in {manifest}: specify exactly one of path or git")
            if dep.git and not dep.rev:
                raise AsterError(
                    f"dependency '{alias}' in {manifest}: git dependencies require 'rev' for reproducible builds"
                )
            out[alias] = dep
        else:
            raise AsterError(f"dependency '{alias}' in {manifest} must be a string or table")
    return out


def load_package(manifest: pathlib.Path, *, source: str = "root", git_commit: str | None = None) -> Package:
    manifest = manifest.resolve()
    data = read_toml(manifest)
    p = data.get("package")
    if not isinstance(p, dict):
        raise AsterError(f"{manifest} has no [package] table")
    name = p.get("name")
    version = p.get("version", "0.0.0")
    kind = p.get("kind", "bin")
    if not isinstance(name, str) or not name.strip():
        raise AsterError(f"{manifest}: package.name must be a non-empty string")
    if not isinstance(version, str):
        raise AsterError(f"{manifest}: package.version must be a string")
    if kind not in {"bin", "lib", "library"}:
        raise AsterError(f"{manifest}: package.kind must be 'bin' or 'lib'")
    kind = "lib" if kind == "library" else kind
    root_default = "src/main.aa" if kind == "bin" else "src/lib.aa"
    root_rel = p.get("root", root_default)
    if not isinstance(root_rel, str):
        raise AsterError(f"{manifest}: package.root must be a string")
    roots_raw = p.get("source-roots", p.get("source_roots", ["src"]))
    roots = _as_str_list(roots_raw, f"{manifest}: package.source-roots")
    if not roots:
        raise AsterError(f"{manifest}: package.source-roots cannot be empty")
    base = manifest.parent
    return Package(
        manifest_path=manifest,
        data=data,
        name=name,
        version=version,
        kind=kind,
        root_file=(base / root_rel).resolve(),
        source_roots=[(base / r).resolve() for r in roots],
        dependencies=parse_dependencies(data, manifest),
        source=source,
        git_commit=git_commit,
    )


def workspace_member_manifests(root_manifest: pathlib.Path) -> list[pathlib.Path]:
    data = read_toml(root_manifest)
    ws = data.get("workspace")
    if not isinstance(ws, dict):
        return []
    members = _as_str_list(ws.get("members", []), f"{root_manifest}: workspace.members")
    excludes = set(_as_str_list(ws.get("exclude", []), f"{root_manifest}: workspace.exclude"))
    out: set[pathlib.Path] = set()
    for pattern in members:
        for match in glob.glob(str(root_manifest.parent / pattern)):
            p = pathlib.Path(match)
            manifest = p if p.name == MANIFEST_NAME else p / MANIFEST_NAME
            rel = os.path.relpath(manifest.parent, root_manifest.parent).replace("\\", "/")
            if rel in excludes:
                continue
            if manifest.is_file():
                out.add(manifest.resolve())
    return sorted(out)


def ensure_git_dependency(dep: Dependency, project_root: pathlib.Path, verbose: bool) -> tuple[pathlib.Path, str]:
    assert dep.git and dep.rev
    git = shutil.which("git")
    if not git:
        raise AsterError(f"dependency '{dep.alias}' requires git, but git is not on PATH")
    key = hashlib.sha256(f"{dep.git}\0{dep.rev}".encode()).hexdigest()[:16]
    checkout = project_root / ".aster" / "deps" / f"{dep.alias}-{key}"
    if not (checkout / ".git").exists():
        checkout.parent.mkdir(parents=True, exist_ok=True)
        if checkout.exists():
            shutil.rmtree(checkout)
        cmd = [git, "clone", "--no-checkout", dep.git, str(checkout)]
        if verbose:
            print("+", " ".join(cmd))
        subprocess.run(cmd, check=True)
    # Fetch the requested revision if it is not already known. This works for commit hashes and refs.
    probe = subprocess.run([git, "-C", str(checkout), "rev-parse", "--verify", f"{dep.rev}^{{commit}}"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    if probe.returncode != 0:
        cmd = [git, "-C", str(checkout), "fetch", "--depth", "1", "origin", dep.rev]
        if verbose:
            print("+", " ".join(cmd))
        subprocess.run(cmd, check=True)
    subprocess.run([git, "-C", str(checkout), "checkout", "--detach", "--force", dep.rev], check=True,
                   stdout=subprocess.DEVNULL)
    commit = subprocess.check_output([git, "-C", str(checkout), "rev-parse", "HEAD"], text=True).strip()
    base = checkout / dep.subdir if dep.subdir else checkout
    manifest = base / MANIFEST_NAME
    if not manifest.is_file():
        raise AsterError(f"git dependency '{dep.alias}' has no {MANIFEST_NAME} at {base}")
    return manifest.resolve(), commit


def resolve_graph(root: Package, *, project_root: pathlib.Path, verbose: bool = False) -> Graph:
    packages_by_path: dict[pathlib.Path, Package] = {}
    edges: dict[pathlib.Path, list[pathlib.Path]] = {}
    visiting: list[pathlib.Path] = []
    ordered: list[Package] = []

    def visit(pkg: Package) -> None:
        key = pkg.manifest_path
        if key in visiting:
            cycle = " -> ".join(str(x) for x in [*visiting[visiting.index(key):], key])
            raise AsterError(f"package dependency cycle: {cycle}")
        if key in packages_by_path:
            return
        visiting.append(key)
        dep_paths: list[pathlib.Path] = []
        for alias in sorted(pkg.dependencies):
            dep = pkg.dependencies[alias]
            if dep.path:
                base = (pkg.directory / dep.path).resolve()
                manifest = base if base.name == MANIFEST_NAME else base / MANIFEST_NAME
                # Preserve a portable, manifest-relative identity in the lockfile even
                # when the dependency lives outside the root package directory.
                source_path = os.path.relpath(base, project_root).replace("\\", "/")
                child = load_package(manifest, source=f"path+{source_path}")
            else:
                manifest, commit = ensure_git_dependency(dep, project_root, verbose)
                child = load_package(manifest, source=f"git+{dep.git}#{commit}", git_commit=commit)
            if child.name != alias:
                # Alias names are allowed, but warn only in verbose mode because module namespace is independent.
                if verbose:
                    print(f"aster: dependency alias '{alias}' resolves to package '{child.name}'")
            visit(child)
            dep_paths.append(child.manifest_path)
        visiting.pop()
        packages_by_path[key] = pkg
        edges[key] = dep_paths
        ordered.append(pkg)

    visit(root)
    return Graph(root=root, packages=ordered, edges=edges)


def all_import_roots(graph: Graph) -> list[pathlib.Path]:
    seen: set[pathlib.Path] = set()
    out: list[pathlib.Path] = []
    for pkg in graph.packages:
        for root in pkg.source_roots:
            if root not in seen:
                seen.add(root)
                out.append(root)
        build = pkg.data.get("build", {})
        if isinstance(build, dict):
            includes = build.get("include", [])
            if isinstance(includes, list):
                for rel in includes:
                    if isinstance(rel, str):
                        p = (pkg.directory / rel).resolve()
                        if p not in seen:
                            seen.add(p)
                            out.append(p)
    return out


def discover_sources(pkg: Package, *, include_tests: bool = False) -> list[pathlib.Path]:
    out: set[pathlib.Path] = set()
    for root in pkg.source_roots:
        if root.is_file() and root.suffix == ".aa":
            out.add(root)
        elif root.is_dir():
            out.update(p.resolve() for p in root.rglob("*.aa") if p.is_file())
    if include_tests:
        td = pkg.directory / "tests"
        if td.is_dir():
            out.update(p.resolve() for p in td.rglob("*.aa") if p.is_file())
    return sorted(out)


def find_compiler(args: argparse.Namespace, pkg: Package | None, script_path: pathlib.Path) -> pathlib.Path:
    candidates: list[pathlib.Path] = []
    explicit = getattr(args, "compiler", None) or os.environ.get("ASTERC")
    if explicit:
        candidates.append(pathlib.Path(explicit).expanduser())
    if pkg:
        tc = pkg.data.get("toolchain", {})
        if isinstance(tc, dict) and isinstance(tc.get("compiler"), str):
            candidates.append((pkg.directory / tc["compiler"]).resolve())
    which = shutil.which("asterc")
    if which:
        candidates.append(pathlib.Path(which))
    repo = script_path.parent.parent
    for rel in ("build/release/asterc", "build/release/asterc.exe", "build/asterc", "build/asterc.exe"):
        candidates.append(repo / rel)
    for p in candidates:
        try:
            rp = p.resolve()
        except OSError:
            continue
        if rp.is_file():
            return rp
    raise AsterError("cannot find asterc; pass --compiler PATH or set ASTERC")


def compiler_version(compiler: pathlib.Path) -> str:
    try:
        return subprocess.check_output([str(compiler), "--version"], text=True, stderr=subprocess.STDOUT).strip()
    except (OSError, subprocess.CalledProcessError) as e:
        raise AsterError(f"cannot execute compiler {compiler}: {e}")


def merge_build_config(pkg: Package, args: argparse.Namespace, overrides: dict[str, Any] | None = None) -> BuildConfig:
    build = pkg.data.get("build", {})
    if not isinstance(build, dict):
        raise AsterError(f"{pkg.manifest_path}: [build] must be a table")
    data = dict(build)
    if overrides:
        data.update({k: v for k, v in overrides.items() if v is not None})
    target = getattr(args, "target", None) or data.get("target") or "x86_64"
    abi = getattr(args, "abi", None) or data.get("abi")
    if abi == "auto":
        abi = None
    emit = getattr(args, "emit", None) or data.get("emit") or "bin"
    obj_format = getattr(args, "obj_format", None) or data.get("format")
    base = getattr(args, "base", None) or data.get("base")
    features = []
    for x in _as_str_list(data.get("features", []), f"{pkg.manifest_path}: build.features") + list(getattr(args, "feature", []) or []):
        if x not in features:
            features.append(x)
    profile = getattr(args, "profile", None) or ("release" if getattr(args, "release", False) else "dev")
    profiles = pkg.data.get("profile", {})
    extra = []
    if isinstance(profiles, dict) and isinstance(profiles.get(profile), dict):
        extra += _as_str_list(profiles[profile].get("compiler-args", []), f"profile.{profile}.compiler-args")
    extra += _as_str_list(data.get("compiler-args", []), f"{pkg.manifest_path}: build.compiler-args")
    extra += list(getattr(args, "compiler_arg", []) or [])
    jobs_value = getattr(args, "jobs", None)
    if jobs_value is None:
        raw_jobs = data.get("jobs")
        jobs_value = int(raw_jobs) if raw_jobs is not None else None
    return BuildConfig(
        target=str(target), abi=str(abi) if abi else None, emit=str(emit),
        obj_format=str(obj_format) if obj_format else None, base=str(base) if base is not None else None,
        features=features,
        no_red_zone=bool(getattr(args, "no_red_zone", False) or data.get("no-red-zone", False)),
        no_promote=bool(getattr(args, "no_promote", False) or data.get("no-promote", False)),
        jobs=jobs_value, profile=profile, extra_args=extra,
    )


def compiler_common_args(cfg: BuildConfig, roots: Iterable[pathlib.Path]) -> list[str]:
    cmd = ["--target", cfg.target]
    if cfg.abi:
        cmd += ["--abi", cfg.abi]
    for feature in cfg.features:
        cmd += ["--feature", feature]
    if cfg.jobs is not None:
        cmd += ["-j", str(cfg.jobs)]
    if cfg.no_red_zone:
        cmd.append("--no-red-zone")
    if cfg.no_promote:
        cmd.append("--no-promote")
    for root in roots:
        cmd += ["-I", str(root)]
    cmd += cfg.extra_args
    return cmd


def run(cmd: list[str], verbose: bool) -> None:
    if verbose:
        print("+", subprocess.list2cmdline(cmd))
    try:
        subprocess.run(cmd, check=True)
    except FileNotFoundError:
        raise AsterError(f"program not found: {cmd[0]}")
    except subprocess.CalledProcessError as e:
        raise AsterError(f"command failed with exit code {e.returncode}: {subprocess.list2cmdline(cmd)}")


def safe_target_dir(target: str) -> str:
    return "".join(c if c.isalnum() or c in "._-" else "_" for c in target)


def artifact_path(pkg: Package, cfg: BuildConfig, override: str | None = None, *, suffix_name: str | None = None) -> pathlib.Path:
    if override:
        return pathlib.Path(override).expanduser().resolve()
    ext = ".bin" if cfg.emit in {"bin", "binary"} else (".obj" if "windows" in cfg.target.lower() else ".o")
    name = suffix_name or pkg.name
    return (pkg.directory / "aster-out" / cfg.profile / safe_target_dir(cfg.target) / f"{name}{ext}").resolve()


def digest_graph(graph: Graph, compiler: pathlib.Path, cfg: BuildConfig, output: pathlib.Path) -> str:
    h = hashlib.sha256()
    h.update(f"aster-build:{TOOL_VERSION}\0{compiler_version(compiler)}\0".encode())
    h.update(json.dumps(dataclasses.asdict(cfg), sort_keys=True).encode())
    h.update(str(output).encode())
    for pkg in graph.packages:
        for p in [pkg.manifest_path, *discover_sources(pkg, include_tests=False)]:
            h.update(str(p).encode())
            h.update(b"\0")
            try:
                h.update(p.read_bytes())
            except FileNotFoundError:
                h.update(b"<missing>")
    return h.hexdigest()


def stamp_path(pkg: Package, cfg: BuildConfig) -> pathlib.Path:
    abi = safe_target_dir(cfg.abi or "auto")
    fmt = safe_target_dir(cfg.obj_format or "auto")
    return pkg.directory / ".aster" / "cache" / f"build-{cfg.profile}-{safe_target_dir(cfg.target)}-{abi}-{cfg.emit}-{fmt}.json"


def build_graph(graph: Graph, compiler: pathlib.Path, cfg: BuildConfig, args: argparse.Namespace, *, output_override: str | None = None, label: str | None = None) -> pathlib.Path:
    roots = all_import_roots(graph)
    if not graph.root.root_file.is_file():
        raise AsterError(f"root source does not exist: {graph.root.root_file}")
    out = artifact_path(graph.root, cfg, output_override or getattr(args, "out", None), suffix_name=label)
    out.parent.mkdir(parents=True, exist_ok=True)
    stamp = stamp_path(graph.root, cfg)
    digest = digest_graph(graph, compiler, cfg, out)
    if not getattr(args, "force", False) and out.is_file() and stamp.is_file():
        try:
            cached = json.loads(stamp.read_text(encoding="utf-8"))
            if cached.get("digest") == digest and cached.get("output") == str(out):
                print(f"Fresh {graph.root.name} -> {out}")
                return out
        except (OSError, json.JSONDecodeError):
            pass

    # Dependency roots are checked as packages before the final root build. AsterASM
    # imports source modules directly, so this is validation, not a fake archive/link step.
    common = compiler_common_args(cfg, roots)
    for dep_pkg in graph.packages[:-1]:
        if dep_pkg.root_file.is_file():
            run([str(compiler), "check", *common, str(dep_pkg.root_file)], getattr(args, "verbose", False))
    cmd = [str(compiler), "build", *common, "--emit", cfg.emit]
    if cfg.obj_format:
        cmd += ["--format", cfg.obj_format]
    if cfg.base is not None:
        cmd += ["--base", cfg.base]
    cmd += ["-o", str(out), str(graph.root.root_file)]
    run(cmd, getattr(args, "verbose", False))
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text(json.dumps({
        "digest": digest, "output": str(out), "compiler": compiler_version(compiler),
        "built_at_unix": int(time.time()), "target": cfg.target, "profile": cfg.profile,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"Built {graph.root.name} -> {out}")
    return out


def check_graph(graph: Graph, compiler: pathlib.Path, cfg: BuildConfig, args: argparse.Namespace) -> int:
    roots = all_import_roots(graph)
    common = compiler_common_args(cfg, roots)
    files: list[pathlib.Path] = []
    if getattr(args, "root_only", False):
        files = [graph.root.root_file]
    else:
        for pkg in graph.packages:
            files.extend(discover_sources(pkg, include_tests=getattr(args, "include_tests", False)))
    seen: set[pathlib.Path] = set()
    files = [p for p in files if not (p in seen or seen.add(p))]
    if not files:
        raise AsterError("no .aa sources found")
    for source in files:
        run([str(compiler), "check", *common, str(source)], getattr(args, "verbose", False))
    print(f"Checked {len(files)} source file(s) across {len(graph.packages)} package(s)")
    return len(files)


def test_graph(graph: Graph, compiler: pathlib.Path, cfg: BuildConfig, args: argparse.Namespace) -> int:
    test_dir = graph.root.directory / "tests"
    tests = sorted(p.resolve() for p in test_dir.rglob("*.aa")) if test_dir.is_dir() else []
    explicit = graph.root.data.get("tests", {})
    if isinstance(explicit, dict) and isinstance(explicit.get("sources"), list):
        tests = [(graph.root.directory / s).resolve() for s in explicit["sources"] if isinstance(s, str)]
    if not tests:
        print("No tests found")
        return 0
    roots = all_import_roots(graph)
    common = compiler_common_args(cfg, roots)
    outdir = graph.root.directory / "aster-out" / cfg.profile / safe_target_dir(cfg.target) / "tests"
    outdir.mkdir(parents=True, exist_ok=True)
    for test in tests:
        if getattr(args, "check_only", False):
            cmd = [str(compiler), "check", *common, str(test)]
        else:
            ext = ".bin" if cfg.emit in {"bin", "binary"} else ".o"
            cmd = [str(compiler), "build", *common, "--emit", cfg.emit]
            if cfg.obj_format:
                cmd += ["--format", cfg.obj_format]
            if cfg.base is not None:
                cmd += ["--base", cfg.base]
            cmd += ["-o", str(outdir / (test.stem + ext)), str(test)]
        run(cmd, getattr(args, "verbose", False))
    mode = "checked" if getattr(args, "check_only", False) else "code-generated"
    print(f"Tests: {len(tests)} target(s) {mode}. Execution is intentionally not implied by raw/object output.")
    return len(tests)


def write_lock(graph: Graph) -> pathlib.Path:
    path = graph.root.directory / "aster.lock"
    lines = ["# Generated by AsterBuild. Do not edit by hand.\n", "lock-version = 1\n"]
    for pkg in graph.packages:
        manifest_hash = hashlib.sha256(pkg.manifest_path.read_bytes()).hexdigest()
        lines += [
            "\n[[package]]\n",
            f"name = {json.dumps(pkg.name)}\n",
            f"version = {json.dumps(pkg.version)}\n",
            f"source = {json.dumps(pkg.source)}\n",
            f"manifest-sha256 = {json.dumps(manifest_hash)}\n",
        ]
        if pkg.git_commit:
            lines.append(f"git-commit = {json.dumps(pkg.git_commit)}\n")
    path.write_text("".join(lines), encoding="utf-8", newline="\n")
    print(f"Wrote {path}")
    return path


def print_tree(graph: Graph) -> None:
    by_path = {p.manifest_path: p for p in graph.packages}
    def rec(path: pathlib.Path, prefix: str, last: bool, seen: set[pathlib.Path]) -> None:
        pkg = by_path[path]
        branch = "└── " if last else "├── "
        print(prefix + branch + f"{pkg.name} {pkg.version} [{pkg.source}]")
        if path in seen:
            print(prefix + ("    " if last else "│   ") + "└── (already shown)")
            return
        seen.add(path)
        children = graph.edges.get(path, [])
        for idx, child in enumerate(children):
            rec(child, prefix + ("    " if last else "│   "), idx == len(children) - 1, seen)
    print(f"{graph.root.name} {graph.root.version}")
    children = graph.edges.get(graph.root.manifest_path, [])
    seen = {graph.root.manifest_path}
    for idx, child in enumerate(children):
        rec(child, "", idx == len(children) - 1, seen)


def metadata(graph: Graph) -> dict[str, Any]:
    return {
        "tool_version": TOOL_VERSION,
        "root": str(graph.root.manifest_path),
        "packages": [
            {
                "name": p.name, "version": p.version, "kind": p.kind,
                "manifest": str(p.manifest_path), "root_file": str(p.root_file),
                "source_roots": [str(x) for x in p.source_roots], "source": p.source,
                "dependencies": [str(x) for x in graph.edges.get(p.manifest_path, [])],
            } for p in graph.packages
        ],
    }


def format_graph(graph: Graph, args: argparse.Namespace, script_path: pathlib.Path) -> None:
    formatter = script_path.with_name("asterfmt.py")
    if not formatter.is_file():
        raise AsterError(f"formatter not found next to package tool: {formatter}")
    targets: set[pathlib.Path] = set()
    packages = graph.packages if getattr(args, "all", False) else [graph.root]
    for pkg in packages:
        targets.update(discover_sources(pkg, include_tests=True))
    cmd = [sys.executable, str(formatter)]
    if getattr(args, "check", False):
        cmd.append("--check")
    if getattr(args, "diff", False):
        cmd.append("--diff")
    cmd += [str(x) for x in sorted(targets)]
    run(cmd, getattr(args, "verbose", False))


def create_project(path: pathlib.Path, name: str, kind: str, force: bool) -> None:
    if path.exists() and any(path.iterdir()) and not force:
        raise AsterError(f"directory is not empty: {path} (use --force to initialize anyway)")
    path.mkdir(parents=True, exist_ok=True)
    src = path / "src"
    src.mkdir(exist_ok=True)
    module_name = "".join(c if c.isalnum() or c == "_" else "_" for c in name)
    root_rel = f"src/{module_name}.aa" if kind == "lib" else "src/main.aa"
    root = path / root_rel
    manifest = textwrap.dedent(f"""\
        [package]
        name = {json.dumps(name)}
        version = "0.1.0"
        kind = {json.dumps(kind)}
        root = {json.dumps(root_rel)}
        source-roots = ["src"]

        [build]
        target = "x86_64"
        emit = "bin"
        features = []

        [profile.dev]
        compiler-args = []

        [profile.release]
        compiler-args = []
    """)
    (path / MANIFEST_NAME).write_text(manifest, encoding="utf-8", newline="\n")
    if not root.exists() or force:
        if kind == "lib":
            code = f"module {module_name};\n\nexport fn answer() -> u32 @abi(c) {{\n    return 42;\n}}\n"
        else:
            code = f"module {module_name};\n\nexport fn entry() -> u32 @abi(c) {{\n    return 0;\n}}\n"
        root.write_text(code, encoding="utf-8", newline="\n")
    gi = path / ".gitignore"
    if not gi.exists():
        gi.write_text("/.aster/\n/aster-out/\n", encoding="utf-8", newline="\n")
    print(f"Created {kind} package '{name}' at {path}")


def add_common_build_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("--target")
    p.add_argument("--abi")
    p.add_argument("--emit", choices=["bin", "obj"])
    p.add_argument("--format", dest="obj_format", choices=["elf", "coff", "macho"])
    p.add_argument("--base")
    p.add_argument("--feature", action="append", default=[])
    p.add_argument("-j", "--jobs", type=int)
    p.add_argument("--no-red-zone", action="store_true")
    p.add_argument("--no-promote", action="store_true")
    p.add_argument("--release", action="store_true")
    p.add_argument("--profile")
    p.add_argument("--compiler-arg", action="append", default=[])


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="aster", description="AsterASM package/build driver")
    ap.add_argument("--version", action="version", version=f"aster {TOOL_VERSION}")
    ap.add_argument("--manifest-path")
    ap.add_argument("--compiler", help="path to asterc (or set ASTERC)")
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("new", help="create a new package")
    p.add_argument("path")
    p.add_argument("--name")
    p.add_argument("--lib", action="store_true")
    p.add_argument("--force", action="store_true")

    p = sub.add_parser("init", help="initialize the current directory")
    p.add_argument("--name")
    p.add_argument("--lib", action="store_true")
    p.add_argument("--force", action="store_true")

    p = sub.add_parser("build", help="build the root package")
    add_common_build_args(p)
    p.add_argument("-o", "--out")
    p.add_argument("--force", action="store_true")

    p = sub.add_parser("check", help="semantic-check package sources")
    add_common_build_args(p)
    p.add_argument("--root-only", action="store_true")
    p.add_argument("--include-tests", action="store_true")

    p = sub.add_parser("test", help="check/codegen tests without pretending to execute raw images")
    add_common_build_args(p)
    p.add_argument("--check-only", action="store_true")

    p = sub.add_parser("matrix", help="build every [[matrix]] cross-target entry")
    add_common_build_args(p)
    p.add_argument("--force", action="store_true")

    p = sub.add_parser("fmt", help="format package AsterASM sources")
    p.add_argument("--check", action="store_true")
    p.add_argument("--diff", action="store_true")
    p.add_argument("--all", action="store_true", help="also format path/git dependencies")

    sub.add_parser("tree", help="show dependency tree")
    sub.add_parser("metadata", help="print package graph as JSON")
    sub.add_parser("lock", help="resolve dependencies and write aster.lock")

    p = sub.add_parser("clean", help="remove package outputs/cache")
    p.add_argument("--cache-only", action="store_true")
    p.add_argument("--outputs-only", action="store_true")

    sub.add_parser("doctor", help="check manifest/compiler/tool prerequisites")
    return ap


def main(argv: list[str] | None = None) -> int:
    ap = parser()
    args = ap.parse_args(argv)
    script = pathlib.Path(__file__).resolve()
    try:
        if args.command in {"new", "init"}:
            path = pathlib.Path(args.path).expanduser().resolve() if args.command == "new" else pathlib.Path.cwd().resolve()
            name = args.name or path.name
            create_project(path, name, "lib" if args.lib else "bin", args.force)
            return 0

        manifest = find_manifest(args.manifest_path)
        data = read_toml(manifest)
        if "package" not in data:
            members = workspace_member_manifests(manifest)
            if not members:
                raise AsterError(f"{manifest} is a virtual workspace with no members")
            # For now virtual workspaces execute package commands member-by-member.
            if args.command in {"tree", "metadata", "lock", "doctor", "clean", "fmt", "check", "build", "test", "matrix"}:
                rc = 0
                for member in members:
                    child_args = list(argv if argv is not None else sys.argv[1:])
                    # Replace/insert manifest path for recursive call.
                    filtered: list[str] = []
                    skip = False
                    for idx, item in enumerate(child_args):
                        if skip:
                            skip = False
                            continue
                        if item == "--manifest-path":
                            skip = True
                            continue
                        filtered.append(item)
                    return_code = main(["--manifest-path", str(member), *filtered])
                    rc = max(rc, return_code)
                return rc

        root = load_package(manifest)
        graph = resolve_graph(root, project_root=root.directory, verbose=args.verbose)

        if args.command == "clean":
            if not args.outputs_only:
                shutil.rmtree(root.directory / ".aster", ignore_errors=True)
            if not args.cache_only:
                shutil.rmtree(root.directory / "aster-out", ignore_errors=True)
            print(f"Cleaned {root.name}")
            return 0
        if args.command == "tree":
            print_tree(graph)
            return 0
        if args.command == "metadata":
            print(json.dumps(metadata(graph), indent=2))
            return 0
        if args.command == "lock":
            write_lock(graph)
            return 0
        if args.command == "fmt":
            format_graph(graph, args, script)
            return 0

        compiler = find_compiler(args, root, script)
        if args.command == "doctor":
            print(f"AsterBuild {TOOL_VERSION}")
            print(f"manifest: {manifest}")
            print(f"package: {root.name} {root.version} ({root.kind})")
            print(f"python: {sys.version.split()[0]}")
            print(f"compiler: {compiler} [{compiler_version(compiler)}]")
            print(f"git: {shutil.which('git') or 'not found (only needed for git dependencies)'}")
            print(f"packages in graph: {len(graph.packages)}")
            return 0

        cfg = merge_build_config(root, args)
        if args.command == "build":
            build_graph(graph, compiler, cfg, args)
            return 0
        if args.command == "check":
            check_graph(graph, compiler, cfg, args)
            return 0
        if args.command == "test":
            test_graph(graph, compiler, cfg, args)
            return 0
        if args.command == "matrix":
            entries = root.data.get("matrix", [])
            if not isinstance(entries, list) or not entries:
                raise AsterError(f"{manifest}: add one or more [[matrix]] entries")
            built = 0
            for idx, entry in enumerate(entries):
                if not isinstance(entry, dict):
                    raise AsterError(f"{manifest}: matrix entry {idx} must be a table")
                name = str(entry.get("name", f"target-{idx+1}"))
                cfg_i = merge_build_config(root, args, entry)
                out = root.directory / "aster-out" / "matrix" / name / (root.name + (".bin" if cfg_i.emit == "bin" else ".o"))
                build_graph(graph, compiler, cfg_i, args, output_override=str(out), label=None)
                built += 1
            print(f"Matrix built {built} target configuration(s)")
            return 0
        raise AsterError(f"unsupported command: {args.command}")
    except AsterError as e:
        eprint(f"aster: error: {e}")
        return 2
    except subprocess.CalledProcessError as e:
        eprint(f"aster: external command failed: {e}")
        return e.returncode or 2


if __name__ == "__main__":
    raise SystemExit(main())
