"""Entry point: `emerald-lsp` over stdio, or `--tcp` for debugging a client."""

from __future__ import annotations

import argparse
import logging
import sys

from . import compiler
from .server import SERVER_NAME, SERVER_VERSION, server


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog=SERVER_NAME, description=__doc__)
    parser.add_argument("--version", action="version", version=SERVER_VERSION)
    parser.add_argument("--tcp", action="store_true", help="listen on TCP instead of stdio")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2087)
    parser.add_argument("--log-file", help="write server logs here (never stdout)")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument(
        "--check",
        metavar="FILE",
        help="run the compiler's checker once and print its diagnostics, "
        "the same query the server makes per keystroke",
    )
    args = parser.parse_args(argv)

    # stdout is the protocol channel; logs must never land there
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(levelname)s %(name)s: %(message)s",
        **({"filename": args.log_file} if args.log_file else {"stream": sys.stderr}),
    )

    if args.check:
        result = compiler.check(args.check, None, compiler.Settings())
        if not result.ok:
            print(result.detail, file=sys.stderr)
            return 2
        for diag in result.diagnostics:
            print(
                f"{diag.get('file')}:{diag.get('line')}:{diag.get('column')}: "
                f"{diag.get('severity')}[{diag.get('code')}] {diag.get('message')}"
            )
        return 1 if result.diagnostics else 0

    if args.tcp:
        server.start_tcp(args.host, args.port)
    else:
        server.start_io()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
