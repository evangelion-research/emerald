"""A language server for Emerald (`.rald`).

See DESIGN.md for the architecture this implements and, more importantly, for
the parts of it that are still missing from the compiler.
"""

from .server import SERVER_NAME, SERVER_VERSION, server

__all__ = ["server", "SERVER_NAME", "SERVER_VERSION"]
__version__ = SERVER_VERSION
