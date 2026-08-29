# AGENTS.md — QEMU Apple TV 1st-gen (ATV1) EFI bring-up

Working notes for anyone (human or agent) continuing this project. Read fully before
making changes. This doc is a checkpoint; update it whenever the picture moves.

## Objective

Get the Apple TV 1st-gen (ATV1) **EFI firmware to boot past POST `0xe4`** on a QEMU
fork by emulating the hardware (so the early-PEI DRAM-init pattern writes are not
silently dropped), then use KVM acceleration to finish DRAM init and reach the next
phase. Long-term: full ATV1 boot in QEMU. Do **not** start a coreboot port while still
on QEMU — staying on QEMU first.

## Environment / how to launch

- Repo: `/run/media/CURRENTUSER/Guinea_pig/Projetos/qemu-atv` — a QEMU fork (i686 guest,
  x86_64 host build). Build with: `ninja -C build qemu-system-x86_64`.
- Firmware image: `atv-flash.bin` (2 MB; first bytes zero; last 16 bytes
  `0f 09 e9 1b fc 00 00 00 00 00 00 00 00 00 fd ff`; size 2097152). Byte-identical copy:
  `/run/media/CURRENTUSER/Guinea_pig/appletv.bin`. `atv-flash.bin` is intentionally
  untracked (only `atv-flash.bin.bak`/`.bak2` present) — see `git status`.
- `/dev/kvm` is available. Use KVM for speed.
- Launch command (KVM):
  ```
  nohup ./build/qemu-system-x86_64 \
    -machine pc,car-size=0x40000 -accel kvm -cpu host -m 256 \
    -bios atv-flash.bin -display none \
    -monitor unix:/tmp/mon.sock,server,nowait \
    -serial file:/tmp/serial.out -no-reboot \
    -gdb tcp::1236 >/tmp/qemu.log 2>&1 &
  ```
  Backgrounded with `nohup ... &`. **Avoid `timeout` on the launcher.**
- Helper scripts: `atv-run.sh` (foreground KVM), `q.sh` (one-shot start→query→kill).
  `q.sh`/`atv-run.sh` currently use `-cpu pentium-m` (TCG-friendly); the manual KVM
  command above uses `-cpu host`.

## Tooling & gotchas

- Monitor: `python3 /tmp/mondump.py "info registers"` — UNIX socket `/tmp/mon.sock`.
  `xp/…` disassembly output is noisy with echoed `x` chars — ignore them, read the
  trailing hex/listing lines. `mondump.py` round-trips are expensive under KVM.
