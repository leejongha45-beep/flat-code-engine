#!/usr/bin/env python3
"""
FCE CLI — Flat Code Engine command-line interface.

Usage (after pip install):
    fce repl <path> [<path> ...]
    fce index <path> [<path> ...]
    fce query <name> [--depth N] [--path <path> ...]
    fce lookup <name> [--path <path> ...]
    fce search <prefix> [--limit N] [--path <path> ...]
    fce symbols <file> [--path <path> ...]
    fce opencode <name> [--path <path> ...]
"""

from __future__ import annotations

import argparse
import io
import json
import os
import sys
import time

# Force UTF-8 stdout/stderr on Windows (avoids cp949 UnicodeEncodeError)
if sys.platform == "win32":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

import fce

# ---------------------------------------------------------------------------
# ANSI colors
# ---------------------------------------------------------------------------

_USE_COLOR = sys.stdout.isatty()


def _c(code: str, text: str) -> str:
    return f"\033[{code}m{text}\033[0m" if _USE_COLOR else text


def _bold(t: str) -> str:
    return _c("1", t)


def _green(t: str) -> str:
    return _c("32", t)


def _cyan(t: str) -> str:
    return _c("36", t)


def _yellow(t: str) -> str:
    return _c("33", t)


def _dim(t: str) -> str:
    return _c("2", t)


# ---------------------------------------------------------------------------
# File collection
# ---------------------------------------------------------------------------

_EXTENSIONS = {
    ".c", ".h", ".cpp", ".cxx", ".cc", ".hpp", ".hxx",
    ".py",
    ".java",
    ".js", ".mjs", ".cjs", ".jsx",
    ".ts", ".mts", ".tsx",
    ".rs",
    ".go",
    ".cs",
}


def collect_files(paths: list[str]) -> list[str]:
    """Recursively collect source files from paths."""
    files: list[str] = []
    for p in paths:
        p = os.path.abspath(p)
        if os.path.isfile(p):
            files.append(p)
        elif os.path.isdir(p):
            for root, dirs, fnames in os.walk(p):
                dirs[:] = [
                    d for d in dirs
                    if not d.startswith(".")
                    and d not in {"node_modules", "__pycache__", "build", "target", "bin", "obj"}
                ]
                for fn in fnames:
                    ext = os.path.splitext(fn)[1].lower()
                    if ext in _EXTENSIONS:
                        files.append(os.path.join(root, fn))
    return files


# ---------------------------------------------------------------------------
# Engine helper
# ---------------------------------------------------------------------------

def create_engine(paths: list[str]) -> tuple[fce.Engine, list[str], float]:
    """Create engine, index files, return (engine, files, elapsed_seconds)."""
    engine = fce.Engine()
    engine.initialize()

    files = collect_files(paths)
    if not files:
        print("No source files found.")
        engine.release()
        sys.exit(1)

    t0 = time.perf_counter()
    engine.index_files(files)
    elapsed = time.perf_counter() - t0

    return engine, files, elapsed


def print_index_stats(files: list[str], engine: fce.Engine, elapsed: float) -> None:
    """Print indexing statistics."""
    count = engine.symbol_count()
    lang_counts: dict[str, int] = {}
    for f in files:
        ext = os.path.splitext(f)[1].lower()
        lang = {
            ".c": "C", ".h": "C/C++",
            ".cpp": "C++", ".cxx": "C++", ".cc": "C++", ".hpp": "C++", ".hxx": "C++",
            ".py": "Python",
            ".java": "Java",
            ".js": "JavaScript", ".mjs": "JavaScript", ".cjs": "JavaScript", ".jsx": "JavaScript",
            ".ts": "TypeScript", ".mts": "TypeScript", ".tsx": "TypeScript",
            ".rs": "Rust",
            ".go": "Go",
            ".cs": "C#",
        }.get(ext, "Other")
        lang_counts[lang] = lang_counts.get(lang, 0) + 1

    print(f"\n{_bold('Index complete')}")
    print(f"  Files:   {_green(str(len(files)))}")
    print(f"  Symbols: {_green(str(count))}")
    print(f"  Time:    {_green(f'{elapsed:.2f}s')}")
    if lang_counts:
        parts = [f"{lang} {n}" for lang, n in sorted(lang_counts.items(), key=lambda x: -x[1])]
        print(f"  Langs:   {', '.join(parts)}")
    print()


# ---------------------------------------------------------------------------
# Display helpers
# ---------------------------------------------------------------------------

