from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import Sequence

from .util import FirmwareToolError

_C_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def write_embedded_fonts(
    output: Path, fonts: Sequence[tuple[str, Path]]
) -> None:
    """Generate a deterministic C translation unit containing font bytes."""

    if not fonts:
        raise FirmwareToolError("at least one font must be supplied")

    lines = ["#include <stddef.h>", ""]
    for symbol, path in fonts:
        if not _C_IDENTIFIER.fullmatch(symbol):
            raise FirmwareToolError(f"invalid C font symbol: {symbol}")
        if not path.is_file():
            raise FirmwareToolError(f"font file is missing: {path}")
        data = path.read_bytes()
        if not data:
            raise FirmwareToolError(f"font file is empty: {path}")

        lines.append(f"const unsigned char {symbol}[] = {{")
        for offset in range(0, len(data), 12):
            chunk = data[offset : offset + 12]
            lines.append("    " + "".join(f"0x{byte:02x}," for byte in chunk))
        lines.extend(
            (
                "};",
                f"const size_t {symbol}_size = sizeof({symbol});",
                "",
            )
        )

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.write_text("\n".join(lines), encoding="ascii")
    temporary.replace(output)


def _font_argument(value: str) -> tuple[str, Path]:
    symbol, separator, path = value.partition("=")
    if not separator:
        raise argparse.ArgumentTypeError("expected SYMBOL=PATH")
    return symbol, Path(path)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="embed TrueType fonts in C")
    parser.add_argument("output", type=Path)
    parser.add_argument("fonts", nargs="+", type=_font_argument)
    arguments = parser.parse_args(argv)
    write_embedded_fonts(arguments.output, arguments.fonts)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
