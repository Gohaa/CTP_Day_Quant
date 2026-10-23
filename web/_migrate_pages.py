# -*- coding: utf-8 -*-
"""Copy lib HTML pages into Quant_Sev/web/static with path/nav rewrites (ASCII-only source)."""
from __future__ import annotations

import re
from pathlib import Path

LIB = Path(r"C:/Quant_Sev/lib")
OUT = Path(r"C:/Quant_Sev/web/static")
OUT.mkdir(parents=True, exist_ok=True)


def read_text(path: Path) -> str:
    raw = path.read_bytes()
    for enc in ("utf-8-sig", "utf-8", "gbk", "gb18030"):
        try:
            return raw.decode(enc)
        except Exception:
            continue
    return raw.decode("utf-8", errors="replace")


def write_utf8(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8", newline="\n")


def mark_nav(active: str) -> str:
    items = [
        ("/", "QUOTE", "quote"),
        ("/analyze", "ANALYZE", "analyze"),
        ("/equity", "EQUITY", "equity"),
        ("/backtest", "BACKTEST", "backtest"),
    ]
    parts = []
    for href, label, key in items:
        cls = "nav-a on" if key == active else "nav-a"
        parts.append(f'<a class="{cls}" href="{href}">{label}</a>')
    return "\n  ".join(parts)


NAV_CSS = (
    ".nav-a{font-family:var(--mono);font-size:11px;color:var(--dim);text-decoration:none;"
    "padding:5px 10px;border:1px solid var(--line2);border-radius:4px;margin-right:6px}"
    ".nav-a.on,.nav-a:hover{color:var(--amber);border-color:var(--amber2)}"
)


def ensure_nav_css(text: str) -> str:
    if ".nav-a{" in text or ".nav-a {" in text:
        return text
    return text.replace("</style>", NAV_CSS + "</style>")


def patch_quote(text: str) -> str:
    text = text.replace("FUTURES AI", "QUANT_SEV")
    text = re.sub(
        r'(<div class="ndiv"></div>\s*)(?:<a class="nav-a[^>]*>.*?</a>\s*)+',
        r"\1" + mark_nav("quote") + "\n  ",
        text,
        count=1,
        flags=re.S,
    )
    return ensure_nav_css(text)


def patch_equity(text: str) -> str:
    text = text.replace("FUTURES AI", "QUANT_SEV")
    text = re.sub(
        r'<a class="header-back" href="/">[\s\S]*?</a>',
        mark_nav("equity"),
        text,
        count=1,
    )
    return ensure_nav_css(text)


def patch_backtest(text: str) -> str:
    text = text.replace("FUTURES AI", "QUANT_SEV")
    nav = mark_nav("backtest")
    text = re.sub(
        r'<a href="/" style="margin-left:auto">\s*<button class="btn"[^>]*>[\s\S]*?</button>\s*</a>',
        f'<div style="margin-left:auto;display:flex;gap:6px;align-items:center">{nav}</div>',
        text,
        count=1,
    )
    return ensure_nav_css(text)


def patch_index(text: str) -> str:
    text = text.replace("FUTURES AI", "QUANT_SEV")
    nav = mark_nav("analyze")
    text = re.sub(
        r'<a href="/equity"[^>]*>[\s\S]*?</a>\s*<a href="/backtest"[^>]*>[\s\S]*?</a>',
        nav,
        text,
        count=1,
    )
    return ensure_nav_css(text)


def main() -> None:
    jobs = [
        (OUT / "quote.html", OUT / "quote.html", patch_quote),
        (LIB / "equity.html", OUT / "equity.html", patch_equity),
        (LIB / "backtest.html", OUT / "backtest.html", patch_backtest),
        (LIB / "index.html", OUT / "analyze.html", patch_index),
    ]
    for src, dst, patcher in jobs:
        if not src.exists():
            print("SKIP", src)
            continue
        text = patcher(read_text(src))
        write_utf8(dst, text)
        print("wrote", dst.name, dst.stat().st_size)


if __name__ == "__main__":
    main()
