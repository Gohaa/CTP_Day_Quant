"""
DEPRECATED — 天勤历史下载已废弃。

请改用极智量化导出 + 本地导入：
  plan/ep-bootstrap.md
  Quant_Sev/tools/ep/VV_SevBootstrap.py
  Quant_Sev/tools/import_ep_to_store.py

本文件仅保留兼容外壳；调用 download() 将直接失败。
"""
from __future__ import annotations

import sys


def download(*_args, **_kwargs):
    raise SystemExit(
        "download_klines.py is deprecated (TqSdk removed). "
        "Use 极智 VV_SevBootstrap + Quant_Sev/tools/import_ep_to_store.py "
        "(see plan/ep-bootstrap.md)."
    )


if __name__ == "__main__":
    print(
        "DEPRECATED: use Quant_Sev/tools/ep + import_ep_to_store.py\n"
        "See plan/ep-bootstrap.md",
        file=sys.stderr,
    )
    raise SystemExit(2)
