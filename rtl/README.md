# Phase 4 RTL starter

Compile the package before the module:

```bash
verilator --binary --timing --sv -Wall \
  rtl/market_types_pkg.sv rtl/order_book.sv tb/order_book_tb.sv \
  --top-module order_book_tb
./obj_dir/Vorder_book_tb
```

`order_book.sv` currently implements reset, ready/valid event acceptance,
event completion, level storage, and a one-level-per-cycle search scaffold.
The book mutations are deliberately marked `TODO`: implement and test each
operation before adding the next one.