- gdb (dedicated stub via `-gdb tcp::1236`): run `set architecture i386:x86-64`.
  Write gdb scripts with the Write tool to `/tmp/*.gdb` (shell escaping of `$eip`
  breaks heredocs). **gdb attach is extremely slow under KVM gdbstub;** avoid
  breakpoints that fire constantly (e.g. a NOT_FOUND return path fires every retry
  and will hang the session — always use a finite timeout), and always `pkill -9 gdb`
  afterward (it leaves the guest stopped until it detaches).
  - Acceptable for sparse breakpoints (e.g. trace the Stall/refresh loop once).
  - Useful gdb registers: use `p/x $pc`; `info registers eip` is INVALID ("Invalid
    register"), use `p/x $eip` or lowercase. `.gdbinit` auto-load warning is harmless.
- Kill QEMU by exact PID via `kill -9 <pid>` (PGIDs observed: 438784, 452118, 478406,
  489904, 515639). Monitor-sampling loops must keep total time under the ~120s/240s
  tool timeouts — use short batches (`for i in $(seq 1 20); do sleep 3; ...`).
- Guest spins at 100% CPU when livelocked (check `/proc/<pid>/stat` utime/stime + top).
- Distinguish states: a *finite* Stall delay completes quickly even at 100% CPU; an
  *infinite* retry loop never leaves the same EIP (symptom of the current blocker).

## Hardware / firmware context

- Apple TV 1st gen = NVIDIA-based board. Firmware DEREF/IBI: the on-die NVRAM/config
  store lives at physical `0xf0000000` (MCHBAR1). The early SEC reads a "top of DRAM"
  byte at off `0x9c` and shifts it left 24 to size DRAM (`0x10 << 24` = 256 MB).
- Real-HW memory map (~244 MB usable, top 16 MB reserved): see
  `/run/media/CURRENTUSER/Guinea_pig/Projetos/atv-devicetree/logs/e820.txt`.
- LPC/TPM: the isa/lpc bus on the real ATV is for the *unpopulated* TPM socket. TPM
  lives at `0xFED40000`, already stubbed `atv-tpm-tis`. The `0xE0000` region is the
  firmware BIOS shadow window — unrelated to TPM.

## Key QEMU type names (VERY IMPORTANT)

- `-machine pc` → concrete type `pc-i440fx-11.1-machine`; the abstract base type name
  is `generic-pc-machine` (`TYPE_PC_MACHINE`, `include/hw/i386/pc.h:128`).
- `OBJECT_DECLARE_TYPE(PCMachineState, PCMachineClass, PC_MACHINE)` at `pc.h:129`.
- So `PC_MACHINE(x86ms)` = `object_dynamic_cast_assert` and works on the concrete
  `pc-i440fx-11.1-machine`. The hardcoded string `"pc-i440fx-machine"` does **NOT**
  match (wrong type name) — that broke the first `atv` gate.
- The whole feature is gated on a QEMU property `car-size` (> 0 selects ATV/NVIDIA
  config). `TypeInfo` for `car_size`/`isa_bios_alias` at `pc.h:56`, `pc.h:116`;
  `PCMachineClass` prop definitions ~`pc.h:73-74`.

## Code changes made (git status: M pc.c, M pc_sysfw.c, M x86-common.c)

### Fix 1 — `hw/i386/pc.c` ~1207-1213 (`pc_memory_init`, option ROM window)
- Stock pc maps `0xc0000-0xdffff` as a **read-only** `pc.rom` option-ROM region,
  silently dropping the ATV DRAM pattern-test writes → verify fails (`0x80000007`) and
  the mem-map retries forever.
- Fix: `if (pcmc->pci_enabled && pcms->car_size == 0) memory_region_set_readonly(option_rom_mr, true);`
  (only lock it read-only for non-ATV). After this, `0xC0000` holds the pattern
  `5a a5 ff 00 b4 4a ff 01` and POST races the whole DRAM-init sub-sequence.

### Fix 2 — `hw/i386/pc_sysfw.c` ~71-84 (`pc_isa_bios_init`)
- Same idea on the pflash path: only `memory_region_set_readonly(isa_bios, true)` when
  `pcms->car_size == 0`. Not active for `-bios` launches; kept for completeness.

### Fix 3 — `hw/i386/x86-common.c` ~1096-1140 (`x86_bios_rom_init`)
- Added `#include "hw/i386/pc.h"`.
- Non-ATV keeps `x86_isa_bios_init(...)`.
- ATV (`car_size > 0`): create a **dedicated writable RAM region** `isa-bios-ram`
  (128 KiB), pre-loaded via `memcpy` with the last 128 KiB of `pc.bios`
  (`memory_region_get_ram_ptr(&x86ms->bios) + bios_size - 128*KiB`), mapped at
  `0xE0000-0xFFFFF` (`1 MiB - 128 KiB`), overlap priority 1.
- Why: the low `0xE0000-0xFFFFF` window must be writable RAM for the DRAM pattern
  test, but making the stock read-only alias writable would let writes to low `0xE0000`
  zero the firmware image still mapped at the top of memory (`0xFFFE0000-0xFFFFFFFF`).
  The dedicated RAM decouples them.
- Verified (mtree): `isa-bios-ram` is `(prio 1, ram)` at `0xE0000-0xFFFFF`; no more
  `alias isa-bios @pc.bios` there; high firmware mapping intact.

### pc_piix.c — ATV NVIDIA GPU shim (already present, gated on `car_size>0`)
- `pc_piix.c:301-302`: `if (pcms->car_size > 0) pci_create_simple(..., TYPE_ATV_NVIDIA_GPU);`
- Shims the embedded NVIDIA G72M ("m63", GeForce Go 7300, vendor 0x10de / device
  0x01d7) with a 16 MiB framebuffer + MMIO so firmware PCI enumeration succeeds.

### pc.c ATV stub / memory-init (`!!` — car_size NVRAM/MCHBAR1/TopDRAM state machine)
Note the diff added an **MCHBAR1 NVRAM rewrite** (see below). Behavior is controlled
by env vars (checked in `pc_machine_done` / `pc_memory_init`):
- `ATV_NVRAM=flash` → load NVRAM window contents from the whole flash dump;
  otherwise (default) present as erased (`0xff`).
- `ATV_TOPDRAM` → override the "top of DRAM" byte; default `0x10` (256 MB).
- `ATV_DEBUG_NVRAM` → verbose logging of NVRAM/MCHBAR1 reads.
- The `0x9c` byte **must be 0x10** (else DRAM sizing underflows and the `0x5a5a5a5a`
  sweep runs forever). The NVRAM window at `0xf0000000` is now an I/O region
  (`atv_mchbar1`) backed by a RAM pointer so reads/writes are observable.

## Current status & the ONE remaining blocker

- **All three fixes verified.** Writes now reach real DRAM: pattern `5a a5 ff 00 b4 4a
  ff 01` confirmed at `0xC0000`, `0xD0000`, `0x1C0000` (1.75 MB), 24 MB, 100 MB
  (`0x06400000`), 128 MB (`0x08000000`). Previously the fill could never cross the low
  1 MB. High-firmware corruption is fully resolved.
- **Current blocker = firmware async-event livelock at POST `0xe4`** (NOT a QEMU
  memory bug). The firmware fills DRAM, then spins forever at 100% CPU waiting for an
  async callback that never fires.

### Exact livelock mechanism (reverse-engineered)
Infinite retry loop at `0x0e51b69e`:
```
0x0e51b69e:  cmpl %esi, 0xe52da44    ; wait-flag (esi=0)
0x0e51b6a4:  je  0x0e51b68d          ; if flag still 0:
0x0e51b68d:  push 0x3e8
0x0e51b697:  calll *0x88(%eax)       ;   EFI Stall(1000µs)  ← the port-0x61 refresh poll
0x0e51b69e:  loop forever
```
- The port `0x61` polling is **EFI `Stall()`**: it reads refresh-clock bit 4 via
  `0x0e9da282`/`0x0e9da25e` (polls `IoRead(0x61)` then `andl $0x10,%eax`). QEMU's
  `hw/audio/pcspk.c:pcspk_io_read` toggles bit 4 every read, so the Stall completes
  fast per call — it is NOT the hang, just the delay inside the wait loop.
- The flag `0xe52da44` is set to 1 only by a **registered async callback** (`0xe51b6e2`),
  registered via the EFI protocol-notify wrapper `0xe526dbf` (`calll *0x34`/`*0x38` on
  protocol at `0xe52da8c`) with a handle from `0xc(%ebp)` and an address `0x200`.
- **The callback never fires under QEMU** → the wait never resolves → 100% CPU forever,
  POST never leaves `0xe4`.
- The `0x8000000e` (`EFI_NOT_FOUND`) from `0x0ea13840` (a generic EFI table lookup
  scanning a table; table base from descriptor `[0x0ea139f4]->+0x44` currently
  `0x0ee4cd90`) is a **red herring** — not the blocker.

### Next step (when resuming)
Identify which device/event the `0xe526dbf` registration is waiting on (the handle
`0xc(%ebp)` at the `0x0e51b6**` caller, and the protocol `0xe52da8c`), then add QEMU
emulation for it (its IRQ, an MCH/timer notification, or the missing device). CRITICAL
gdb warning: do NOT break on the NOT_FOUND return path expecting a single hit — it
fires every retry iteration and will hang gdb/KVM. Prefer reading the live handle +
protocol via the monitor (`xp` on the globals) or a sparse breakpoint with a timeout.

## Reference addresses / data seen (firmware, running from RAM)

- Frozen state while livelocked: `EIP=0x0ea138b8` (`leave; ret` end of the `inb`
  helper `0x0ea138b0`), `EDX=0x61`, `EAX` toggles `0x10/0x20/0x30` (refresh bit 4
  ± timer-2 out bit 5), `EBX=1`, `ESI=0x0ee8aee3`, `EDI=1`, `EBP=0x0ee8aea4`,
  `ESP=0x0ee8aea4`.
- IoRead dispatch `0x0ea13400` is the generic EFI CPU I/O read; `0x0ea138b0-0x0ea138c4`
  are generic `inb/inw/inl/outb` helpers. `0x0d9da25e` reads `port61 & 0x10`.
- Static DRAM walking-pattern reference table at `0x0e53d270` (checked; matches RAM):
  `5a a5 ff 00 b4 4a ff 01 68 95 fe 03 d0 2a fd 07` — the actual compare table is the
  dynamic one referenced by the descriptor, not this static one.
- `hw/audio/pcspk.c:134-151`: `pcspk_io_read` — port 0x61; bit 4 toggles each read,
  bit 5 = `ch.out` (timer 2).

## Repo hygiene & conventions

- Only commit when explicitly asked. `git status` currently shows the 3 modified C
  files plus untracked `atv-flash.bin*` and helper scripts. Do not commit
  `atv-flash.bin` unless asked.
- Follow existing QEMU code style. No comments unless they serve the reverse-engineering
  purpose (this project's comments document the WHY of unusual ATV gating).
- Build: `ninja -C build qemu-system-x86_64`.
