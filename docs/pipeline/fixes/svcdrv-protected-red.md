# svcdrv-protected-red

- **What:** in the Baseline plan list, rows offered by the protected-items override are drawn in the theme's bad colour
  (`ListView::Ink::Bad`, the per-row ink core/ui gained in PR #13). The cell keeps "PROTECTED: <reason>", because ink is
  not used on the selected row or in high contrast. Console already drew them red.
- **Also:** the override checkbox's "on" state was a red square with a dark inner square, which read as an empty box on
  the dark themes. It now draws a check mark with GDI lines (no font glyph needed; Wine's font had no U+2713).
- **Why:** the owner's override spec asked for red protected rows; the earlier PR left Baseline as text only.
- **Checks:** services_check.sh exit 0, selftest 484/0 standalone and suite, goldens unchanged; renders checked.
