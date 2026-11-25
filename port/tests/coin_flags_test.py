"""The coin producer and the fighter's pickup check use the same bit order."""
from pathlib import Path
import unittest

HEADER = (Path(__file__).resolve().parents[2] /
          "sourceport/extern/melee/src/melee/it/itCharItems.h")


class CoinFlags(unittest.TestCase):
    def test_fighter_side_flags_use_the_disc_layout(self):
        header = HEADER.read_text(encoding="utf-8")
        fields = header.split("typedef struct itUnk4_ItemVars {", 1)[1]
        fields = fields.split("} itUnk4_ItemVars;", 1)[0]
        self.assertRegex(
            fields,
            r"struct\s*\{\s*/\*[^\n]+\*/ u8 xDEC_b0 : 1;"
            r"\s*/\*[^\n]+\*/ u8 xDEC_b1 : 1;\s*\} DISC_STRUCT;"
            r"\s*/\*[^\n]+\*/ Fighter_x1614_t xDF0;",
        )


if __name__ == "__main__":
    unittest.main()