def print_symbol_entry(sym: fce.SymbolEntry, idx: int | None = None) -> None:
    prefix = f"  [{idx}] " if idx is not None else "  "
    kind = _cyan(sym.kind)
    name = _bold(sym.name)
    loc = _dim(f"{sym.file_path}:{sym.start_line}-{sym.end_line}")
    parent = f" ({_dim(sym.parent_class)})" if sym.parent_class else ""
    print(f"{prefix}{kind} {name}{parent}")
    print(f"       {loc}")
    if sym.signature:
        sig = sym.signature
        if len(sig) > 120:
            sig = sig[:117] + "..."
        print(f"       {_dim(sig)}")


def print_related(rel: fce.RelatedSymbol) -> None:
    kind = _cyan(rel.kind)
    name = _bold(rel.name)
    relation = _yellow(str(rel.relation).replace("RelationKind.", ""))
    depth = _dim(f"depth={rel.depth}")
    loc = _dim(f"{rel.file_path}:{rel.start_line}")
    print(f"  {relation} -> {kind} {name}  {depth}  {loc}")
    if rel.signature:
        sig = rel.signature
        if len(sig) > 100:
            sig = sig[:97] + "..."
        print(f"       {_dim(sig)}")


def do_opencode(engine: fce.Engine, name: str) -> None:
    symbols = engine.lookup(name)
    if not symbols:
        print(f"  Symbol '{name}' not found.")
        return

    for i, sym in enumerate(symbols):
        if i > 0:
            print()

        filepath = sym.file_path
        start = sym.start_line
        end = sym.end_line

        print(f"  {_cyan(sym.kind)} {_bold(sym.name)}")
        if sym.parent_class:
            print(f"  parent: {sym.parent_class}")
        print(f"  {_dim(filepath)}:{start}-{end}")

        if not os.path.isfile(filepath):
            print(f"  {_yellow('(file not found)')}")
            continue

        try:
            with open(filepath, "r", encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
        except OSError as e:
            print(f"  {_yellow(f'(cannot read: {e})')}")
            continue

        start = max(1, start)
        end = min(len(lines), end)
        if start > len(lines):
            print(f"  {_yellow('(line range out of bounds)')}")
            continue

        print(f"  {'-' * 60}")
        width = len(str(end))
        for lineno in range(start, end + 1):
            num = _dim(f"{lineno:>{width}} |")
            line = lines[lineno - 1].rstrip("\n\r")
            print(f"  {num} {line}")
        print(f"  {'-' * 60}")


# ---------------------------------------------------------------------------
# Subcommands
# ---------------------------------------------------------------------------

def cmd_index(args: argparse.Namespace) -> None:
    engine, files, elapsed = create_engine(args.path)
    print_index_stats(files, engine, elapsed)
    engine.release()


def cmd_query(args: argparse.Namespace) -> None:
    engine, files, elapsed = create_engine(args.path)
    print_index_stats(files, engine, elapsed)

    t0 = time.perf_counter()
    result = engine.query(args.name, bfs_depth=args.depth)
    qt = (time.perf_counter() - t0) * 1000

    print(f"{_bold('Query')}: {_green(args.name)}  depth={args.depth}  {_dim(f'{qt:.2f}ms')}\n")

    if result.symbols:
        print(f"  {_bold('Direct matches')} ({len(result.symbols)}):")
        for i, sym in enumerate(result.symbols):
            print_symbol_entry(sym, i)
    else:
        print(f"  {_yellow('No direct matches.')}")

    if result.related_symbols:
        print(f"\n  {_bold('Related symbols')} ({len(result.related_symbols)}):")
        for rel in result.related_symbols:
            print_related(rel)

    print()
    engine.release()


def cmd_lookup(args: argparse.Namespace) -> None:
    engine, files, elapsed = create_engine(args.path)
    print_index_stats(files, engine, elapsed)

    symbols = engine.lookup(args.name)
    if symbols:
        print(f"{_bold('Lookup')}: {_green(args.name)}  ({len(symbols)} matches)\n")
        for i, sym in enumerate(symbols):
            print_symbol_entry(sym, i)
    else:
        print(f"  {_yellow(f'Symbol {args.name!r} not found.')}")

    print()
    engine.release()


def cmd_search(args: argparse.Namespace) -> None:
    engine, files, elapsed = create_engine(args.path)
    print_index_stats(files, engine, elapsed)

    raw = engine.search_symbols(args.prefix, limit=args.limit)
    results = json.loads(raw)

    print(f"{_bold('Search')}: {_green(args.prefix)}  limit={args.limit}\n")
    if results:
        for i, entry in enumerate(results):
            kind = _cyan(entry.get("kind", ""))
            name = _bold(entry.get("name", ""))
            loc = _dim(f"{entry.get('file', '')}:{entry.get('line', '')}")
            print(f"  [{i}] {kind} {name}  {loc}")
    else:
        print(f"  {_yellow('No matches.')}")

    print()
    engine.release()


def cmd_symbols(args: argparse.Namespace) -> None:
    engine, files, elapsed = create_engine(args.path)
    print_index_stats(files, engine, elapsed)

    filepath = os.path.abspath(args.file)
    raw = engine.get_file_symbols(filepath)
    symbols = json.loads(raw)

    print(f"{_bold('Symbols in')} {_green(filepath)}\n")
    if symbols:
        for i, entry in enumerate(symbols):
            kind = _cyan(entry.get("kind", ""))
            name = _bold(entry.get("name", ""))
            parent = entry.get("parent", "")
            parent_str = f" ({_dim(parent)})" if parent else ""
            line = entry.get("start_line", "")
            print(f"  [{i:>3}] {kind:<20} {name}{parent_str}  {_dim(f'L{line}')}")
    else:
        print(f"  {_yellow('No symbols found. Check if the file was indexed.')}")

    print()
    engine.release()


def cmd_opencode(args: argparse.Namespace) -> None:
    engine, files, elapsed = create_engine(args.path)
    print_index_stats(files, engine, elapsed)

    print(f"{_bold('OpenCode')}: {_green(args.name)}\n")
    do_opencode(engine, args.name)
    print()
    engine.release()


# ---------------------------------------------------------------------------
# REPL
# ---------------------------------------------------------------------------

def cmd_repl(args: argparse.Namespace) -> None:
    engine, files, elapsed = create_engine(args.path)
    print_index_stats(files, engine, elapsed)

    print(_bold("FCE Interactive REPL"))
    print(_dim("Commands:"))
    print(f"  {_green('/query <name> [depth]')}   BFS graph traversal")
    print(f"  {_green('/lookup <name>')}          Direct symbol lookup")
    print(f"  {_green('/search <prefix>')}        Search by prefix")
    print(f"  {_green('/symbols <file>')}         List symbols in file")
    print(f"  {_green('/opencode <name>')}        View symbol source code")
    print(f"  {_green('/smart <name>')}           Smart query (density eval)")
    print(f"  {_green('/files')}                  List indexed files")
    print(f"  {_green('/stats')}                  Show index stats")
    print(f"  {_green('/quit')}                   Exit")
    print()

    while True:
        try:
            line = input(_bold("fce> ")).strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not line:
            continue

        parts = line.split(maxsplit=2)
        cmd = parts[0].lower()

        if cmd in ("/quit", "/exit", "/q"):
            break

        elif cmd == "/query":
            if len(parts) < 2:
                print("  Usage: /query <name> [depth]")
                continue
            name = parts[1]
            depth = int(parts[2]) if len(parts) > 2 else 2
            t0 = time.perf_counter()
            result = engine.query(name, bfs_depth=depth)
            qt = (time.perf_counter() - t0) * 1000
            print(f"\n  {_bold('Query')}: {_green(name)}  depth={depth}  {_dim(f'{qt:.2f}ms')}\n")
            if result.symbols:
                for i, sym in enumerate(result.symbols):
                    print_symbol_entry(sym, i)
            else:
                print(f"  {_yellow('No direct matches.')}")
            if result.related_symbols:
                print(f"\n  {_bold('Related')} ({len(result.related_symbols)}):")
                for rel in result.related_symbols:
                    print_related(rel)
            print()

        elif cmd == "/lookup":
            if len(parts) < 2:
                print("  Usage: /lookup <name>")
                continue
            symbols = engine.lookup(parts[1])
            if symbols:
                print()
                for i, sym in enumerate(symbols):
                    print_symbol_entry(sym, i)
            else:
                print(f"  {_yellow(f'Not found: {parts[1]}')}")
            print()

        elif cmd == "/search":
            if len(parts) < 2:
                print("  Usage: /search <prefix>")
                continue
            raw = engine.search_symbols(parts[1], limit=20)
            results = json.loads(raw)
            print()
            if results:
                for i, entry in enumerate(results):
                    kind = _cyan(entry.get("kind", ""))
                    name = _bold(entry.get("name", ""))
                    loc = _dim(f"{entry.get('file', '')}:{entry.get('line', '')}")
                    print(f"  [{i}] {kind} {name}  {loc}")
            else:
                print(f"  {_yellow('No matches.')}")
            print()

        elif cmd == "/symbols":
            if len(parts) < 2:
                print("  Usage: /symbols <file>")
                continue
            filepath = os.path.abspath(parts[1])
            raw = engine.get_file_symbols(filepath)
            symbols = json.loads(raw)
            print()
            if symbols:
                for i, entry in enumerate(symbols):
                    kind = _cyan(entry.get("kind", ""))
                    name = _bold(entry.get("name", ""))
                    parent = entry.get("parent", "")
                    parent_str = f" ({_dim(parent)})" if parent else ""
                    line = entry.get("start_line", "")
                    print(f"  [{i:>3}] {kind:<20} {name}{parent_str}  {_dim(f'L{line}')}")
            else:
                print(f"  {_yellow('No symbols found.')}")
            print()

        elif cmd == "/opencode":
            if len(parts) < 2:
                print("  Usage: /opencode <name>")
                continue
            print()
            do_opencode(engine, parts[1])
            print()

        elif cmd == "/smart":
            if len(parts) < 2:
                print("  Usage: /smart <name>")
                continue
            name = parts[1]
            t0 = time.perf_counter()
            result = engine.smart_query(name)
            qt = (time.perf_counter() - t0) * 1000
            d = result.density
            print(f"\n  {_bold('SmartQuery')}: {_green(name)}  {_dim(f'{qt:.2f}ms')}")
            print(f"  Density: score={_green(f'{d.weighted_score:.1f}')}  "
                  f"edge_types={d.edge_type_count}  "
                  f"sufficient={_green('YES') if d.sufficient else _yellow('NO')}  "
                  f"expansions={d.expansions_used}")
            qr = result.data
            if qr.symbols:
                print(f"\n  {_bold('Symbols')} ({len(qr.symbols)}):")
                for i, sym in enumerate(qr.symbols):
                    print_symbol_entry(sym, i)
            if qr.related_symbols:
                print(f"\n  {_bold('Related')} ({len(qr.related_symbols)}):")
                for rel in qr.related_symbols:
                    print_related(rel)
            print()

        elif cmd == "/files":
            raw = engine.get_file_tree()
            file_list = json.loads(raw)
            print(f"\n  {_bold('Indexed files')} ({len(file_list)}):")
            for f in file_list[:50]:
                print(f"  {_dim(f)}")
            if len(file_list) > 50:
                print(f"  {_dim(f'... and {len(file_list) - 50} more')}")
            print()

        elif cmd == "/stats":
            print(f"\n  Symbols: {_green(str(engine.symbol_count()))}")
            raw = engine.get_file_tree()
            file_list = json.loads(raw)
            print(f"  Files:   {_green(str(len(file_list)))}")
            print()

        else:
            print(f"  {_yellow(f'Unknown command: {cmd}')}")
            print(f"  Type {_green('/help')} or see commands above.")
            print()


# ---------------------------------------------------------------------------
# Argument parser
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        prog="fce",
        description="FCE — Flat Code Engine CLI",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    # repl
    p = sub.add_parser("repl", help="Interactive REPL (index once, query many)")
    p.add_argument("path", nargs="+", help="Files or directories to index")

    # index
    p = sub.add_parser("index", help="Index files and show stats")
    p.add_argument("path", nargs="+", help="Files or directories to index")

    # query
    p = sub.add_parser("query", help="BFS graph traversal by symbol name")
    p.add_argument("name", help="Symbol name to query")
    p.add_argument("--depth", type=int, default=2, help="BFS depth (default: 2)")
    p.add_argument("--path", nargs="+", required=True, help="Files or directories to index")

    # lookup
    p = sub.add_parser("lookup", help="Direct symbol lookup")
    p.add_argument("name", help="Symbol name")
    p.add_argument("--path", nargs="+", required=True, help="Files or directories to index")

    # search
    p = sub.add_parser("search", help="Search symbols by prefix")
    p.add_argument("prefix", help="Name prefix to search")
    p.add_argument("--limit", type=int, default=20, help="Max results (default: 20)")
    p.add_argument("--path", nargs="+", required=True, help="Files or directories to index")

    # symbols
    p = sub.add_parser("symbols", help="List symbols in a specific file")
    p.add_argument("file", help="Source file to inspect")
    p.add_argument("--path", nargs="+", default=None, help="Index paths (default: same as file)")

    # opencode
    p = sub.add_parser("opencode", help="View source code for a symbol")
    p.add_argument("name", help="Symbol name")
    p.add_argument("--path", nargs="+", required=True, help="Files or directories to index")

    args = parser.parse_args()

    if args.command == "symbols" and args.path is None:
        args.path = [os.path.dirname(os.path.abspath(args.file)) or "."]

    commands = {
        "repl": cmd_repl,
        "index": cmd_index,
        "query": cmd_query,
        "lookup": cmd_lookup,
        "search": cmd_search,
        "symbols": cmd_symbols,
        "opencode": cmd_opencode,
    }

    commands[args.command](args)


if __name__ == "__main__":
    main()
