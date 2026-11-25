# t70pro-lkmenu

A boot menu for the Teclast T70 Pro (MediaTek MT6789), built into the stock
`lk` bootloader stage — in the spirit of lk2nd, but on MediaTek's own LK.

Goal: boot normally by default; press any key during the stock orange-state
warning to get a menu (slot A/B, recovery, fastboot, download mode, SD card,
EDK2/UEFI, power off), with the default kept in `misc`.

**Status:** build pipeline and the first hook (the 5 s warning wait now calls
`t70_orange_wait()`, which still just waits 5 s). Statically checked; not yet
booted on the device.

## How it works

This repository contains no MediaTek code. At build time:

1. `tools/lk_img.py` splits your stock `lk.img` (G3D1 V1.08 only; checked by
   SHA-256) and takes out the `lk` (BL33) payload.
2. `tools/lk_disasm.py` turns that payload into re-assemblable GNU assembler
   source under `out/gen/`: every branch, literal, adrp/`:lo12:` pair, switch
   table and data pointer becomes a symbol, so the code can move and grow.
   Without hooks it rebuilds the stock payload byte for byte (`make check`).
3. `names.txt` gives our names to stock functions and data (linker aliases);
   `hooks.txt` replaces a few stock instructions with calls into `src/`.
4. `build.mk` assembles the generated source, compiles `src/*.c`, links them
   with the generated layout and `tools/lk_img.py` packs `out/lk.img` with the
   other stock stages (`bl2_ext`, `aee`, device trees, certificates) unchanged.

`make relocheck` compares the result with the stock payload: every stock
branch, literal, adrp pair, pointer and jump-table entry must still reach its
(moved) target, and every other stock word must be unchanged.

## Build (Linux, aarch64 cross binutils + gcc, Python 3 with capstone)

    make STOCK_LK_IMG=/path/to/stock/lk.img -j        # -> out/lk.img
    make check STOCK_LK_IMG=/path/to/stock/lk.img     # generated source == stock lk
    make relocheck STOCK_LK_IMG=/path/to/stock/lk.img

Stock `lk.img`: sha256 `18f56d90f25692f8547840b644f405aae7769a97e780acbdf7f076b737c20846`,
`lk` payload sha256 `05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4`.

The image only works on a device whose bootloader is unlocked (the stock
preloader and bl2_ext then accept a changed `lk`). Keep a copy of your stock
`lk_a`/`lk_b` and a way to restore them (preloader/BROM download mode) before
flashing anything.

## License

GPL-3.0-or-later (see `LICENSE`).
