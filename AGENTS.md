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

### Refined diagnosis (`!!` — DXE async protocol-notification wait)
The livelock is an **EFI DXE driver/protocol-connection wait**, not the DRAM verify.
- Function F at `0x0e51b5ff` (called from `0x0e51b3a0`) registers an **async protocol
  notification** via wrapper `0xe526dbf` (which itself does `calll *0x34`/`*0x38` on the
  protocol instance) for an **opaque EFI handle token** `0x11e1a300` (NOT a parseable
  RAM pointer — monitor `xp` on it returns "Cannot access memory"). Callback
  `0xe51b6e2` sets wait-flag `0xe52da44`=1; it never fires → F busy-waits at
  `0x0e51b69e` on `Stall(1000)` forever.
- The platform protocol instance is `[0xe52da8c]` = `0x0ee55aa0` — a **large custom/p
  platform protocol** with a method table reaching offset `0xc0` (Stall lives at `0x88`;
  also uses `0x30/0x34/0x38/0xbc/0xc0`). Not the plain EFI_CPU_ARCH_PROTOCOL (too many
  methods); likely Apple/MCP-platform-specific. Live stack frame: `[0x0ee8af24]`=saved
  EBP `0x0ee8af68`, ret `0x0e51b3a0`, `8(%ebp)`=`0x0ee8af5c`, `0xc(%ebp)`=handle
  `0x11e1a300`.
- **NOT an interrupt wait (`!!`)**: live registers at the frozen loop show
  `EFL=00000002` (**IF=0, interrupts disabled**) and `IDT=00000000 0000ffff` (no real
  IDT). So no interrupt/timer event can fire to advance the wait. And during the wait the
  firmware touches ONLY port `0x61` (Stall delay) plus the in-RAM flag — no other port or
  MMIO poll. → The awaited event is a **synchronous completion callback from a missing
  DXE driver installing a protocol**, not an IRQ to add.
- **Apple EFI context**: near the connection code, ROM globals `0x0e52d710-0x0e52d760`
  hold Apple boot-loader GUIDs — the ASCII strings `427 6f6f 74` ("Boot"), `5265 636f`
  ("Reco…"), `4846 5300` ("HFS"), etc. So this is the standard Apple EFI boot/loader
  driver dispatch; the busy-wait is part of Apple's DXE driver connection that on real
  HW completes because the matched ATV device/driver is present.
- **Root cause of the hang**: a platform/device DXE driver never installs the awaited
  protocol because the corresponding ATV1 hardware is absent under QEMU's generic `pc`
  machine (no MCP/memory-sync notification, missing device, or the matching Apple
  driver never binds to a present device).

### CONFIRMED via RAM image (`!!` — module identity + awaited protocol)
Using `pmemsave 0x0e480000 0x100000 /tmp/atv-ram.bin` (written to the QEMU **cwd**,
e.g. `qemu-atv/ramdump.bin`, NOT /tmp) I found the MZ/PE header of the busy-wait
module and decoded function F end-to-end:
- **Module base = `0x0e51b000`** (MZ@dump-off `0x9b000`; ImageBase field `0xe51b000`;
  PE32, `.text/.rdata/.data/.reloc`, machine 0x14c). The busy-wait `0x0e51b69e` sits at
  module offset `0x69e`. Recognized strings inside: `Primary Master %s`,
  `Apple_Recovery`, `BootFFFF`, `Boot0080` → this is an **Apple BDS / boot-picker
  driver** (boot-device / filesystem selection), not a plain CPU/mem driver.
- **Protocol GUIDs the driver registers/waits against** (read from RAM globals):
  `0xe52d794` = `964E5B22-6459-11D2-8E39-00A0C969723B` = **EFI_SIMPLE_FILE_SYSTEM**
  (EFI-1.x `964e5b21/22` family; `964e5b21`=BLOCK_IO). `0xe52d754` =
  `8BE4DF61-93CA-11D2-AA0D-00E098032B8C` = **EFI_GLOBAL_VARIABLE** (standard var ns).
  `0xe52d5c4`=`E121EC07-9C42-45EE-B0B6-FFF8EF03C521`, `0xe52d614`=
  `35D15D9C-535B-4FCA-A536-2A4E9BE8FBA0` = Apple-custom GUIDs. `0xe52d709+` holds the
  ASCII "Boot"/"HFS"/"Recovery" strings.
