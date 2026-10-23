# src/oms

订单管理系统：OrderRef 持久化、限价 GFD、SHFE/INE 平今平昨、发单闸。

```text
quant_sev --config config/live.env --trade-selftest
quant_sev --config config/live.env --oms-dry-run --oms-product SHFE.rb
quant_sev --config config/live.env --oms-roundtrip --enable-trading --oms-product SHFE.rb
```

`--enable-trading` 只作用于本次进程，不改 `live.env`。默认 `ENABLE_TRADING=false`。单笔最多 1 手，须白名单（本次会话写入主力合约）。
