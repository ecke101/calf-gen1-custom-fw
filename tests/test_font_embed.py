from pathlib import Path

import pytest

from calf_fw_tool.font_embed import write_embedded_fonts
from calf_fw_tool.util import FirmwareToolError


def test_write_embedded_fonts_is_deterministic(tmp_path: Path) -> None:
    font = tmp_path / "font.ttf"
    output = tmp_path / "font_data.c"
    font.write_bytes(bytes((0x00, 0x7F, 0x80, 0xFF)))

    write_embedded_fonts(output, (("calf_test_font", font),))

    assert output.read_text(encoding="ascii") == (
        "#include <stddef.h>\n"
        "\n"
        "const unsigned char calf_test_font[] = {\n"
        "    0x00,0x7f,0x80,0xff,\n"
        "};\n"
        "const size_t calf_test_font_size = sizeof(calf_test_font);\n"
    )


@pytest.mark.parametrize("symbol", ("", "has-dash", "9starts_with_digit"))
def test_write_embedded_fonts_rejects_invalid_symbols(
    tmp_path: Path, symbol: str
) -> None:
    font = tmp_path / "font.ttf"
    font.write_bytes(b"font")

    with pytest.raises(FirmwareToolError, match="invalid C font symbol"):
        write_embedded_fonts(tmp_path / "output.c", ((symbol, font),))