- Function F (`0x0e51b5ff`) decoded:
  ```
  call 0xe526dbf(cb=0xe51b6e2, h=0x11e1a300, ...)  ; wrapper -> prot(*0x34), prot(*0x38)
  prot=0xe52da8c: call *0x34(push 0x200,0x8,0xe51b6f6,0,&key)   ; cb->flag=2
                   call *0x60(GUID 0xe52d794=SimpleFileSystem, h, &h2)
  call 0xe51bc4c(0x8(%ebp)) -> 0x8000000e (NOT_FOUND)
  wait@0x0e51b69e: while (0xe52da44==0) { prot->Stall(0x3e8) }   ; loop forever
  ```
  Callback `0xe51b6e2` sets flag=1; callback `0xe51b6f6` sets flag=2. **Both are async
  protocol/event notify callbacks that never fire.** No IRQ, no other port/MMIO polled.
- ⇒ **The driver awaits an install of EFI_SIMPLE_FILE_SYSTEM (a bootable filesystem
  device), or the platform `*0x34/*0x38/*0x60` notify to fire.** On real ATV a boot
  device (internal ATA/FireWire/USB disk with HFS) + its FS driver appear; under QEMU's
  generic `pc` none does, so the notify never fires and BDS spins at POST `0xe4`.
- Platform protocol instance `[0xe52da8c]` = `0x0ee55aa0`: method table offsets used
  `*0x30`(free?) `*0x34` `*0x38`(reg-notify) `*0x44`(free) `*0x4c` `*0x60`(open-install
  by GUID) `*0x88`(Stall) `*0xac`(read cfg struct) `*0xbc`(get next) `*0xc0`(read mem
  block). It wraps the EFI protocol database with an Apple async-notify layer.

### Next step (when resuming)
Goal is now concrete: make the awaited EFI_SIMPLE_FILE_SYSTEM / boot-device notify fire
(or bypass the wait). Ideas in priority order:
1. **Add a bootable block device** to the QEMU `pc` machine (e.g. an IDE/i440FX ATA disk
   on the ATV stub, or a USB/FireWire virtual disk) + the matching EFI FS driver, so BDS
   sees a filesystem. Simplest first try: attach a small HFS/EFI-partitioned virtio/ATA
   disk and see if BDS proceeds past `0xe4`.
2. If a disk alone doesn't bind (Apple driver looks for a specific ATV device path),
   instrument `pc_memory_init`/ATV stub (`pc.c`) to **log the platform `*0x34/*0x60`
   protocol-registration GUIDs live** (the `ATV_DEBUG_NVRAM` style knob) to confirm the
   exact awaited GUID right at freeze, then emulate that device's "ready" signal.
3. Failing that, patch the firmware's wait (`0xe52da44` init to non-zero) only as an
   experiment to see how far it gets — do NOT rely on it as the fix.

### Next step (when resuming)
Narrow which ATV1 **device/driver** the DXE connection is waiting for, then add QEMU
emulation for it (its IRQ / a memory-sync / the missing device). Concrete leads:
- The registrations build on handle `0x11e1a300` and protocol `0x0ee55aa0`; trace
  F's caller `0x0e51b3a0` and the `0x0e51b2e0` driver-connection routine (uses globals
  `0xe52da4c`/`0xe52da90`, `*0x38`/`*0xc0`) to find the expected GUID. Read that GUID
  by dumping the protocol database / the args pushed at the registration sites
  (`0x0e51b613`, `0x0e51b73d`).
- gdb-under-KVM is too slow to catch the registration (it races to the loop). Under
  **TCG** (`-accel tcg -cpu pentium-m`) gdbstub is faster; still race-prone. Prefer
  reading the live handle/protocol/GUID via the monitor, or instrument QEMU to log the
  registration. AGENTS warns: do NOT break on the NOT_FOUND return path (fires every
  retry, hangs gdb/KVM).

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
