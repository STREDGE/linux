#!/usr/bin/env python3
"""Вшивает шрифты из third_party/fonts в src/ui/fonts_data.h (массивы байтов).
Запуск из корня репозитория: python3 tools/embed_fonts.py"""
import pathlib

FONTS = [
    ("kFontRussoOne", "third_party/fonts/RussoOne.ttf"),
    ("kFontRubikMedium", "third_party/fonts/Rubik-Medium.ttf"),
    ("kFontMonoBold", "third_party/fonts/JBMono-Bold.ttf"),
]

out = ["// Сгенерировано tools/embed_fonts.py — не редактировать руками.",
       "// Шрифты под SIL Open Font License 1.1 (third_party/fonts/OFL-*.txt),",
       "// урезаны до латиницы и кириллицы.", "#pragma once", "#include <stddef.h>", ""]
for name, path in FONTS:
    data = pathlib.Path(path).read_bytes()
    out.append(f"alignas(4) static const unsigned char {name}[{len(data)}] = {{")
    for i in range(0, len(data), 24):
        out.append("    " + ",".join(str(b) for b in data[i:i + 24]) + ",")
    out.append("};")
    out.append(f"static const size_t {name}Size = {len(data)};")
    out.append("")
pathlib.Path("src/ui/fonts_data.h").write_text("\n".join(out))
print("ok", sum(pathlib.Path(p).stat().st_size for _, p in FONTS), "bytes")
