"""Read macro inventory rows independently of clang-format line wrapping."""
from pathlib import Path
import re


def macro_rows(path):
    text = Path(path).read_text(encoding='utf-8')
    return [' '.join(m.group().split()) for m in re.finditer(r'^AST_\w+\((?:[^()]|\([^()]*\))*\)', text, re.M)]
