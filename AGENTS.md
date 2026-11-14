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

### Resumed 2026-08-29 — phase-4 (`0x0ee4d4e0` spin) & RUNTSERV engine fully mapped
`!!` NOTE: the current blocker is a NEW, later wait (POST `0xe6` region) — the
**IBI/C9DCF469/RUNTSERV** dead-wait at `0x0ee4d4e0`, distinct from the phases below.
Key facts established this session (all RAM reads, live QEMU pid 1215586):

- **Phase-4 function `0x0ee4d260`** (IBI module base `0x0ee4d000`, ONLY data ref to it =
  slot `0xee55c90`, module's own "last-run" marker). Frame: `EBP=0x0ee8aff0`,
  `[ebp]=0x0ebbfd18` (context block: 8 pointers `0x100017b0`/`0x100017bc` into the m63
  VBIOS imagebase w/ params 8,0xa), **`[ebp+4]=0` (return address is ZERO → a plain ret
  after flag-clear TRIPLE-FAULTS — poking flag alone is unsafe)**. `[ebp+8]=1` spin flag;
  `ESI=0`, `ECX=0xe52d701` (Apple GUID/str region) during spin. EFL=0x2 (IF=0), **no
  IDT/paging: CR0=0x13 (32-bit PM, PG off), CR3=0, CR4=0x600** → linear 0 = RAM (we
  wrote/restored `90`s there; a trampoline at 0x0 IS viable).
- Post-spin code is just `pop/pop/pop; leave; ret` — no inline continuation; the real
  completion must (a) clear `[ebp+8]` AND (b) rewrite `[ebp+4]` with the continuation.
- **Spin is a pure `cmp/jne` loop (no calls, no port I/O)** ⇒ NOTHING can advance it
  from within; on real HW an SMM/NMI/other-context write (no IRQ, no IDT) must service it.
- **Tail sequence**: first `call *0x44` on RUNTSERV (`[0xee55c60]=0x0eeffd90`,
  `[+0x44]=0x0e9e82e1`) args `(1, 0x3041000, 0, &C9DCF469, 0)`; then 5-6 quick GUID
  lookups `0xee4d4f0(GUID,&out)`; ALL returned <0 ⇒ async tail: second `*0x44`
  `(1, 0x3041001, 0, &C9DCF469, 0)`; `call *(%eax)` with `[0xee569ac]=0x0e528358` →
  `[0x0e528358]=0x0e51bca1` (DuetBds); `mov edi,0x8(%ebp)`; spin@0x0ee4d4e0.
- **C9DCF469 = Apple EFI variable namespace GUID** (`C9DCF469-A7C4-11D5-87DA-00062945C3B9`;
  edk2 `setvar MacUniqueId -guid C9DCF469-…`). NOT the VBIOS: m63 VBIOS's own file GUID is
  `3481C745-…`; the VBIOS is a *client* of the same `*0x44` registry (its `.data` GUID table
  @VA `0x10026850` holds C9DCF469; its code `mov 0x100268f0,%edx; call *0x44(%edx)` with args
  `(0x90000002, 0x3050007, 0, &C9DCF469, buf)`). C9DCF469 appears once in each of
  m63/IsaBusDxe/SmmBase `.data`; SmmBase `.data@0x14ac` also holds `F8E21975-…` = the GUID
  `0xee56924` that phase-4 memcmps at `0x0ee4d5ff`. The 5 lookup GUIDs decoded:
  `0xee567c4`=D8117CFE-94A6-11D4-9A3A-0090273FC14D, `0xee56764`=9CF24CE8-1F19-4EAE-96E1-F46AEAEC0BEA,
  `0xee56754`, `0xee56664`=D8117CFC, `0xee56654`=D8117CFF, `0xee56674`=D8117D02 (Intel EFI-HW GUID family).
- **RUNTSERV record engine** (module `0xe9e8000-0xe9e9000`):
  - `0x0e9e82e1` (`*0x44` wrapper): locks gate `0xe9e9954`, calls `0xe9e8675` (key→POST-code
    dispatcher, `outb` ports `0x80`/`0x81`, table @`0xe9e9760` 5-byte records), and if gate
    `0xe9e9962==0` calls `0xe9e8797`.
  - `0xe9e8744`: alloc (`pool` `[0xe9e9974]`, tag `"BDHS"`, size 0xd4, via `*0x2c`) + LINKS a
    node (link@node+4/+8, head@`0xe9e9948`, tail@`0xe9e994c`, count@`0xe9e9940`), returns rec=node+0xc.
  - `0xe9e8797`: fills rec `{arg1@+0, key@+0x4, arg3@+0x8, GUID[16]@+0xc, +0x1c/0x30…}` —
    **BUT only creates when gate `0xe9e9964==1` (armed)** else returns 0 silently.
  - **`0x0e9e88bf` RunServiceList**: `movb $1,0xe9e9964` (ARM) unconditionally, then for each
    node: `rec=node+0xc`, opcode = (arg1==1?0x8: arg1==2?2:3?3:0xf); `call [([0xe9e993c])]` =
    `call [0x0e9ed848]` = `0x0e9ed3d0` with args `(self=0x0e9ed848, cfgA=0xe9e9880, cfgB=0xe9e9624,
    opcode, 0, rec, size)`; then unlink+frees node (`[0xe9e9974]->*0x30`); finally `movb $0,0xe9e9964`
    (DE-ARM). cfgA/cfgB are DATA (UTF-16 `"%2$0x…"`/`"null string"`).
  - `0x0e9ed3d0` (dispatch): resolves a handler via `self+0x30` struct + `0x0e9ed5c8`, checks
    cfgA fields, calls `0x0e9ed5e6`, writes result to `*0x14(%ebp)`. Real "service" semantics
    still fuzzy.
  - **Live state: count@`0xe9e9940`=0, head/tail self-linked (`0xe9e9948/0xe9e994c`=self),
    gates `0xe9e9964=0` (never armed!), `0xe9e9962=0`, `0xe9e9954=0`** → no records ever appended.
- **RunServiceList refs** (all DATA, no code caller found): `0xe9e8296` (module code),
  `0xe9eeca4` (RUNTSERV phd0 desc table), `0xea1a428` (phd0 desc cluster at `0x0ea1a000` w/
  `"ptal"/"evnt"` strings; holds `[0xe9e9944]=0x0ea1a410` pending ptr). "phd0" tables = module
  descriptor structs (`{magic"phd0", size, count, next, …}`); the one at `0x0ebc0000`
  (size 0x2f24) also EMBEDS the module PE (MZ@0x3c, PE@0x100) = self-relocation copy. The
  six-lookup walker `0xee4d4f0` walks this chain via `0xee550d3` (records: `[0]`u16 type (4=GUID),
  `[2]`u16 size, `[8]`GUID[16], `[0x18]`payload; `[ebp+4]`(our earlier note)… walker reads
  `mov (%ebx),%edx; cmpw $0xffff`).
- **Data tables dumped**: `/tmp/phd0.bin` (0x0ebc0000-0x0ebc4000), `/tmp/rts_tbl.bin`
  (0xe9e9700-0xe9e9a40), runts module regions in `/tmp/ram_full.bin`.

**⇒ Root cause refined**: phase-4 waits for the RUNTSERV C9DCF469 variable-namespace record to be
processed, but (a) nothing arms the engine (gate64 stays 0 → `*0x44` silently drops records) and
(b) nothing ever calls RunServiceList to process them. All six sync-lookup GUIDs absent from the
phd0 chain, so the async path is taken and the dead spin runs. A C9DCF469 var-store/apple-nvram
driver (or SmmBase F8E21975-armed service) is the missing real-HW component.

### Next step (from phase-4 analysis, when resuming)
Priority experiments (each gdb-pokable, read-only until the poke):
1. **Synthetic completion via RunServiceList**: fabricate one node in low RAM
   `{link@+4/+8, rec: arg1=1 @+0xc, key=0x3041001@+0x10, arg3=0@+0x14, GUID=C9DCF469@+0x18,
   size-field@+0x2a(=node+0x1e… rec+0x1e=0)}`, link into list (count=1), then set EIP to
   `0x0e9e88bf` + `step` — observe whether the dispatch (`0x0e9ed3d0` → `0x0e9ed5c8`/`0x0e9ed5e6`)
   clears `[ebp+8]` AND rewrites `[ebp+4]`. If yes → boot may advance past `0xe6`.
2. **Force the six sync lookups**: craft a type-4 GUID record (one of D8117CFx) into the
   `0x0ebc0000` chain so `0xee4d4f0` succeeds → phase-4 completes via ITS OWN designed sync path
   (no spin). Still must solve `[ebp+4]=0` (populate a continuation, e.g. via a 0x0-RAM trampoline;
   paging is OFF so VA 0 works).
3. **Trace frame surgery**: give phase-4 a continuation by writing it into `[ebp+4]` (e.g.
   DuetBds-area `0x0e51bca1` caller `0x0ee4d4da` is a bad ret target — that re-enters the tail).
   Prefer (1)/(2) over ad-hoc continuation guesses.
4. Long-shot real-fix directions (not device emulation): implement the apple-nvram **variable-store
   format** at MCHBAR1 `0xf0000000` (`ATV_NVRAM` knob already loads flash into the window), so some
   var driver registers C9DCF469; or find which module posts the C9DCF469 record to RUNTSERV.

### AHCI experiment (2026-08-29) — CONFIRMED: disk alone does NOT unblock phase-2
Implemented a real ICH9 AHCI HBA and tested it. **Result: the AHCI disk does NOT fix the
livelock**; the firmware's storage driver never runs. Concrete facts:
- `hw/i386/pc_piix.c` ATV gate: create `ich9-ahci` (`TYPE_ICH9_AHCI`, device `8086:2922`,
  class 0x010601 = SATA/AHCI) via `pci_create_simple_multifunction` at `PCI_DEVFN(0x1f,2)`
  (the ICH7 SATA slot), wire `pcms->idebus[0/1]` from `"ide.0"/"ide.1"`. Also disabled the
  PIIX3 `has-usb` in ATV mode so `00:1f.2` is free. The AHCI's 6 port buses register in
  qtree as `ide.2`..`ide.7` (PIIX3 owns `ide.0`/`ide.1`); attach with
  `-device ide-hd,bus=ide.2,drive=sata0` (port 0). Do NOT call `ide_drive_get`/`ahci_ide_create_devs`
  in ATV mode — they fight PIIX3's auto-wiring (`Drive 'ide?-cd0' already in use`). `if=ide`
  also lands on PIIX3, not the AHCI, so use `if=none` + `-device ide-hd,bus=ide.N`.
- `info pci` confirms `00:1f.2 SATA controller 8086:2922` present; disk `sata0` attaches to
  AHCI port `ide.2`. Firmware still boots to POST `0xe4` (phase-1 wait, flag `0xe52da44`=0).
- Poking phase-1 flag `0xe52da44=1` (gdb `set {int}0xe52da44=1` then **`detach`** — must
  detach, not `pkill` gdb, or the gdbstub leaves the guest stopped with utime=0; use
  `-gdb tcp::1236` and `detach/quit`) advances boot cleanly out of the `0x0ea138xx` busy-wait.
- Boot then enters a **phase-2 live-lock** at 100% CPU in `0x0ee5xxxx`/`0x0e51xxxx` (Apple
  BDS module base `0x0e51b000`, visible `0x0e51c3f0`/`0x0e51d566`). The phase-2 call at
  `0x0e51c3f0` does `calll *0xac(%eax)` on platform protocol `[0xe52da8c]=0x0ee55aa0` probing
  Apple-custom GUID `0xe52d614` = `9C5DD135-5B53-CA4F-A536-2A4E9BE8FBA0` (verified 4xwx from
  RAM). Returns NOT_FOUND; a BDS loop re-invokes the GUID list-search (`0x0ee519aa`, list head
  `0x0ee562e8`, memcmp `0x0ee54fdc`) forever.
- **Decisive: the entire `-trace pci_cfg_*` log (72 lines) through the live-lock shows ZERO
  accesses to `00:1f.2`** — only the hardcoded table (i440FX 00:00.0, ISA 00:1f.0, "empty"
  00:1d.7 UHCI/EHCI, e1000-as-IGD 00:02.0, PIIX4_PM 00:1f.3). The `SataAhci` driver NEVER runs
  → the disk is never probed → SimpleFileSystem never installs.
- ⇒ The phase-2 Apple-GUID `*0xac` wait is a **BDS boot-option busy-wait that occurs BEFORE
  the storage/driver connection dispatch**. Adding any storage device alone won't satisfy it;
  must either (a) make `*0xac`/GUID `0xe52d614` resolve, (b) poke the phase-2 wait condition
  to let dispatch proceed to SataAhci, or (c) identify/emulate the missing device that installs
  GUID `0xe52d614` (likely a BDS boot-option / Apple platform protocol, not a plain block device).

### IR device experiment (2026-08-29) — CONFIRMED: Apple IR receiver present but does NOT unblock phase-2
Added real USB IR hardware to the ATV machine and tested. **Negative result: the firmware
never even reaches the point where the IR driver could bind.** Concrete facts:
- Fixed a **pre-existing abort**: `ich7m-ahci` shared the `ICH9_AHCI()` type-asserting cast
  (asserts concrete `ICH9_AHCI`, but realize is shared with `ich7m-ahci`). Replaced with
  `container_of(PCI_DEVICE(..), AHCIPCIState, parent_obj)` in `pci_ich9_reset`,
  `pci_ich9_ahci_init`, `pci_ich9_ahci_realize` (offset-0 `parent_obj`, type-neutral). Build clean.
- Added an **`atv-ir`** Apple IR receiver HID device in `hw/usb/dev-hid.c`: modelled on
  `usb-kbd`, VID `0x05ac`/PID `0x8241`, product "Apple TV Remote IR Receiver"
  (Strings `STR_PRODUCT_IR`/`STR_CONFIG_IR`/`STR_SERIAL_IR`, serial `1ATVREV1`),
  `desc_ir`/`desc_ir2`, `usb_ir_realize`/`usb_ir_class_initfn`/`usb_ir_info`,
  registered in `usb_hid_register_types`.
- `hw/i386/pc_piix.c` ATV block: `pci_create_simple(...,-1,"pci-ohci")` (Apple OHCI) +
  `object_resolve_type_unambiguous(TYPE_USB_BUS)` + `usb_create_simple(atv_usb,"atv-ir")`.
  `info pci`: `Bus 0, device 2 = USB controller 106b:003f` (Apple OHCI — matches real ATV).
  `info usb`: `Apple TV Remote IR Receiver` @ 12 Mb/s on port 1. No crash; boots to `0xe4`.
- **Still livelocks**: after poking phase-1 `0xe52da44=1`, EIP `0x0ee54fd3`/`0x0ee51a34`
  (BDS GUID list-search), `ECX=0e52d614` (`9C5DD135` global), NOT_FOUND, 99.9% CPU.
- ⇒ Device **presence alone is insufficient**. `armt` (`A7A1EC06`) is a **connect-time**
  driver that only binds *after* BDS's boot-option busy-wait is past — but that wait is
  upstream and never resolves. The `9C5DD135` search is the blocker, not the IR device.

### F-wait mechanism fully decoded (2026-08-29) — the actual resident wait in `DuetBds`
Function F (`0x0e51b5ff`, called from connection code at `0x0e51b434` with handle
`0x11e1a300`) is the engine of BOTH the phase-1 and the BDS connection wait. It is a pure
**async-notify busy-wait with NO IRQ** (EFL `IF=0`, no IDT) — decodes cleanly:
```
0xe52da44 = 0
call 0xe52ddbf(cb=0xe51b6e2, h=0xc(%ebp), ...)      ; protocol-notify wrapper (arg order: [8,esp,es,esp,cb])
call *0x34(0x200, 8, cb=0xe51b6f6, 0, &k)           ; event-notify on platform prot [0xe52da8c]
call *0x60(GUID 0xe52d794=EFI_SIMPLE_FILE_SYSTEM,h,&h2)
call 0xe51bc4c(0x8(%ebp)) ; may return 0x8000000e (NOT_FOUND) -> enters wait
wait: while (0xe52da44 == 0) { prot->*0x88(0x3e8) /* Stall */ }
      if (0xe52da44 == 2) call 0xe51bc4c(0x8(%ebp))
```
- Callback `0xe51b6e2`: if flag==0 set flag=**1**. Callback `0xe51b6f6`:
  `if (0xe51bc4c(0) >= 0) flag=2`.
- ⇒ Exit requires an **async protocol/event notify to fire**. Both are registered against
  EFI_SIMPLE_FILE_SYSTEM (`0xe52d794`) via the Apple platform protocol `0x0ee55aa0`
  (`*0x34`=`BeginNotify`-style, `*0x60`=`open-install-by-GUID`, `*0x88`=Stall, `*0x30`/
  `*0x44`=free). The notify never fires because no driver installs SimpleFileSystem
  (storage driver never runs — is downstream of this wait).

### Next step (when resuming)
The storage/IR driver dispatch is **downstream** of the F async-notify wait, so adding
devices can't help. The wait is the blocker. Real options, in priority order:
1. **Poke the F wait flag to force BDS past the notify wait** and observe whether boot then
   proceeds a meaningful distance (an experimental bypass to map what comes next). Poke
   `0xe52da44=1` (gdb) — same technique as phase-1. If BDS proceeds past connection to the
   bus/FS drivers, the IR+disk we added should then actually bind.
2. **Instrument QEMU to log `*0x34`/`*0x60`-registered GUIDs and the installed-protocol DB**
   at freeze, to confirm exactly which protocol the notify expects (`0xe52d794`=SimpleFileSystem
   is the lead) and which module would install it.
3. Make `*0xac`/GUID `0xe52d614` (`9C5DD135`) resolve by having a QEMU-side driver install it
   early — but that GUID is Apple "armt" specific and its installer (armt) is connect-time
   and downstream, so it cannot install before BDS's wait. Likely a dead end for the wait.
- Preferred immediate experiment = **option 1** (poke `0xe52da44` to let BDS leave the
  SimpleFileSystem notify wait, then re-enumerate via `info usb`/`info pci` + trace to see
  if SataAhci/armt now bind). Do NOT break on the NOT_FOUND return path (fires every retry).

### Option-1 poke RUN (2026-08-29) — decisive: OHCI+IR+disk STILL livelocks on `9C5DD135`
Executed the poke-recovery for real with ALL devices present. Concrete results:
- Fresh TCG boot with `-drive sata0` (bootloader img) + OHCI `106b:003f` + `atv-ir` `05ac:8241`,
  plus `-trace pci_cfg_read,pci_cfg_write -gdb tcp::1236`. Confirmed devices: `info usb`
  "Apple TV IR Receiver @ 12 Mb/s", `info pci` USB `106b:003f` + SATA `8086:27c1`.
- Poked phase-1 `0xe52da44=1` (gdb, **detach**, not pkill) → boot advanced past `0x0ea138xx`
  into the BDS/IBI connection region.
- **Firmware now DOES config-probe the OHCI**: pci trace shows `pci-ohci 00:02.0` accessed at
  regs `0xf0/0xf1` (`@0xf1 <- 0xa/0xf/0x0`, `@0xf0 <- 0xa00/0x0`) — the ONLY device beyond the
  hardcoded i440FX/PIIX3/empty/PIIX4_PM table. No `00:1f.2` SATA access. This is early-PEI
  USB/hardware probe (clustered with CAR/draminit config ops), NOT DXE dispatch.
- Guest then **livelocks identically** at `0x0ee54fd3`/`0x0ee54fdc` (CPU~0% idle-poll, EFL
  toggling IF). EAX walks many distinct memory objects ⇒ still scanning the GUID list.
- **Decoded the livelock site**: `0x0ee54fd3` is a **16-byte GUID memcmp** (compare 4 dwords:
  `push 4; mov (eax),edi; sub (ecx,eax),edi; ...; sete al`). It is the compare used by the
  `9C5DD135` list-search (`ECX=0e52d614`) at `0x0ee519d4`. Confirms NOT a device/IRQ wait.
- **Active module identity**: all live EIPs are in ImageBase `0x0ee4d000` (4 sections
  `.text@0x260/.rdata/.data/.reloc`), strings `BOOTSERV`/`DXE_SERV`/`RUNTSERV`/`IBI SYST` =
  the Apple/Intel **IBI (systab) core** — the EFI driver/connection-dispatch library. It does
  NOT reference the BDS wait flag `0xe52da44`/`0xe52da8c`, i.e. it is not F/DuetBds itself.
- **DEFINITIVE conclusion**: Disk alone, and disk+OHCI+IR, both still deadlock on `9C5DD135`.
  The `armt` connect-time driver that would install it is downstream of the very list-search
  that blocks. ⇒ device emulation cannot unblock BDS. The only remaining levers are
  (a) QEMU-side: install `9C5DD135` (or make the search hit) early in the IBI connection, or
  (b) RAM-poke the search to fabricate a hit and map what comes after. Do NOT keep adding devices.

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

---

## 2026-11-14 — tooling rebuild + corrections (READ BEFORE TRUSTING OLD ADDRESSES)

### New tooling (all in repo root, untracked)
- **`atvctl.py`** — direct GDB-RSP client (no gdb binary). Reads/writes memory AND
  registers, steps, breakpoints. Register writes via `P` work; this removes the old
  "set $eip is rejected" problem entirely. Key protocol facts discovered:
  - QEMU sends **nothing** on connect and ignores a bare `0x03`; the stop reason only
    arrives in reply to a `?` packet. Sync by sending `?` (then `QStartNoAckMode`).
  - QEMU describes this **32-bit guest with the x86_64 target description**: `g` packs
    8-byte GPRs (rax,rbx,rcx,rdx,rsi,rdi,rbp,rsp,r8-r15,rip,eflags) then 4-byte
    cs,ss,ds,es,fs,gs. Offsets: eax 0, ebx 8, ecx 16, edx 24, esi 32, edi 40, ebp 48,
    esp 56, rip 128, eflags 136, cs 144…  (`P` regnums: rip=0x10, eflags=0x11, cs=0x12…)
- **`atvboot.sh <img> [qemu args]`** — launch harness: gdb stub :1236, monitor
  `/tmp/qemu-atv/mon.sock`, *and POST-code capture*. Do not use two `-debugcon` options
  (only one allowed); the harness uses explicit chardevs on **0x80 and 0x81**.
- **`atvmon.py`** — one-shot HMP commands. **Quote file args** (`pmemsave … "path"`) or
  the monitor parses `/tmp/...` as a division expression.
- **`evie.py`** — offline analysis of a RAM snapshot: `images` (PE headers), `dis ADDR
  [LEN]`, `hex`. Snapshot with `pmemsave 0x0e400000 0xb00000 "/tmp/opencode/atv/ram.bin"`.
- I/O + hardware access tracing: `-trace enable=memory_region_ops_read -trace
  enable=memory_region_ops_write` (QEMU 11 syntax; the bare `a,b` form is rejected).
  **Warning:** 30 s of trace = 5.4 GB, 62 M lines, ~99.9 % of it port-0x61 Stall polling.

### Flash image facts (important)
- `atv-flash.bin` in the repo is **NOT pristine**. vs the flashrom dump it has 3 code
  stubs (`55 8b ec` → `33 c0 c3`) at file offsets **0x1d1872, 0x1e1556, 0x1e474f**
  (probably "silence an unsupported PEIM ReportStatusCode" patches; undocumented).
  `atv-flash.bin.bak` == `atv-devicetree/atv-flash.bin` == the real flashrom dump.
  New file `atv-pristine.bin` is that clean image — **use it for honest debugging**.
- `appletv.bin` (xgecu, desoldered) vs the flashrom dump differ in **6557 bytes, all in
  the NVRAM store region 0x1aac2a–0x1adf19** (flashrom's has one more variable). Firmware
  code is identical.
- Flash NVRAM store (0x1aac00+) holds only **`Setup`, `MTC`, `MemoryConfig`,
  `MemoryTypeInformation`** — **no `Boot####`, no `BootOrder`**. Both dumps agree.
- `bootloader-v24-04.img` = GPT, 2.25 GiB, part1 "Apple TV recovery" (HFS, 256 M),
  part2 Linux 2 G. It is a *Linux* bootloader disk, not an HFS EFI boot volume.

### CORRECTION 1 — module load addresses move between boots
DuetBds was observed at **0x0e51a000** in the pristine+disk boot but at **0x0e51b000**
in the older notes. Every hardcoded address in the older sections is therefore
**0x1000 off** in the current bring-up. Never trust a recorded absolute address:
re-derive it from the live stack/backtrace, then disassemble the running code
(`evie.py dis <addr>`). Concretely: the DuetBds SFS-wait flag is at **0xe52ca44**, not
0xe52da44 — poking the old address does nothing.

### CORRECTION 2 — "the phase-4 spin is a pure cmp/jne wait" is WRONG
The phase-4 tail (IBI module) is a **deliberate `while(1)` halt**, not a wait:
```
ee4d4d2: mov 0xee569ac,%eax ; push %eax
ee4d4d8: call *(%eax)              ; DuetBds entry — EXPECTED TO NEVER RETURN
ee4d4dd: mov %edi,0x8(%ebp)        ; edi was set to 1
ee4d4e0: cmp %esi,0x8(%ebp)        ; esi = 0  (constant)
ee4d4e3: jne 0xee4d4e0             ; == while(1);
```
The BDS entry is supposed to boot something and transfer control. When it *returns*
(nothing bootable) the IBI hangs forever. Nothing can "complete" this — do not try to
satisfy it. The real failure is **inside** the DuetBds entry.

### CORRECTION 3 — the resident wait is a normal Stall() loop, with a decodable exit
Live backtrace at POST 0xe4 (pristine + disk), innermost first:
```
inb(0x61) ← 0x0ea12484 ← 0x0e9d9277/0x0e9d929b (Stall) ← 0x0ee542ff (BOOTSERV *0x88)
← 0x0e51a69d (DuetBds wait) ← 0x0e51a3a0 ← 0x0e51ade2 (BDS entry)
← 0x0ee4d4da (phase-4 IBI) ← 0x0ee8aff0 (ret addr = 0, bottom of stack)
```
Decoded DuetBds function F (0x0e51a5ff in this boot), the wait at 0x0e51a69e:
```
call *0x60(guid=964e5b22 SFS, handle, &out)   ; BOOTSERV "open protocol by GUID"
   out != 0  -> skip the wait entirely
call 0xe51ac4c(&local)                        ; -> 0x8000000e EFI_NOT_FOUND
wait: while (0xe52ca44 == 0) BOOTSERV->Stall(0x3e8)
      if (0xe52ca44 == 2) redo 0xe51ac4c
```
So the exit condition is memory-checkable at **0xe52ca44** (0 = waiting, 2 = success
path). `0xe51ac4c` is a ConnectController-style call that returns NOT_FOUND.

### CORRECTION 4 — hardware-access trace: the firmware never enumerates PCI
Full `memory_region_ops_*` trace of a 30 s boot (until the wait):
`pcspk` 62.5 M, `io` 4448, `rtc-index` 2700, `rtc` 1350, `atv-mchbar` 661,
`atv-mchbar1` 167, `pci-conf-idx/data` 72 (=36 accesses), `isa-debugcon` 44,
`pic` 16, `pit` 2, `elcr` 2. **Zero IDE/AHCI/SCSI/USB-storage accesses.**
- All 36 PCI config accesses target only **00:00.0** (regs 0x44/0x48/0xdc/0xe4) and
  **00:1f.0** (0x40/0x80/0x84/0x88/0xf0) plus probes of **00:1e.7** (wrong slot, returns
  0xffffffff). That is chipset/PEI register poking, *not* bus enumeration.
- The GPU at 00:02.0, the OHCI/IR device, the ICH7 AHCI at 00:1f.2 and the disk are
  **never touched** — confirming (independently and definitively) that adding devices
  cannot unblock this.

### CORRECTION 5 — the IBI protocol registry, enumerated
BOOTSERV `*0x60` = "open protocol by GUID" over a **circular registry**:
- list head/sentinel **0x0ee562e8**; record `{+0: tag 'prte', +4: next link,
  +0xc: GUID[16], +0x1c: list A head, +0x24: instance-list head}`; the find loop is
  0x0ee519aa and the 16-byte GUID compare is 0x0ee54fd3.
- **40 records, list is intact and terminates.** Notable:
  - `964e5b22` EFI_SIMPLE_FILE_SYSTEM — **present, 1 instance**
  - `2f707ebb` EFI_PCI_ROOT_BRIDGE_IO — present, **0 instances**
  - `5b1b31a1` EFI_LOADED_IMAGE — present, **0 instances**
  - `964e5b21` EFI_BLOCK_IO — **absent**; no DiskIo, no Partition, no HFS/FAT.
- The GUID BDS keeps searching for and never finds is
  **`35d15d9c-535b-4fca-a536-2a4e9be8fba0`** (data at DuetBds 0x0e52c614). It is
  referenced by 9 loaded blobs but never installed.

### CORRECTION 6 — no storage/filesystem driver is ever loaded (root cause)
Scanning the whole 11 MB RAM image: `AtaBus`, `AHCI`, `BlockIo`, `SimpleFile`, `HfsDxe`,
`FAT`, `Scsi`, `Partition` → **0 hits**, while the IBI core is fully alive
(`RUNTSERV` 3, `BOOTSERV` 2, `DXE_SERV` 2, `IBI SYS` 4, **1796 `phd0` descriptors**,
2214 `ptal`). The `SataAhci`/`SataPri`/`SataSec` strings near 0xeaa0c50 are **Setup
variable option names**, not a driver (an earlier note mis-read this).
- The BOOTSERV method table at 0x0ee55aa0 is otherwise fully populated: only `*0x84` is
  a stub (`mov $0x8000001c,%eax; ret`) and `*0x5c` is NULL. `*0xb8` → 0x0e9e96af is a
  **CRC32** helper (table at 0xe9e9c80), not a dispatcher — don't chase it.
  `*0x60`=open-by-GUID, `*0x88`=Stall, `*0xac`=locate-by-GUID (0x0ee51832),
  `*0x30`/`*0x44`=free.

### CORRECTION 7 — `ATV_NVRAM=flash` is HARMFUL; erased (`0xff`) is the good baseline
Tested both (identical machine + disk, 20 s):
- `ATV_NVRAM=erase` (default): POST80 = `28 28 28 28 28 e4`, registry has **40** records
  → reaches the DuetBds filesystem wait.
- `ATV_NVRAM=flash`: POST80 = `28 28 28 28 28` (never reaches 0xe4), registry **0**
  records; the guest parks in **flash XIP code at 0xfffe2285** (= flash offset 0x1e2285)
  with ESI=0xf0008000, incrementing a 16-bit index.
The loop it kills itself in:
```
fffe2246: movzwl 0xf0000100(%edx),%ax   ; NVRAM window, base +0x100, edx = idx<<12
fffe224d: cmp $0xffff,%ax ; je  -> return 0      ; <-- "erased" short-circuits here
          test %ax,%ax   ; je  -> return 0
fffe2272: movzwl %ax,%ecx
fffe2275: movzwl (%ecx,%esi,1),%ebx     ; dereferences a value read out of NVRAM
          cmp %edi,%ebx ; je  found
fffe227d: movzwl 0xf0000102(%ecx,%edx,1),%ax
fffe2285: shr $4,%ax ; jne loop
```
So the firmware expects a **decoded platform/NVRAM structure at 0xf0000000+0x100 with
4 KiB stride and 16-bit fields** — *not* a raw 2 MB flash alias. Feeding it raw flash
makes it dereference garbage. The `memset(0xff)` default only works because 0xffff is
treated as "no entry / empty". Any future NVRAM work must model this structure, not just
copy the flash image.

### ⇒ Revised root cause
The IBI/DXE core runs, installs ~40 platform protocols, then **BDS starts before any
PCI enumeration and before the storage drivers are dispatched**. It tries to connect the
boot-device handle (`0xe51ac4c` → EFI_NOT_FOUND), registers an async notify for a
filesystem, and waits. The notify can never fire because the driver that would supply it
was never dispatched. Once poked past that wait, BDS's `35d15d9c` lookup fails, the BDS
entry returns, and the IBI parks in `while(1)`. There is also **nothing to boot**: the
NVRAM store has no boot options at all.

### Reproducible baseline (current best)
```
./atv/atvboot.sh atv/atv-pristine.bin \
  -drive file=/run/media/orestes/Guinea_pig/Projetos/atv-devicetree/bootloader-v24-04.img,if=none,id=sata0,format=raw \
  -device ide-hd,bus=ide.2,drive=sata0
# then: atv/atvmon.py "stop" 'pmemsave 0x0e400000 0xb00000 "atv/dumps/ram.bin"' "cont"
# POST80 should read 28 28 28 28 28 e4 within ~20 s (TCG).  Do NOT set ATV_NVRAM=flash.
```
`bootcompare.sh <tag> ENV=VAL ...` boots, snapshots RAM and reports
`POST80 + registry record count` — use it to compare configurations objectively.

### Where to go next (highest value first)
1. **Driver-dispatch, not device emulation.** Stop adding hardware. The question to
   answer is why the modules listed in the `phd0`/`ptal` descriptor chain are not
   dispatched. Decode the phd0 record format properly (`{magic 'phd0', u32 size, u32
   count, u32 next}` + records tagged with GUIDs, `ptal`, `evnt`) and map it to the
   boot-path dependency graph. `*0xb8` (RUNTSERV) is the dispatch entry point.
2. Instrument the registry live: watch installs/opens of the 40 records and see which
   module installs what, to find the first missing dependency.
3. Only after a storage driver runs does the disk matter; then `bootloader-v24-04.img`
   (part1 = HFS "Apple TV recovery") becomes relevant.
4. Consider supplying NVRAM boot options once a block device exists — the store format in
   flash is Apple's (`aa 55` record headers + 4-byte GUID + UTF-16 name) and the ATV stub
   already backs the 0xf0000000 window with RAM.

---

## 2026-11-14 (later) — tooling layout, dispatch trace, m63 VBIOS located

### Tooling now lives in `atv/` (nothing left at the repo root)
All helpers moved to `atv/`: `atvctl.py`, `atvmon.py`, `evie.py`, `reg.py`, `li.py`,
`names.py`, `pair.py`, `phd.py`, `pe.py`, `grefs.py`, `fwinv.py`, `dispatch.py`,
`snapstat.py`, `snapshot_seq.py`, `trace_protocols.py`, `trace_loads.py`, `whoguid.py`,
`vbios.py`, `atvboot.sh`, `bootcompare.sh`, plus `atv-pristine.bin` and `venv/`.
**Every command needs the venv** (`uefi_firmware` is needed for Tiano decompression):
`atv/venv/bin/python atv/fwinv.py ...`. `-bios atv/atv-pristine.bin`.
RAM snapshots now go to `atv/dumps/` (`/tmp` was wiped once already); monitor socket is
`atv/run/mon.sock`, POST80 log `atv/run/post80.log` — start QEMU with a **relative**
monitor path or it silently fails to create the socket.

### The dispatcher never *attempts* the missing drivers (`!!` correction)
`atv/trace_loads.py` breaks in the core's protocol installer (`0x0ee51c3d`, `esi`=new
`pifc`, `ebx`=`prte` record) and keeps LOADED_IMAGE installs; it now also prints the
FvFile GUID from the image's `FilePath` and the caller stack. Result (one TCG boot):
- 25 LOADED_IMAGE installs, **all from the same call site** `0x0ee4dbf9`
  (`CoreLoadImage` in the core at `0x0ee4d000`; `0x0ee48390` is a constant 2nd stack
  slot). So every image came from the DXE dispatcher — not from BDS/ConnectController.
- Install order (FV file indexes): 2,3,4,5,7,8,9,10,**15**,29,31,6,11,12,13,**16**,32,
  100,53,**326e9cc6**,41,41,51,30.
- The final RAM snapshot only holds 22 records: **files 15 (`67ac0b1e`), 16
  (`62fc1b9a`) and `326e9cc6-9839-4885-b2ed-275903b668e1` were loaded and later
  unloaded**, and `1a2b4139` was loaded twice — so "only 21 drivers ever load" was
  wrong: ~24 load and 3 disappear again.
- Files 14–28 etc. are *never attempted* at all (no CoreLoadImage call), so DEPEX
  evaluation / driver-list population is the gate, not the PE loader. The FV driver did
  hand out a device path for every file (each FvFile GUID node `04 06 14 00` occurs once
  in RAM, twice for dispatched ones), so the FV side is fine; the *dispatcher's* driver
  list is the thing to inspect next.

### The m63 VBIOS IS inside the firmware dump (but not as an option ROM) (`!!`)
There is **no** standalone PCI option ROM in `atv-pristine.bin`: zero `PCIR` and zero
`NVIDIA` strings in raw flash, and the only `55 aa` hits are random data. The GeForce Go
7300 VBIOS is instead a blob inside a normal DXE module:

```
FV0 file 3481c745-41d1-4795-92c4-cf5e8e63af04   (idx 120, off 0x94f70)
  COMPRESSION(Tiano) -> GUID_DEFINED fc1bcdb0 -> PE32 imagebase 10000000, entry 10001702, imgsz 28000
    .data @ PE 0x16000 -> VBIOS option ROM @ PE 0x16b80, length 0xec00 (60416 bytes)
```
ROM facts (extracted by `atv/venv/bin/python atv/vbios.py --info` → `atv/work/m63-vbios.rom`):
`55 aa 76 eb`, `K7400`/`VIDEO`/`IBM VGA Compatible`/`10/08/08`,
**"GeForce Go 7300 VGA BIOS (Apple M63)"**, `Version  5.72.22.68.00`,
`Copyright (C) 1996-2006 NVIDIA Corp.`, `PCIR` vendor **10de** device **01d7** class
`030000`, indicator 0x80 (last image), **checksum OK**. `10de:01d7` is exactly the ID the
QEMU shim (`TYPE_ATV_NVIDIA_GPU`, `pc_piix.c`) advertises, so the shim and the real
device agree.
- The module has **no DXE_DEPEX section** and is **not** in the LOADED_IMAGE list, i.e. it
  is never dispatched; its ROM only exists twice in RAM as FV extraction copies
  (`0xe983b94`, `0xe9acbb0`). Phase-4's context block pointers `0x100017b0/0x100017bc`
  are PE-relative call arguments inside this module (`0x10001702` is its entry), so the
  IBI/RUNTSERV side *does* reference it as a service provider.

### ⇒ No video, and none is possible yet (`!!`)
- `atv-nvidia-gpu` exists at `00:02.0` (BAR0 = 16 MiB VRAM RAM, BAR2 = 16 MiB **stub**
  MMIO where reads return 0 and writes are discarded, `hw/i386/pc_piix.c:747-786`).
- The firmware **never touches `00:02.0`**: the complete `memory_region_ops_*` trace shows
  PCI config accesses only to `00:00.0`, `00:1f.0` and a bogus probe of `00:1e.7`.
- An 11 MB RAM image at the freeze contains **0** hits for `GraphicsOutput`, `GOP`, `UGA`,
  `Framebuffer`, `Video`; the 6 `VGA`/`NVIDIA` hits are inside the m63 blob/tables.
- So graphics (GOP/UGA + m63 VBIOS handoff) sits downstream of the same dispatch/BDS wall.
  When it is reached, do not expect the stub MMIO to mode-set — a real NVIDIA MMIO model
  (or a `romfile=` on the shim) will be required to get pixels, and VRAM is dumpable with
  `pmemsave` for headless checking.

---

## 2026-11-15 — DXE dispatch CRACKED (60 drivers); the wall is BDS, provably downstream

### TL;DR
Forcing the 7 arch-protocol producers **and never unloading a failed driver** makes the real
EDK1 cascade run: **60 drivers start in one dispatcher pass**, all 13 arch protocols get
installed (registry 40 → 63 `prte` records; `PciRootBridgeIo` gains its first instance).
PCI enumeration *still* does not happen (trace byte-identical), `BlockIo` is still absent,
and BDS is still the wall — now proven to be **downstream** of the dispatcher.

### Tooling added this session (all in `atv/`)
- `exp_start.py --false GUID[,…]` — rewrite a driver's runtime depex to `FALSE/END`
  (`07 08`) + clear flags, i.e. *never schedule* it. Complements `--true`.
- `exp_start.py --keepimg` — hw breakpoint at `0x0EE4DE15` (`call *0x10(%esi)` in
  `CoreStartImage`); if the driver EntryPoint returned an error status, set `eax=0` before
  continuing. This suppresses the unload described next.
- `guidram.py` — in a RAM snapshot, list every occurrence of a protocol GUID, the owning PE
  image (loaded vs FV copy) and the nearest preceding string; `--missing` covers the DEPEX
  GUIDs that no flash module installs.

### Why forcing alone killed the boot (`!!` — CoreUnloadImage, not CpuDxe)
`CoreStartImage` ends with (verified live at `0x0EE4DE70`):
```
ee4de70: mov 0x6c(%esi),%edi              ; EntryPoint status
ee4de73: cmp %ebx,%edi ; jl 0xee4de7d     ; status < 0  → unload
ee4de77: cmpl $0xa,0x8(%esi) ; jne …      ; or image state == 0xa
ee4de7d: push $1 ; call core_unload_image ; 0x0ee4dea6
```
So **any driver returning an error status is unloaded**. Two forced drivers do:
`62d171cb` (CpuDxe) → `0x8000000e` and `e0ecbec9` → `0x8000001d`. Their images are freed
while everything they installed stays registered → dangling pointers. `gCpu`
(`0x0EE569A0`) keeps pointing at the freed CpuDxe image, so `CoreRestoreTpl`
(`0x0EE52462`: `call *method(gCpu)`) jumps into reused memory (`~0xbddc3407`) — the "death"
of the earlier cluster run.
⇒ **The killer is the unload, not one driver.** Bisect evidence: forcing only the 6
non-CpuDxe drivers dies even earlier (4 drivers, then `eip=0x100f`); forcing only CpuDxe
parks at `0e5190ac`. With `--keepimg` (no unload) all 7 run fine.

### Reproducible full cascade
```
./atv/atvboot.sh -drive file=…/bootloader-v24-04.img,if=none,id=sata0,format=raw,snapshot=on \
                 -device ide-hd,bus=ide.2,drive=sata0 -trace pci_cfg_read -trace pci_cfg_write
# ~15 s → POST80 = 28 28 28 28 28 e4
atv/venv/bin/python atv/exp_start.py --keepimg --status \
  --true cbd2e4d5-…,ad608272-…,62d171cb-…,2bded685-…,45424d0c-…,378d7b65-…,f099d67f-… \
  --false 5f4735f7-dcf9-40fd-8858-026f931942a9 --rounds 1 --budget 100 \
  --dump atv/dumps/ram-skip1.bin
# → 60 drivers, round 1 returns normally, guest back in the BDS Stall loop
```
Snapshot `atv/dumps/ram-skip1.bin` (60 started / 63 `prte` / all arch protocols done);
logs `atv/run/skip1.log`; earlier variants `atv/run/keep1.log`, `atv/dumps/ram-ram512.bin`
(57 drivers with `5f4735f7` still hanging).

### Driver `5f4735f7` (FV idx 14, image `0x0E017000`) — ICH/MCH chipset init (skipped)
57th in the cascade, **hangs forever**; `--false` on it releases 4 more drivers
(`ef0c99b6`, `8a107cc5`, `c21cf0e2`, `42f5f135`). Decoded:
- `e017d7a: mov 0xe0180c4,%edi` — its MMIO base comes from the **platform store**:
  `mov 0xf0000048,%eax; and $0xfffffffe; test $6,%al; and $0xf8,%al`.
- Our stub returns `0xffffffff` there (erased window) → base `0xfffffff8` → every access
  wraps into low RAM (`base+0xd8040` → `0x0E58038`, `base+0xd8000+0x40` → `0x0D8038`).
- It builds descriptors in the `base+0xd8000` block, then busy-polls forever:
  `e017e7e: mov 0x68(%esi),%cl; and $3; cmp $2; jne` with `esi = 0x10000000` (the value it
  got back from `DXHESERV+0x1c`), which is unmapped/RAM → reads 0, never `2`.
  Register trio: `+0x60` address, `+0x64` data, `+0x68` command/status; `+0x8` bit0 is a
  self-clearing "start" bit.
- `DXHESERV` = table `0x0EE55B68` (tag `'DXHE'/'SERV'`), method `+0x1c` = `0x0EE4EB0F` →
  `0x0EE4E74A` = the platform **PCI config accessor** (validates bus<5, dev<4, fn<3,
  reg<0x40; `0x8(%ebp)&0x20` selects the Mem path `0xEE55E60`).
- **Store layout hint**: the code also hardcodes reads at `0xf00f80f0` and `0xf00f8048`,
  which equal `base+0xf8000+0xf0` for base `0xF0000000` ⇒ store field `+0x48` is meant to
  hold `0xF0000000` (the store's own base). Live window: `+0x40=0xfed19001`,
  `+0x44=0xfed14001`, `+0x48=0xffffffff`, `+0x4c=0xfed18001` — the firmware programs three
  device bases but never `+0x48`. Pre-seeding `+0x48` in the ATV stub is a possible (untested)
  improvement, but the `+0xd8000/+0xef000/+0xf8000/+0xfa000` sub-blocks would then also need
  register models, so it may just move the hang.

### The BDS wall, precisely located (this boot: DuetBds @ `0x0E51A000`)
1. SFS (filesystem) wait — `0x0E51A5…`:
```
e51a66c: call *0x60(BOOTSERV, guid 0xe52c794 = 964e5b22 SFS, handle, &out)
e51a674: cmp %esi,%edi ; jne done      ; out != 0 → skip the wait
e51a678: call 0xe51ac4c                ; ConnectController(boot handle) → 0x8000000e
e51a69e: while (0xe52ca44 == 0) BOOTSERV->*0x88(0x3e8)      ; Stall loop
e51a6a6: if (0xe52ca44 == 2) redo 0xe51ac4c
```
   Poke `0xe52ca44 = 1` + monitor `cont` to release it (2 = redo the connect).
2. Then BDS spins on `LocateProtocol(35d15d9c-535b-4fca-a536-2a4e9be8fba0)`:
   `0x0E51B3F0` → BOOTSERV `*0xac` → IBI registry walk `0x0EE519AA` (GUID memcmp
   `0x0EE54FD3`; hot EIPs `0x0EE519D4` / `0x0EE54FD3` / `0x0EE54FDC`).
   Only three flash files mention that GUID: `a6f691ac` (consumer, also uses BlockIo),
   `a7a1ec06` = **armt** (depex-less, *did* run this boot, status 0) and `3e4cf2bb`
   (consumer, still depex-blocked). ⇒ armt installs it only when it *binds* to a device,
   i.e. inside ConnectController, which never gets that far.
   The IBI locate helper can **auto-create** a `prte` record when its 3rd arg is nonzero
   (`0x0EE519E7`: pool 0x2c, tag `'prte'`, copy GUID); BDS passes 0.
- **GUID identity note**: the "two" mystery GUIDs in older notes are one protocol —
  `0x0E52D614`/`9C5DD135-5B53-…` is just the wire-byte rendering of
  `35d15d9c-535b-4fca-a536-2a4e9be8fba0`.

### Still-blocked drivers after the full pass (`depex_eval.py atv/dumps/ram-skip1.bin`)
- BEFORE-only chain `51c9f40c`, `53bcc14f`, `9f455d3b`, `a62d933a`: `CoreIsSchedulable`
  returns FALSE while `rec+0x38` (BEFORE) is set, so they never become schedulable.
- SMM family: `1390954d` (SMM_BASE ×6), `8b24e4d4` (SMM_CONTROL ×6), `3792095a` — need SMM
  initialised, which the generic `pc` machine never does.
- Misc: `ebbe2d1b`, `f496922d`, `f33261e7`, `14fc52be`, `c50b323e`, `b709efa0`, `e541b773`,
  `074e1e48`, `2a82fce6`, `53a4c71b`, `e4518e76`, `35d15d9c`.

### PCI enumeration is NOT in the dispatcher (decisive)
With 60 drivers started the `pci_cfg_*` trace is byte-for-byte the same as a plain boot:
72 accesses total — only `i440FX 00:00.0` (0x44/0x48/0xdc/0xe4), `PIIX3 00:1f.0`
(0x40/0x80/0x84/0x88/0xf0) and the bogus `00:1e.7` probe. `00:1f.2` (AHCI), `00:02.0` (GPU)
and the USB controller are never touched. `BlockIo` (964e5b21) is absent; the block/FS
drivers that ran (`a6f691ac`, `5058f21c`, `43b93232`, `56aaa799`) all returned 0 and
installed nothing — they are driver-binding drivers waiting for a controller handle.
⇒ Enumeration/binding lives in BDS's ConnectController phase, downstream of both waits.
**Do not add devices yet.**

### Verdict: is the arch deadlock real hardware behaviour? No.
- The *gate* is genuine EDK1 (`CoreIsSchedulable 0x0EE54ADF` → `CoreAllEfiServicesAvailable
  0x0EE53B86`): depex-less drivers really must wait for all 13 arch protocols.
- But the 7 arch producers' depexes need `3f557189`, `f496922d`, `5572e711`, `f33261e7`,
  which **no flash module installs** (guidprov: consumers only). They must come from the
  IBI/phd0 platform layer — the part that never arms under QEMU (RUNTSERV gate `0xe9e9964`
  stays 0, record count 0). On real hardware those protocols exist, the arch drivers run
  normally, and nothing needs poking.
⇒ `--true` + `--keepimg` is a faithful *stand-in* for the missing IBI install step, not a
hack around broken firmware.

### Next moves (priority)
1. Answer **who installs `3f557189` / `f496922d` / `5572e711` / `f33261e7`**. If it is the
   IBI, the real fix is arming the phd0/RUNTSERV engine (see the 2026-08-29 phase-4 notes);
   if it is a PEIM, find it in the PEI FV (`0x1d0000`) and why it doesn't run.
2. Make BDS's `ConnectController` (`0x0E51AC4C`) produce something: the boot handle it is
   given (`0xc(%ebp)`, opaque token `0x11e1a300`) resolves to `EFI_NOT_FOUND`. Either give
   the platform a real boot-device handle, or make `35d15d9c` resolvable so the `*0xac`
   lookup succeeds.
3. Only then expect PCI enumeration → AHCI → `BlockIo` → `SimpleFileSystem` (the entire
   storage stack is downstream of item 2), and only after that does the disk/NVRAM boot
   option work make sense.

---

## 2026-11-15 (late) — EFI 1.02 GUIDs cracked: 31 driver bindings, PCI enumeration reached

### TL;DR — the "no driver bindings / driver model is absent" conclusion was WRONG
This firmware's DXE drivers use the **EFI 1.02 protocol GUIDs**, not the UEFI ones:

| protocol | GUID here | instances (live, 61-driver cascade) |
| --- | --- | --- |
| DriverBinding | `18a031ab-b443-4d1a-a5c0-0c09261e9f71` | **31** |
| ComponentName | `107a772c-d5e1-11d4-9a46-0090273fc14d` | 27 |
| LoadedImage | `5b1b31a1-…` | 82 |
| PciRootBridgeIo | `2f707ebb-…` | 1 |
| PciIo | `4cf5b200-…` | **0** (still absent) |
| BlockIo / SimpleFileSystem | `964e5b21` / `964e5b22` | 0 / 0 |

The UEFI `665e3ff6` DriverBinding record has exactly **one** instance and it belongs to
`a6f691ac` (the module at `0x0E51A000` that everyone called "DuetBds"), so filtering the
registry by `665e3ff6` shows "1 binding" and is a decoy.  **Any tool that looks for
driver bindings must use `18a031ab`.**

How it was found: `atv/fvpe.py 64 --save atv/work/pcibus.bin` + `objdump -D -b binary
-mi386 --adjust-vma=0x10000000` on the PciBus FV file — its entry point installs the
interface at `0x10005a00` (`Supported/Start/Stop/Version/ImageHandle/DriverBindingHandle`)
under GUID `18a031ab` and the interface at `0x10005a20` under `107a772c`.

### gBS is fully decoded (0x0ee55aa0, index 0 at **+0x4c**)
Apple header occupies `[gBS+0x00 .. gBS+0x4b]` (tag `"BOOTSERV"`), then the EFI 1.10
service vector with 4-byte slots.  Verified both by the offsets the firmware's own
drivers call and by disassembling `CoreConnectController` itself:

```
+0x4c InstallProtocolInterface      +0x50 ReinstallProtocolInterface
+0x54 UninstallProtocolInterface    +0x58 HandleProtocol
+0x5c NULL (reserved)               +0x60 RegisterProtocolNotify
+0x68 LocateDevicePath              +0x70 LoadImage       +0x74 StartImage
+0x7c UnloadImage                   +0x84 stub -> 0x8000001c
+0x88 Stall                         +0x90 ConnectController (0x0ee534c5)
+0x94 DisconnectController          +0x98 OpenProtocol    +0x9c CloseProtocol
+0xa8 LocateHandleBuffer            +0xac LocateProtocol
+0xb0 InstallMultipleProtocolInterfaces  +0xb4 UninstallMultipleProtocolInterfaces
+0xb8 CalculateCrc32
```
(The old "Apple custom platform protocol with methods at 0x34/0x60/0xac…" was just gBS.)

### 'pifc' (protocol-interface) layout — verified
```
pifc+0x00 'pifc'            pifc+0x04 LIST_ENTRY  (into Handle->Protocols)
pifc+0x0c EFI_HANDLE        pifc+0x10 LIST_ENTRY  (into Prot->Protocols = registry node)
pifc+0x18 'prte' record     pifc+0x1c Interface   pifc+0x20 LIST_ENTRY (OpenList)
```
Registry instance node = `pifc+0x10`, so `node+0x0c` = Interface (what `reg.py` prints)
and `node-0x10` = pifc.  Owning handle = `[pifc+0x0c]`.

### The storage stack IS in the flash (and does start)
New tool **`atv/fvgrep.py`** unwraps COMPRESSION/GUID_DEFINED and greps the decompressed
payload for ASCII **and UCS-2** keywords.  Identified:

| FV idx | GUID | what |
| --- | --- | --- |
| 64 | `c0734d12-7927-432b-986b-a7e3a35ba005` | **PciBus** (string "PciBus"), entry 0x260 |
| 65 | `69fd8e47-a161-4550-b01a-5594ceb2b2b2` | IDE driver ("Ide") |
| 80 | `4cf484cd-135f-4fdc-bafb-1aa104b48d36` | HFS driver |
| 92 | `43b93232-afbe-11d4-bd0f-0080c73c8881` | Partition/disk driver |
| 98 | `3849885c-1646-42a7-83a7-9bd9645e211b` | SimpleFileSystem/FileSystem |
| 35 | `f6d35fbb-63ea-4b25-81a5-5e62b4886292` | "Fat" + PciIo consumer (big driver) |
| 120 | `3481c745-…` | m63 VBIOS module (also started, `--false 5f4735f7` cascade) |

`6c79ba9b`'s "SataAhci/SataPri/SataSec" are **Setup variable option names** (ACPI/Setup
module), not an AHCI driver — do not chase them.

With `exp_start.py --true cbd2e4d5 --false 5f4735f7 --rounds 1 --budget 120` **61 drivers
start**, including PciBus (`c0734d12`), IDE (`69fd8e47`), HFS (`4cf484cd`), Partition
(`43b93232`), armt (`a7a1ec06`) and the m63 VBIOS module.

### ConnectController on the PCI root bridge now reaches PciBus.Start() (`!!`)
New tools:
- `atv/protomap.py` — handle database dump (handles → protocols → interfaces, with the
  DriverBinding `Supported/Start/Stop/Version/ImageHandle` fields).  Use `--guid` / `--bindings`.
- `atv/call.py` — safe firmware call: plants a `jmp $` trampoline below ESP, returns into it,
  reads EAX, restores the register file.  Do **not** return into the guest's old EIP (it lives
  in the Stall loop, so the breakpoint fires spuriously).
- `atv/connectroot.py` — fire-and-forget `gBS->ConnectController(root bridge, 0, 0, TRUE)`
  (sends `c` and closes the socket, so the RSP stream can't desync).
- `atv/pciconnect.py` — same but with breakpoints and a stack dump annotated with image bases.

Facts:
- root-bridge handle: from the `2f707ebb` registry record → `[pifc+0x0c]` (e.g. `0x0eb2b19c`).
- `ConnectController(handle,0,0,TRUE)` → **EFI_SUCCESS**, and it calls `Supported()`
  (`0x0e22d2e9`) and `Start()` (`0x0e22d3a8`) of **DriverBinding instance #3 = PciBus**
  (iface `0x0e232a00`; image handle `0x0e23cf10`, module base `0x0e22d000`).
- **PciBus.Start() then panics**: the guest ends up in Apple's deliberate halt at
  `0x0e457ba6`, byte signature `c7 45 fc 01 00 00 00 83 7d fc 00 75 fa`
  (`movl $1,-4(ebp); cmpl $0,-4(ebp); jne`) — 3 copies exist in RAM, find them by pattern.

### The panic is Apple's "report fatal status and halt"
`0x0e457b49` (same module as CpuArch, `62d171cb` at `0x0e457000`):
```
look up arg1 in a 17-entry table (dwords @0xe45c2c0, bytes @0xe45c2c4, count @0xe45c348)
code = 0x3000000 | 0x3050000 | table[arg1]
RUNTSERV ([0xe45cc58] = 0x0eeffd90) -> *0x44(0x90000002, code, 0, &C9DCF469, 0)
for (;;);        // no code can clear the local -> a real halt
```
Break at `0x0e457b49` right after ConnectController: `ebx = arg1 = 6`, caller `0x0e45ba76`
= the status-code dispatcher (`call *[0xe45c280+ebx*4]`, table head `0x0e45cea0`), stack
args `0x0dde1010` (pool buffer) and `0x9c4`.  Note `*0x44(0x90000002, key, 0, &C9DCF469, buf)`
is the same Apple variable-namespace service the m63 VBIOS uses with key `0x3050007`.
⇒ The ATV firmware **halts on certain reported error status classes** while the console
does not exist yet, so any error inside ConnectController is fatal.

### Root cause: the PCI root bridge reports an EMPTY resource list
Calling `PciRootBridgeIo->Configuration()` (`[iface+0x44]`) directly from the debugger
returns SUCCESS with a static buffer at **`0x0e5479f0`** (inside module `9fd2360e`, the
root-bridge driver) holding six `0x2e`-byte ACPI QWORD address-space descriptors whose
addresses are all zero: `8a 2b 00 01 00 00 …`, `8a 2b 00 00 00 00 20 00 …`.
`GetAttributes` returns `Supported = 0, Attributes = 0`.
`Configuration()` = `0x0e547748` merely copies entries from the root-bridge **instance**
at `This-0x130` (40 bytes per entry, only entries with `Type` @`+0x18` == 3 are emitted,
max 6) into that static buffer ⇒ the instance's resource table is empty because the
platform/NVRAM init that should fill it never ran.

### Zero PCI config traffic from PciBus.Start() (decisive)
With `-trace pci_cfg_read -trace pci_cfg_write`, the count is **44 before and 44 after**
the ConnectController call (only `i440FX 00:00.0`, `PIIX3 00:1f.0`, `PIIX4_PM 00:1f.3`,
`pci-ohci 00:02.0`).  PciBus dies *before* touching the bus, consistent with the
resource-parse failure.

### Next steps (priority)
1. **Populate the root-bridge resource table** at `This-0x130` (6 × 40-byte entries,
   `Type@+0x18 = 3`) with a real PCI resource list (memory aperture, IO aperture, bus
   range) so `Configuration()` hands PciBus usable descriptors.  The exact source→descriptor
   field mapping is in `0x0e54775c..0x0e5477a5` (copies dwords to buffer `+0x00/+0x0a/+0x16/+0x1a`
   per entry) — decode it before writing entries.
2. Or make the root bridge build its own resources: it must be reading the platform store
   at `0xf0000000` (the 4 KiB-stride/16-bit-field structure of CORRECTION 7; `+0x48` should
   hold `0xF0000000` per the `5f4735f7` analysis).  That is the "real" fix if the format can
   be recovered.
3. Then re-run `connectroot.py` and check `reg.py`-style live reads for `4cf5b200` (PciIo)
   instances plus `-trace pci_cfg_*` accesses to `00:1f.2` (AHCI).  Only then do
   BlockIo/Partition/SimpleFileSystem become reachable.
4. Tool gotchas learned here: (a) `atvctl` desyncs once `cont()` times out — observe with
   the HMP monitor (`atvmon.py "info registers"`) and use the fire-and-forget
   `connectroot.py`; (b) use **hardware** breakpoints (`Z1`) for the panic — a software
   breakpoint at `0x0e457b49` did not fire; (c) HMP does not accept `$reg` in `xp`
   expressions, so parse `info registers` and use absolute addresses.

---

## 2026-11-17 — video: the machine now has a display, and Linux can own the framebuffer

### Hardware facts (from the physical ATV1)
- 256 MiB system RAM, **64 MiB dedicated GeForce Go 7300 (G72M) VRAM**. The BAR1
  aperture is the VRAM window (`0x10000000`, now modelled 64 MiB:
  `ATV_NVIDIA_FB_SIZE 0x04000000`).
- Maximum output is **1080p60**. Everything leaves the box through **HDMI only**, via a
  **Silicon Image SiI1930CTU** PanelLink/HDCP transmitter fed by the GPU's digital
  output (the transmitter also mixes the audio stream). Any future "real" video model
  needs to produce DVI/HDMI TMDS timings, not analogue VGA.
- There is **no firmware-side picture**: on real hardware the panel stays dark (no
  signal, amber LED) until an OS initialises the display. So "video works" under QEMU
  means the *guest OS* drives a framebuffer, not that the firmware draws something.

### The GPU shim now has a QEMU graphic console (`hw/i386/pc_piix.c`)
`ATVNvidiaGPUState` gained `QemuConsole *con` + a realtime refresh timer (100 ms) and
renders one of two sources into the console surface:

| `ATV_GPU_DISPLAY` | source |
| --- | --- |
| `text` (default) | 80x35 VGA text cells at physical `0xB8000`, drawn with `ui/vgafont.c:vgafont16`, scaled to the surface, CGA palette |
| `vram` | the BAR1 aperture as `WxHx32` little-endian pixels |
| `auto` | `text` until the guest has written anything into the first 64 KiB of BAR1, `vram` afterwards |

`auto` is the mode to watch a boot with (`ATV_DISPLAY=gtk ATV_GPU_DISPLAY=auto`): the
firmware and the loader only ever draw into the text console, while the kernel switches
to the stamped linear framebuffer, so `text` alone shows a frozen screen and `vram` alone
shows black until Linux starts drawing.

Other knobs: `ATV_GPU_GEOM=WxHxD` (default **1920x1080x32**), `ATV_GPU_TEXT=CxR`
(default 80x35). `screendump` works headlessly, so the picture can be inspected
without a GUI:

```
atv/venv/bin/python atv/atvmon.py 'screendump "atv/work/screen.ppm"'
```

Live window: launch with `-display gtk` (or `sdl`) instead of `-display none`.
Verified: `atv/work/screen.png` shows the kernel's boot log rendered as text.

### Why the guest got no framebuffer, and the exact fix (`!!`)
The disk's loader normally draws a splash and fills `boot_params.screen_info` itself
(it needs the GPU VBIOS/UGA path, which never runs here), so under QEMU `screen_info`
is zero and the kernel falls back to a dummy/VGA text console.

`atv/fbpatch.py` stamps `screen_info` from the debugger. **Two traps cost a session:**

1. **Field offsets.** `struct screen_info` (uapi/linux/screen_info.h) has
   `orig_video_lines` at `0x0e`, `orig_video_isVGA` at `0x0f`, `lfb_width` `0x12`,
   `lfb_height` `0x14`, `lfb_depth` `0x16`, `lfb_base` `0x18`, `lfb_size` `0x1c`
   (64 KiB units for VLFB), `cl_magic/cl_offset` `0x20`, `lfb_linelength` `0x24`,
   `red_size/red_pos` `0x26/0x27`, green `0x28/0x29`, blue `0x2a/0x2b`,
   `rsvd_size/rsvd_pos` `0x2c/0x2d`. Writing everything one byte low "worked" (vgacon
   came up) but the framebuffer fields were nonsense.
2. **`sysfb_parse_mode()` must match `SIMPLEFB_FORMATS` exactly.** For 32bpp it requires
   `lfb_depth=32`, red `{8,16}`, green `{8,8}`, blue `{8,0}` **and transp `{8,24}`**,
   i.e. `rsvd_size=8`, `rsvd_pos=24` (`x8r8g8b8`). With `rsvd_size=0` the parse fails
   and sysfb silently falls back to a legacy **"vesa-framebuffer"** device, which
   nothing binds (no `vesafb` driver in this kernel) — that is why no `fb0` appeared
   even though the mode looked plausible.

Correct values now used: `VIDEO_TYPE_VLFB`, base `0x10000000`, `1920x1080x32`,
`lfb_size = VRAM>>16 = 1024` (the whole 64 MiB, as VBE reports it),
`lfb_linelength = 7680`.

Then: `sysfb_init()` (device_initcall) → `platform_device_alloc("simple-framebuffer")`
→ `simpledrm` binds. Alpine's initramfs already does
`modprobe -a $(echo "$KOPT_modules $rootfstype" | tr ',' ' ') loop squashfs simpledrm`
(unconditional), and `simpledrm.ko.gz` **is** in the initramfs, so no cmdline change is
required; `--cmdline` exists in `fbpatch.py` only as an override.

### Watchpoint handshake (why polling does not work)
The loader writes the setup header **byte by byte**, so `HdrS` appears over four stops
(`H\x00\xb4J` → `Hd\xb4J` → `HdrJ` → `HdrS`). `fbpatch.py` keeps one gdb connection open
and sets write watchpoints on `0x90202` (`HdrS`) and `0x90228` (`cmd_line_ptr`), stamps
`screen_info` on the `HdrS` stop and (optionally) rewrites the command line at the
pointer once it becomes plausible (`0x1000 < ptr < 0x9f000`). Ordering is safe because
the copy runs low→high: by the time `0x202` is written, `0x000-0x1ff` is already in
place, and our stamp at `0x90000` is never overwritten afterwards.

Timing (TCG): `HdrS` lands ~45-60 s after `session.sh`, i.e. right after the firmware's
boot manager (POST `58 5a`) starts the loader; `-no-shutdown` means a triple fault
parks QEMU instead of resetting it.

### Tooling added
- `atv/fbpatch.py` — boot_params/screen_info/cmdline patcher (watchpoint driven).
- `atv/monsample.py` — high-rate HMP sampler (the gdb stub can only read memory with the
  VM stopped, and each `atvctl` connect costs ~1 s; the monitor answers `xp` while
  running, which is how the `HdrS` timing above was measured).
- `atv/vidboot.sh` — boot → `session.sh` → `fbpatch.py` → wait → `screendump` → PNG.
- `atv/fbdump.py` (VRAM → PPM + ASCII preview) and `atv/fbboot.py` (earlier polling
  based patcher) are superseded by `fbpatch.py` but still useful for VRAM dumps.

### Where video goes next
1. With a real `fb0`, relaunch with `ATV_GPU_DISPLAY=vram` so the display scans out of
   VRAM; then the console (and anything else the OS draws) comes from guest pixels.
2. The firmware-side path (the loader's splash + `UGA Draw`) still needs the m63 driver
   to leave `EFI_NOT_READY`: `GetMode` fails because the driver's own device object
   reports "not ready", i.e. the NVIDIA init sequence (VBIOS init scripts, PMC/PBUS/
   PFB/PRAMIN/PCRTC) never completes against the register stub. The extracted VBIOS is
   `atv/work/m63-vbios.rom` (60 KB, `10de:01d7`, checksum OK, "GeForce Go 7300 VGA BIOS
   (Apple M63)") and carries the BIT tables (`B` bios-init, `I` init scripts, `M`
   memory config, `t` PLL/TMDS) that this model would have to satisfy.
3. HDMI: any faithful output model must drive the SiI1930CTU transmitter, not a VGA DAC.

---

## 2026-11-17 (later) — a window on the machine, and the disk/bootloader bring-up

### Watching the machine instead of inferring it
- `atv/atvboot.sh` takes **`ATV_DISPLAY=gtk|sdl|x11`** (default `none`) and adds
  **`-vga none`** whenever a backend is selected: otherwise the stock VGA device owns
  console 0 and the window shows a black 640x480 screen instead of the ATV GPU shim's
  console (0xB8000 text, or the BAR1 VRAM when `ATV_GPU_DISPLAY=vram`).
- `atv/watch.sh [--geom 1024x768x32]` is the one-command version: boot → POST 0xe4 →
  forced dispatch + PCI enumeration → `screen_info` stamp → window, then streams the
  serial log.  Ctrl-C stops the tail, not the machine.
- **`ATV_SERIAL=interactive`** turns the UART into a socket (`atv/run/serial.sock`) and
  starts `atv/serial.py serve`, which logs to `atv/run/serial.out` *and* lets
  `atv/serial.py send "cmd"` type into the guest.  This matters because the initramfs
  emergency shell has no tty (its stdin is the one-way ttyS0) — without the bridge the
  guest cannot be questioned at all.  `atv/probe-shell.sh` runs the standard
  disk/IRQ/PCI interrogation through it.
- `atv/bootcycle.sh` gained `ATV_QEMU_EXTRA` (extra QEMU args), a per-run pid-file guard
  (two overlapping cycles used to fold their reports together, because both rewrite
  `atv/run/*`), and copies the serial log to `atv/work/last-serial.out`.
  Do not edit `bootcycle.sh` while it is executing — bash reads scripts incrementally,
  and the byte shift turns the rest of the run into nonsense (`line 47: ... command not
  found`).

### What the boot disk actually is (`bootloader-v24-04.img`)
It is **KD3533/atv-bootloader** v24 — a *kexec* bootloader, not a distro installer:
- part1, LBA 2048 (256 MiB, HFS+, "boot"): `boot.efi` (298800 B), `mach_kernel`
  (18 MB), `BootLogo.png`, `com.apple.Boot.plist`
  (`Kernel Flags = video=vesafb root=LABEL=root modules=sd-mod,sr-mod,isofs,usb-storage,ext4
  quiet rootfstype=ext4`).
- part2, LBA 526336 (2048 MiB, ext4, **LABEL=`root`**, UUID
  `b3e9689f-a316-4c52-a25e-c195817e03d8`).
- `mach_kernel` is a Mach-O (`__TEXT` vm `0x2000000`, `0x1171000` bytes) wrapping a bzImage
  at file `0x5a20` (`setup_sects 29`, `code32_start 0x100000`, payload offset 0) whose gzip
  payload starts at file `0x96ce` and inflates to a 32-bit ELF kernel; the bootloader's own
  initramfs is appended at file `0x6276a0` (gzip → newc cpio, `0xb49270` bytes).
- The kernel is **5.15.145-0-lts** (built 2024-01-04, Alpine), the initramfs is
  **"Alpine Init 3.7.0-r1"**, and after mounting root it runs the bootloader's kexec GUI
  (root, empty password).  The loader sets `ramdisk_image=0x14b6000`,
  `ramdisk_size=0xb49270` and leaves `cmd_line_ptr` at its reset value `0x90000` (i.e.
  pointing into boot_params), which is exactly why `fbpatch.py` must supply the command
  line itself.

### Boot chain now verified end-to-end
`POST80 = 28 28 28 28 28 e4` → `session.sh` → `58 5a` → loader → kernel:
```
Linux version 5.15.145-0-lts ... Alpine Init 3.7.0-r1
Loading boot drivers: ok
[drm] Initialized simpledrm 1.0.0 for simple-framebuffer.0 on minor 0
Console: switching to colour frame buffer device 128x48     <- real fbcon, 1024x768
Mounting root: ... mount: mounting LABEL=root on /sysroot failed: No such file or directory
initramfs emergency recovery shell launched.
```
So video, the console, the loader and the initramfs all work; only the **block device is
missing**.

### The IDE prog-if trap (`!!`)
`hw/ide/piix.c` presents `8086:27df` (ICH7-M IDE) in ATV mode.  The prog-if value decides
whether Linux can use it at all:
- `0x8f` (**native**, the real controller's value): `pci_read_bases` finds the four
  hardwired BARs unsizable (`[Firmware Bug]: reg 0x10: invalid BAR (can't size)`), so
  `ata_resources_present()` is false for every port and
  `ata_pci_sff_init_host()` prints **`no available native port`** → `-ENODEV`, no disk.
  Native mode would also need a PCI interrupt, and this machine has **no PCI interrupt
  routing whatsoever** (`uhci_hcd 0000:00:1d.0: can't find IRQ for PCI INT A`).
- `0x8a` (**ISA-compat**, what the PIIX3 underneath really is): libata ignores the BARs and
  binds the legacy ports —
  `ata1: PATA max UDMA/100 cmd 0x1f0 ctl 0x3f6 bmdma 0xffa0 irq 14`.
  The firmware's own Ide driver is happy either way (POST `58 5a` is still reached), so
  nothing is lost.  Knob: **`ATV_IDE_PROGIF=0x8a`** (default `0x8f`), and the whole story
  is in the comment above `piix_ide_atv_progif()`.

### OPEN issue: the ports are up but no ATA device appears (`!!`)
With `0x8a` the controller binds and both channels are configured, yet libata never prints
`ata1.00`, no `lost interrupt`, no timeout — nothing happens at all, and the guest sits in
the emergency shell (or, later, HLT-idle).  Independently verified on the QEMU side:
- `info qtree`: `dev: piix3-ide` at `00:1f.1`, `pci id 8086:27df`, BAR4 `0xffa0`,
  `bus: ide.0` → `dev: ide-hd, unit = 0, drive = "d0"`.  The disk *is* on the controller
  Linux drives.
- `i /b 0x1f7` (monitor, host side) returns **`0x50`** = DRDY|DSC, i.e. the drive is ready
  and answering at the legacy ports.
- The firmware reads the same disk happily (it loads `boot.efi`/`mach_kernel` from it), so
  the model, the drive and the ports are fine.
- `-trace enable=ide_bus_exec_cmd` records **zero** commands after the firmware's own use:
  the kernel never issues IDENTIFY, so the probe is not running at all rather than failing.
⇒ Next: check whether the EH probe thread is running/blocked (`/sys/class/ata_port/ata1`,
`cat /sys/block/*/device/state`, `ps` on the guest), i.e. `ata_host_activate()` succeeded
(hsts 0/1 *are* registered and ata1/ata2 print) but `ata_bus_probe()` never got scheduled.
Candidate causes to check, in order: (a) the SCSI/ATA async workqueue never runs because the
machine has no usable timer for it (unlikely, the rest of the kernel is fine); (b) the port
was marked disabled by `ata_piix` (`hpriv`/IOCFG path reads config offset 0x54 which this
model only half implements); (c) `pdev->irq` is 14 but `request_irq(14)` silently shared
with something.  The interactive serial console (`ATV_SERIAL=interactive` +
`atv/probe-shell.sh`) is the tool that answers this directly.

---

## 2026-11-17 (even later) — PCI interrupts are honest: the ATV $PIR path works

### TL;DR
The guest now gets real PCI interrupts through the 8259.  Linux reports
`pci 0000:00:1f.0: PIIX/ICH IRQ router [8086:7000]`, routes `00:01.0 INT A -> IRQ 10`
and `00:1d.0 INT A -> IRQ 11`, binds `uhci_hcd 0000:00:1d.0: irq 11`, enumerates the
Apple IR receiver *and* a `-device usb-kbd` through a hub, and `/proc/interrupts` shows
the line **counting** (`11: 127 XT-PIC uhci_hcd:usb1`) — i.e. the UHCI really raises
interrupts that the 8259 delivers and Linux services.  Nothing is being polled.

### Two facts that decided the design (both measured, both surprising)
1. **POST `0xe4` is posted *before* the DRAM pattern sweep, not after.**  Sampling
   0xf0000 while booting shows the sequence: all six POST bytes
   (`28 28 28 28 28 e4`) in the log at ~1 s, then the sweep overwrites the F-segment at
   ~2 s, then the firmware parks in the BDS `Stall()` loop.  So "POST 0xe4 appeared" is
   **not** a readiness signal — `atv/bootcycle.sh` now waits for the machine's own
   memory-settled marker (the $PIR table appearing, see below) before running
   `session.sh`.  Poking the guest in that window is what made an early run report
   `started 0 drivers`.
2. **The firmware's sweep covers the whole F-segment *and* the EBDA.**  Sampled with
   `atv/memsample.py` (multi-address HMP sampler): `0x9fc00`, `0xf0000` and then
   ascending memory all take the pattern within ~150 ms of each other, and nothing ever
   writes there again.  So there is **no** legacy address that survives from reset — a
   $PIR table (or an RSDP/MP table) has to be published *after* the sweep, not at machine
   init.  This also kills the "RSDP in the EBDA" idea.

### What the machine does (`hw/isa/piix.c`, `include/hw/southbridge/piix.h`)
- ICH7-faithful **eight-line PIRQ router** in the PIIX, gated on the `atv-pirq` property
  (set from `car_size > 0`): route registers `0x60..0x67`, PIC path when bit 7 is clear,
  plus a permanent IO-APIC path on **GSI 16+n** (`atv-pirq-apic` GPIOs → `x86ms->gsi[16..23]`,
  matching the real DSDT `_PRT`'s `PIRQA=16 … PIRQG=22`).
- Reset programs **PIC-mode** routes (`10, 11, 5, 7, 3, 11, 10, 5`) — what a BIOS for a
  PIC-only OS would leave.  The stock PIIX3 reset value `0x80` ("APIC only") is exactly
  why a guest with no MADT got no PCI interrupts at all.
- Slot → PIRQ wiring is transcribed from the firmware's own DSDT `_PRT` into
  `atv_pirq_wiring[]` / `piix_atv_slot_pirq()` (slot-granular, like the DSDT's `0x00NNFFFF`
  function wildcard).  Unrouted slots keep the i440FX swizzle `(pin + slot - 1) & 3`, and
  **the $PIR table applies the same fallback**, so table and `pci_bus_map_irqs()` can never
  disagree (they did, briefly: a zero-initialised array made every unrouted slot claim
  PIRQA).
- The **$PIR** table (528 B at `0xf0000`) advertises router `8086:7000` at `00:1f.0`;
  `intel_router_probe()` accepts that as "PIIX/ICH", and the link bytes are plain config
  offsets (`0x60+n`), one bitmap bit per link naming the IRQ it is programmed to.
- **Publication is late and self-healing**: a 100 ms `QEMU_CLOCK_VIRTUAL` timer compares
  the head of the table window plus the last 16 bytes of the F-segment against its last
  sample and writes the table only when the window has been quiet for a whole period.
  That is what protects the firmware's write→verify pair (a write of ours landing inside
  it would fail the DRAM test outright, i.e. the old `0x80000007` dead end) while still
  landing the table before any OS scans for it.  It also means a later sweep (e.g. while
  forcing more DXE drivers) cannot permanently eat the table.

### Verified boot recipe (IRQ demo, ~5 min)
```
ATV_IDE_PROGIF=0x8a ATV_QEMU_EXTRA="-device usb-kbd" atv/bootcycle.sh --wait 120 -- \
  --cmdline "console=ttyS0,115200 ignore_loglevel root=LABEL=root rootfstype=ext4 \
             modules=sd-mod,sr-mod,isofs,usb-storage,ext4 loop squashfs simpledrm" \
  --cmdline-ptr 0x98000
```
`--wait`/`fbpatch` are only needed to *see* the kernel: without the `--cmdline` patch the
loader leaves `cmd_line_ptr` at 0x90000 (pointing into `boot_params`) and the kernel boots
**silently** — it still reaches the initramfs, it just has no console, which reads like a
hang.  With `ATV_SERIAL=interactive`, `atv/serial.py send "cat /proc/interrupts"` asks the
guest directly; `atv/probe-shell.sh` does the whole interrogation.

### Still open (unchanged by this work)
`ata1`/`ata2` register but the disk is never probed (`ide_bus_exec_cmd` = 0 commands), so
`mounting LABEL=root` fails and the initramfs shell comes up.  Now that PCI INTx routing is
real, the native-mode variant (`ATV_IDE_PROGIF=0x8f`, where libata needs PIRQC → IRQ 5)
is worth retrying — it was previously dismissed for un-sizeable BARs, which is a separate
model bug (`hw/ide/piix.c` hardwired BARs).

---

## 2026-11-18 — USB boot works end-to-end (loader + kernel from a USB stick)

### The missing piece was the UHCI BAR, not the firmware (`!!`)
The firmware's `UsbUhci` driver did install `EFI_USB_HC`, but nothing ever appeared
behind it, so there was no `BlockIo`/`SimpleFileSystem` to boot from.  The reason is the
same resource hole as the root bridge: **no PCI resource allocation ever runs on this
machine**, so every BAR that is not hardwired reads 0.  The UHCI (`00:1d.0`,
`8086:27c8`) has soft BARs — its BAR4 read 0, the register window was unmapped, and the
driver's MMIO/I-O writes went nowhere (reads of `USBCMD`/`USBSTS` returned 0xff, i.e.
"controller absent"), so it never enumerated a port.

Fix (`hw/i386/pc_piix.c`, ATV block): leave **BAR4 = `0xcc00` (I/O space)** and
`PCI_COMMAND = IO|MASTER` programmed, the way a BIOS would have.  Linux then reports

```
pci 0000:00:1d.0: [8086:27c8] type 00 class 0c0300
pci 0000:00:1d.0: reg 0x20: [io 0xcc00-0xcc1f]
uhci_hcd 0000:00:1d.0: irq 11, io base 0x0000cc00
```

### Reset-ordering rule (why an init-done notifier is required) (`!!`)
Programming the BAR once while the device is created does **not** work:
`pci_do_device_reset()` clears the command register and `pci_reset_regions()` rewrites
each BAR with its type bits only, and that reset runs between machine init and the first
guest instruction.  So the value has to be re-applied by a reset handler — and the
handler can only be registered once the device tree exists, which is
`qemu_machine_creation_done()`.  Hence:

```c
atv_uhci = uhci;                                   /* in pc_init1()'s ATV block   */
qemu_add_machine_init_done_notifier(&atv_uhci_bar_notifier);
/* notifier -> qemu_register_reset(atv_uhci_bar_reset, atv_uhci);              */
/* atv_uhci_bar_reset(): pci_set_long(BAR4, ATV_UHCI_IO_BASE|SPACE_IO) +
 *                       pci_default_write_config(PCI_COMMAND, IO|MASTER, 2); */
```

`atv_uhci_bar_reset()` / `atv_uhci_bar_notify()` sit above `pc_atv_slot_get_pirq()` and
the notifier struct is file-static.  Any future pre-programmed BAR on this machine needs
the same treatment; a one-shot write is silently undone.

### Verified USB-only boot (no IDE disk at all)
```
ATV_BOOTDEV=usb atv/bootcycle.sh --wait 200 -- \
  --cmdline "console=ttyS0,115200 ignore_loglevel root=LABEL=root rootfstype=ext4 \
             modules=sd-mod,sr-mod,isofs,usb-storage,ext4 loop squashfs simpledrm" \
  --cmdline-ptr 0x98000
```
`ATV_BOOTDEV=usb` swaps the `ide-hd` for `-device usb-storage,...,removable=on`, i.e. the
real-hardware "bootloader on a USB stick, nothing on the IDE cable" setup.  Result:
- POST80 runs `28 e4 58 93 92 90 95 5a bb f8 f9`, then the alternating `00 ff` stream
  (Linux's `io_delay` poking port 0x80) — i.e. the firmware's boot manager really did
  hand off to `boot.efi`, which loaded `mach_kernel` off the stick.
- `0x14b6000` no longer holds the DRAM pattern (`5a a5 ff 00 b4 4a ff 01`); the loader
  put the real initrd there (`RAMDISK: [mem 0x014b6000-0x01ffffff]`).
- Serial: Alpine initramfs binds `usb-storage`, `sd 0:0:0:0: [sda] ... sda1 sda2`,
  mounts `EXT4-fs (sda2)` = `LABEL=root`, `Mounting root: ok.`, OpenRC starts, then
  `Console: switching to colour frame buffer device 240x67` (the stamped 1920x1080
  simpledrm console).

### Where it stops now: the loader's own GUI, on tty0 only (`!!`)
After OpenRC, `/etc/local.d/atv.start` (from the stick's ext4 root) redirects
stdin/stdout/stderr to **`/dev/tty0`** and runs `/mnt/bootloader.sh` from the HFS+
partition — a `dialog`-based kexec menu.  Everything it prints goes to the framebuffer
console, so the **serial log goes silent** at that point (it looks like a hang; it is
not).  Consequences for the harness:
- Read the machine's output with `ATV_GPU_DISPLAY=vram` + `screendump` (the guest draws
  into the stamped BAR1 framebuffer), not with the serial file.
- Drive the menu with `ATV_QEMU_EXTRA="-device usb-kbd"` (tty1..tty6 gettys also live on
  fbcon), or with `ATV_SERIAL=interactive` for anything that still uses ttyS0.
- `mount -t hfsplus` is what puts `bootloader.sh` at `/mnt`; the rootfs carries
  `hfs/hfsplus/fat/vfat` modules, so the mount works as long as the kernel can call
  `/sbin/modprobe`.  A failed mount just prints "No script on partition one found" to
  tty0 — invisible over serial.

⇒ The Apple TV firmware now boots a real OS to userspace on QEMU, both from IDE and from
USB.  The next frontier is no longer "does the firmware dispatch a driver" but the
loader-side UI/video path (`bootloader.sh`'s dialog menu, and beyond it the m63 VBIOS /
UGA work).

## 2026-11-18 (later) — the full chain runs, kexec included

### The firmware really sees the USB stick (`!!`)
`session.sh` now reports `964e5b21-... BlockIo inst=1` on a USB-only boot, next to the
usual `964e5b22 SimpleFileSystem inst=0`.  That is the firmware's *own* protocol
database, i.e. the UHCI BAR fix did not merely make QEMU's emulation work — the
firmware's `UsbUhci`/`UsbMassStorage` stack enumerated the stick and installs a real
EFI_BLOCK_IO instance on it.  (SimpleFileSystem appears later, when an FS driver binds;
the loader is loaded through the block device.)

### Driving the loader menu from the harness (`!!`)
The atv-bootloader GUI is a `dialog` menu.  With `-device usb-kbd` attached, HMP
`sendkey` drives it headlessly:

```
atvmon.py 'sendkey ret'      # "Select Linux Kernel" -> "Select Linux Initrd" -> "Select commandline"
atvmon.py 'screendump "atv/work/gui.ppm"'
```

The menus are `/boot/vmlinuz-lts` (kernel), `/boot/initramfs-lts` (initrd) and
`Reuse` (command line, i.e. keep the `console=ttyS0,...` the patch installed).  Choosing
all three runs `kexec -l ... --reuse-cmdline` + `kexec -e`, and the serial log then holds
**two** boots:

```
[  424.406853] kexec_core: Starting new kernel
[    0.000000] Linux version 5.15.145-0-lts ...
[    4.609716] sd 0:0:0:0: [sda] Attached SCSI removable disk
[    8.833078] Mounting root: ok.
```

so the complete ATV1 chain — firmware → `boot.efi` → `mach_kernel` → initramfs → root →
loader menu → `kexec` → second kernel — works under QEMU.  (`sendkey` takes effect ~20 s
later under TCG; screenshot after each step instead of guessing.)

### Watching it live
`ATV_DISPLAY=gtk ATV_GPU_DISPLAY=auto` (see the GPU-shim table above) gives one window
that follows the boot: the firmware's text console, then the Linux framebuffer with the
dialog.  Use `ATV_QEMU_EXTRA="-device usb-kbd"` to type into it.

### floppy: `atv/fvsearch.py` — who in the flash mentions a GUID at all
`guidprov.py` only looks at DRIVER files and prints DEPEX strings; `fvsearch.py` walks
**every** FFS file of every volume (SEC/PEI_CORE/PEIM/DRIVER/APP/FREEFORM), unwraps
COMPRESSION/GUID_DEFINED and counts raw occurrences per file.  Use it to separate
"installs this protocol" from "lists it in a DEPEX":

```
atv/venv/bin/python atv/fvsearch.py --guid 3f557189-...,f496922d-...
```

Result for the four GUIDs that block the arch drivers (`3f557189`, `f496922d`,
`5572e711`, `f33261e7`): they occur **only** in DEPEX sections plus a single consumer
site — `fv000000 idx46 240612b5-a063-11d4-9a3a-0090273fc18e`, an **APP** that does
`gBS->LocateProtocol(&f33261e7)` (`0x1000186d: push $0x10002fc0; call *0xac(%eax)`).
No file of any type installs them, so they cannot come from the DXE volume — they are
IBI/platform-layer protocols (or SMM), which is consistent with the older conclusion
that the RUNTSERV/phd0 engine never arms.

## 2026-11-18 (evening) — watching the machine: auto display, frame skipping, watch.sh

### `ATV_GPU_DISPLAY=auto` had to become sticky and sample the whole aperture (`!!`)
The first version of `auto` decided "the OS is drawing" by looking at the *first 64 KiB*
of BAR1.  That works for a text console (it draws from the top) but not for the loader's
`dialog` menu, which clears the screen and paints **in the middle**: the head of the
aperture stays zero, the heuristic flipped back to the (now empty) text console, and the
window went black *while the dialog was on screen*.  Verified by dumping the aperture:
`pmemsave 0x10000000 0x4000000` had its first non-zero byte at `+0x30cba0` (the dialog's
top border, `415 * 7680`), i.e. several MiB in.
Fix: scan the whole aperture at a 4 KiB stride and make the decision **sticky**
(`s->vram_seen`), because a graphical program that clears the screen must not make the
console switch sources again.

### Frame skipping (`!!`)
Re-copying the aperture and re-uploading a 1920x1080 texture ten times a second costs more
host CPU than the guest does under TCG, and it made a windowed boot feel much slower than
a headless one.  `atv_nvidia_render_if_changed()` now renders only when the source
changed:
- text mode: FNV-1a signature of the 80x35 (or `ATV_GPU_TEXT`) cells at `0xB8000`;
- VRAM mode: `memory_region_snapshot_and_clear_dirty(..., DIRTY_MEMORY_VGA)` — the shim
  already enables that dirty log on the aperture (`memory_region_set_log`), so a frame
  whose pages are all clean is skipped entirely.
`screendump` still renders on demand (the `gfx_update` op calls `atv_nvidia_render()`,
not the "if changed" variant), so a headless capture never returns a stale frame.

### Watch it with one command
```
atv/watch.sh                      # GTK window, USB boot, 1024x768x32, dialog at the end
ATV_BOOTDEV=ide atv/watch.sh      # same, but the loader comes off the ICH7 PATA disk
atv/watch.sh --wait 300 --shot atv/work/shot.ppm
```
It stamps `screen_info` **and** a `console=ttyS0` command line by default (without them the
kernel boots silently and the window stays blank until userspace draws), and it exports
`ATV_GPU_DISPLAY=auto` + `ATV_GPU_GEOM` so the shim's scanout matches the stamped mode.
1024x768 is the fast choice: fbcon scrolling copies a whole line of the aperture on every
scroll, so 1080p costs 3.5x more guest time per scroll under TCG.

### The boot picture, stage by stage
1. firmware text console at `0xB8000` (the shim's `text` source) — mostly blank, the
   firmware only posts codes;
2. loader text (same source) while `boot.efi` walks the partitions;
3. kernel switches to the stamped linear framebuffer → `auto` flips to VRAM and the boot
   log appears in 128x48 (1024x768) or 240x67 (1080p);
4. `atv-bootloader`'s `dialog` menu (Select Linux Kernel / Initrd / commandline), which
   `sendkey` or the GTK keyboard can drive — verified end to end, including the `kexec`.

### No splash yet, and that is a *firmware* gap, not a display gap (`!!`)
The loader's `BootLogo.png`/UGA splash never appears because the firmware's NVIDIA UGA
driver (`fv000000 idx120 3481c745-...`, "NVIDIA GPU Driver", strings `UGA_DRAW_HANDLER`,
`gRM_GRAPHICS`, `FRAME_BUFFER_INFO`) never finishes its init: its `GetMode` reports the
device as not ready, so `boot.efi` has no UGA and draws nothing.  The QEMU side already
scans out the aperture, so **the missing piece is the driver's init sequence against the
BAR0 register model** — see the GPU log tool:

```
atv/gpulog.sh            # boot + forced dispatch cascade with ATV_GPU_LOG, then report
```

It captures every BAR0 access ("last value wins" sparse model, `ATV_GPU_LOG`), then prints
the most-polled addresses (a repeated read of one register is the driver waiting on a bit)
plus the first/last accesses.  That trace is the input for modelling PMC/PBUS/PFB/PRAMIN/
PCRTC well enough for the driver to reach `SetMode`; because the shim scans BAR1 out
directly, a successful mode set only needs to leave the pixels in VRAM — the PLL/TMDS
programming can be modelled as "writes accepted, lock bits read back as locked".



## 2026-11-18 (night) — software UGA draws; USB boot is the default; ConsoleControl is the gate

### The fake UGA driver works end to end (`!!`)
`atv/ugablob.c` (built and installed by `atv/ugafake.py`) replaces the three methods of the
firmware's dead `EFI_UGA_DRAW_PROTOCOL` with a linear framebuffer in the BAR1 aperture.
Proven, not assumed:

```
atv/venv/bin/python atv/ugafake.py test      # paints 8 colour bars + 1 stripe
atv/venv/bin/python atv/ugafake.py events    # the ring shows exactly those 9 calls
```

The bars appear in the GTK window, because the call chain is real: firmware `Blt` →
blob at `0x13F00000` → aperture `0x10000000` → `pc_piix.c` scanout.  Blob symbols (stable,
`ugafake.py build` prints them): `uga_get_mode+0x4b`, `uga_set_mode+0xf5`, `uga_blt+0x197`,
`cc_get_mode+0x460`, `cc_set_mode+0x48f`, `cc_get_mode_info+0x4c7`.

- State/event block: `0x13FFE000` (`'UGA1'` + w/h/depth/base/pitch + counter `+0x20` +
  64 × 16-byte ring at `+0x40`).  Event tags: `0x100` get, `0x200` set, `0x300|op` blt,
  `0x400/0x500/0x600` ConsoleControl GetMode/SetMode/GetModeInfo.
- The QEMU shim prefers the guest's `'UGA1'` header over `ATV_GPU_GEOM`, so `SetMode`
  really resizes the window; `fbpatch.py` retires the header when the kernel takes over.
- **`ugafake.py status/events` read the state through the GDB stub, i.e. virtual
  addresses.** They work while the firmware runs (paging off) and fail with `E14` once
  Linux has paging on.  Then dump physically instead:
  `atvmon.py 'pmemsave 0x13ffe000 0x800 "atv/work/s.rs"'`.

### The atv-bootloader's boot.efi still never calls UGA (`!!`)
With the instance installed and patched *and* ConsoleControl repointed, a full USB boot
leaves the event counter at **0**.  So its graphics init never reaches the framebuffer
lookup.  What the binaries actually reference (GUID scan of the extracted EFI files):

| file | protocols referenced |
| --- | --- |
| diags `System/Library/CoreServices/boot.efi` (indy-tool) | ConsoleControl, SimpleFileSystem, DevicePath, LoadedImage, DriverBinding |
| diags `GUI/Gui.efi` | **UGA Draw (Apple)**, SimpleFileSystem, DevicePath, LoadedImage, CpuArch |
| atv-bootloader `boot.efi` | **UGA Draw (Apple)**, **ConsoleControl**, SimpleFileSystem, BlockIo, FileInfo |

so the Apple order is `ConsoleControl.SetMode(graphics)` → `LocateProtocol(UGA Draw)` → draw.

### ConsoleControl exists already — patching it, not adding one (`!!`)
The firmware installs its own `EFI_CONSOLE_CONTROL_PROTOCOL` (`f42f7782-012e-4c12-9956-
49f94304f721`); on a 61-driver boot the database shows it on handle `0e18e910`
(iface `0edc48e4`, methods `0edc393d/0edc3993/0edc32cf`).  **`LocateProtocol` hands out the
first instance**, so installing an extra handle of our own is invisible to the loader.
`ugafake.py cc` therefore **repoints the existing instance's three methods** at the blob
(originals saved at state `+0x34/+0x38/+0x3c`) and only calls `InstallProtocolInterface`
when no instance exists at all.

### USB is the boot path now (`!!`)
- `atv/bootcycle.sh` defaults to `ATV_BOOTDEV=usb`; `atv/ugaboot.sh` gained the same knob
  (it used to hardcode `ide-hd,bus=$ATV_IDE_BUS`).  Both use
  `-drive ...,if=none,id=usb0,format=raw,snapshot=on -device usb-storage,drive=usb0,removable=on`.
- Why: the ICH7 PATA model cannot be driven by libata (`no available native port`, no
  block device, root mount fails), while the UHCI stick gives `sda`/`sda1`/`sda2` and
  `EXT4-fs (sda2) ... Mounting root: ok.`  It is also how Apple's field diagnostics run
  on the real box.
- `atv/watch.sh` now passes `console=tty0` first (else fbcon never takes over the stamped
  framebuffer and the window stays black while the serial log is complete) and streams the
  serial log with `tail -F` (a plain `-f` latched onto the previous run's deleted inode and
  printed a stale boot).

### New/updated tools
- `atv/bslog.py` — logs `gBS->{HandleProtocol,OpenProtocol,LocateHandleBuffer,
  LocateProtocol}` calls (GUID, caller, args; `--only`, `--all`, `--guid`, and per-GUID
  return status for watched GUIDs).  It locates `gBS` through `corebase.find(r)`, because
  the old hardcoded `0x0ee55aa0` is only true for one boot — a stale table silently makes
  every breakpoint bogus (0 hits).  Note the cdecl argument order: `LocateProtocol` takes
  the GUID first, the others second.
- `atv/diagsboot.sh` — boots a volume (default `atv/work/fd-prim-rec.img`, the Apple field
  diagnostics) over USB with the software UGA + ConsoleControl installed, screenshots
  every 10 s into `atv/work/diagshots/` and decodes the event ring at the end.
- Boot-manager context: before it starts a volume's `boot.efi`, BDS spins on
  `LocateProtocol(08f9977d-1141-4e84-9376-5706ccc388ad)` from caller `0x0ee0a7ad`
  (hundreds of hits); that flood is what a naive `bslog.py --all` run shows first.

---

## 2026-11-18 (night 2) — the picture is on screen: UGA `Blt` had BltBuffer in the wrong slot

### TL;DR
The framebuffer was never broken.  The software UGA blob had the **wrong `Blt`
prototype**, so every draw returned `EFI_UNSUPPORTED` and painted nothing.  With the
prototype fixed the firmware paints its Apple TV splash (`atv/work/shot-uga-ok.png`,
1024x768: black screen, white "?" over the Apple TV box) and the QEMU window shows it.

### The prototype bug (`!!`)
`EFI_UGA_DRAW_PROTOCOL::Blt` declares **`BltBuffer` second, before `Op`**:

```
Blt(This, BltBuffer, Op, SourceX, SourceY, DestinationX, DestinationY,
    Width, Height, Delta)          /* 10 args; Delta = buffer row stride in bytes */
```

The blob had `(This, Op, SrcX, SrcY, DstX, DstY, W, H, Delta, BltBuffer)`, i.e. every
argument after `This` shifted by one — `op` was read from the *pixel pointer* the caller
passes, so all 13 015 blits of the previous run fell into `default:` and set
`OSTATUS = 0x80000003`.  That `UNSUPPORTED` was the whole "black screen".

Measured live, not inferred — new tool **`atv/bltargs.py N`** sets a hardware breakpoint
on the blob's `uga_blt` (`0x13F00000+0x22c`) and dumps the real frame:

```
ret=0ed3c7c4  arg1(This)=0e3d6090  arg2(BltBuffer)=0db70010  arg3(Op)=2
arg4(SrcX)=10 arg5(SrcY)=10 arg6(DstX)=10 arg7(DstY)=10 arg8(W)=13 arg9(H)=0x12
arg10(Delta)=0x1000            [0db70010] = 9c636300 9c636300 ...
```
`Delta = 0x1000 = 1024*4` is the *buffer* pitch: the caller keeps a full 1024x768 shadow
screen in RAM and blits rectangles (src == dst coordinates) into the device.  So
`Op 2 = BltBufferToVideo` must read `buf + (SrcY+y)*Delta + (SrcX+x)*4`; the old code
ignored SrcX/SrcY there.

### Proof chain that isolates the bug (all measured)
1. `pmemsave 0x102e0000 0x20000` (bottom 32 scanlines of BAR1) contained exactly the
   blob's selftest bars (`ffffff, ffff00, ff00ff, ff0000, 00ffff, 00ff00, ...`) and
   `screendump` rendered them ⇒ guest stores land in the aperture **and** the shim scans
   it out.  Nothing was ever wrong with BAR1/`pc_piix.c`.
2. Therefore the only thing left was "does the guest ever ask for pixels" — it did
   (13 015 blits), and the blob rejected every one.
3. After the prototype fix: `bad-op 0`, `last status 0`, and the aperture **byte-for-byte
   equals the caller's shadow buffer** (both 9823 non-black pixels, identical colour
   histogram) ⇒ `Blt` is now a faithful copy in both directions.

### What the firmware actually draws
The first call is a full-screen `Blt(0,0,1024x768)` from the shadow buffer, then
repeated 154x153 blits at (435,307) — that is the Apple TV "?" + console graphic, i.e.
the boot manager's *no bootable volume* splash.  Still greyscale-only, so the
UGA/Blt path is exercised but nothing renders a colour UI yet.

### Reproducing the diags USB image (atv/work was wiped, `dmg2raw.py` is broken for it)
`atv/dmg2raw.py` dies with `ValueError: subsection not found` on
`693-6420-A.9999.dmg`; `build/qemu-img convert -f dmg -O raw` works and yields the
**bare HFS+ volume** (449 220 608 B, `H+` at offset 1024, no partition table):
```
build/qemu-img convert -f dmg -O raw .../693-6420-A.9999.dmg /tmp/opencode/atv/fd.raw
atv/venv/bin/python atv/mkgpt.py /tmp/opencode/atv/fd.raw atv/work/fd-prim-rec.img
atv/venv/bin/python atv/atvdisk.py atv/work/fd-hfs.img --src atv/work/fd-prim-rec.img \
    --part 1 --type 48465300-0000-11aa-aa11-00306543ecac --name "Apple TV Diags"
```
`mkgpt.py` defaults to the Apple "Recovery" type that `restore_diags` writes; BDS skips
Recovery-typed volumes while selecting a boot device, so `atvdisk.py` retypes the
partition to "HFS" when the volume itself must be booted (`ATV_DISK=atv/work/fd-hfs.img`).

### Tooling notes
- `atv/atvboot.sh` now always passes **`-vga none`** (the stock VGA owned console 0 and
  could make `screendump`/window show a black 640x480 screen).
- `atv/ugablob.c` state (`0x13FFE000`): ring 32x32B at +0x40, linear trace 24x32B at
  +0x440, stats at +0x740 (`traces/fills/bad-op/last-op/selftest`), ConsoleControl GUID
  copy at +0x800.  Decode a dumped state with
  `atv/venv/bin/python atv/ugafake.py status --file <dump>` /
  `... events --file <dump> [--only-trace]`.
- No PIL in `atv/venv`: `diagsboot.sh`'s PPM→PNG step silently skips.  Convert with a
  ~10-line zlib/PNG writer (see the session log) or just `read` the PPM bytes.
  `screendump` writes the shim's console (1024x768), which is the honest picture.

---

## 2026-11-18 (night 3) — CPU identity from microcode, and SPD is NOT in the flash

### The flash carries 11 Intel microcode updates (new tool `atv/ucode.py`)
A dedicated 48 KiB nested firmware volume at **flash `0x194000-0x1a0000`**
(`_FVH` at `0x194028`, length `0xc000`) holds a flat microcode container:

```
atv/venv/bin/python atv/ucode.py list          # table below
atv/venv/bin/python atv/ucode.py extract --sig 0x6d8     # -> atv/work/ucode/
atv/venv/bin/python atv/ucode.py extract --out atv/work/ucode-all
```

| flash off | sig | model | rev | date (MMDDYYYY as stored) | size |
| --- | --- | --- | --- | --- | --- |
| 0x194100 | 0x06F1 | Merom/Core 2 | 0x12 | 2005-11-29 | 0x1000 |
| 0x195100 | 0x06F4 | Merom/Core 2 | 0x21 | 2006-01-05 | 0x1000 |
| 0x196100 | 0x06F5 | Merom/Core 2 | 0x36 | 2006-05-23 | 0x1000 |
| 0x197100 | 0x06F6 | Merom/Core 2 | 0x48 | 2006-06-13 | 0x1000 |
| 0x198100 | 0x06F2 | Merom/Core 2 | 0x51 | 2006-07-13 | 0x1000 |
| 0x199100 | 0x06E4 | Yonah/Core Duo | 0x26 | 2005-08-16 | 0x1000 |
| 0x19A100 | 0x06E8 | Yonah/Core Duo | 0x39 | 2005-11-15 | 0x1000 |
| 0x19B100 | 0x06EC | Yonah/Core Duo | 0x54 | 2006-05-01 | 0x1000 |
| 0x19C100 | 0x06D6 | **Dothan/Pentium M** | 0x18 | 2004-10-17 | 0x0800 |
| 0x19C900 | 0x06D8 | **Dothan/Pentium M** | 0x20 | 2004-07-22 | 0x0800 |
| 0x19D100 | 0x06D8 | **Dothan/Pentium M** | 0x21 | 2006-08-31 | 0x0800 |

All 11 pass the standard "every dword sums to zero" checksum.  The Dothan entries have
their `datasize`/`totalsize` header fields zeroed (0x800 stride instead), so `ucode.py`
recovers the size from the container layout and *then* verifies the checksum rather than
trusting the header — that is why a naive header scan finds nothing.  The Yonah/Merom
updates are baggage from the shared Insyde base (they are what makes a "rip the microcode"
attempt confusing): the ATV itself is **family 6 model 0x0D, 32-bit only**.
Corroboration: the flash's variable-default area (`0x1a0000-0x1aac00`, ~180 `aa 55`
records in the live store `0x1aac00-0x1ae000`) carries `0x000006D0` (fam6 model 0x0D,
step 0) in ~50 record headers, and the Dothan ACK in the microcode container.

### System-RAM SPD is **not** in the flash — the firmware reads it over SMBus (`!!`)
Both RAM kinds are soldered on ATV1, so "the SPD must be in the BIOS" is a natural guess.
Measured, it is false for system RAM:
- A JEDEC DDR2 SPD search over the raw 2 MB image **and over every decompressed FV
  payload** (every byte offset, `/tmp/opencode/atv/spdscan.py`) returns **0 candidates**.
- Instead the PEI memory init programs the ICH7 SMBus and probes it: `00:1f.3` config
  `@0x20 <- 0xEFA1/0xEFA0` (SMB_BASE), `@0x4 <- 1`, `@0x40 <- 0x1` (HST_EN), and then does
  **exactly one** write + one read of `0xEFA0` (HST_STS) before continuing with its
  hardcoded timings.  It sits in the middle of the MCHBAR DDR sequence (offsets 0x96,
  0x52, 0x9c, 0x9d, 0x9e, 0xe7), so it is the DRAM init, not a later driver.
- Under QEMU that I/O window does not exist (our `piix4-pm` model keeps the SMBus at the
  PIIX4 location `0x90`, ICH7 uses `0x20`), so the probe reads `0xFF` and the firmware
  falls back.  ⇒ **Modelling the ICH7 SMBus BAR at 0x20 plus `smbus-eeprom` at 0x50/0x51
  with a DDR2 SPD is a real fidelity gap and an untested lever on DRAM init.**
- For VRAM the opposite holds: the G72M's memory configuration lives in the VBIOS inside
  FV idx120 (`3481c745`) — the BIT `M` tables of `atv/work/m63-vbios.rom`, which is what
  the driver/newveau program the memory controller with.  Nothing SPD-like is needed.

### New QEMU knob: `ATV_IOPORT_LOG=1` (`system/ioport.c`)
Logs every access to an **unassigned** I/O port (`atv-ioport: read/write port=0x… size=…
val=0x…`), which is how the `0xEFA0` probe above was found.  It also exposes the other
hallucinated windows: ports `0x72/0x73` (ICH7 RTC/NMI upper bank, 1400+ accesses — PIIX3
does not model them), and one-shot bursts at `0x44x/0x46x/0x50x-0x53x/0x68c-0x690/0x164e-
0x164f` = drivers computing `base+offset` from BARs that read 0.  Unassigned accesses are
rare, so the log stays small (4.3 k lines for a full boot).

---

## 2026-11-19 — real-hardware ground truth: CPU is Dothan, 0xf0000000 is MMCONF

### CPU settled from the physical box (`!!` — no more Dothan-vs-Yonah guessing)
`atv-devicetree/board/cpuinfo.txt` is a live capture:
```
cpu family 6  model 13 (0x0D)  stepping 8  microcode 0x21
model name "Genuine Intel(R) processor 1.00GHz"  cache 2048 KB  32-bit physical
flags ... tm pbe nx bts cpuid est tm2        (no sse3, no lm => Pentium M, not Yonah)
```
CPUID is **`6D8` (Dothan)** and `microcode 0x21` is *exactly* the third Dothan blob in
the flash (flash `0x19D100`, `6D8 rev 0x21`, 2006-08-31) — the firmware's microcode set
matches the installed CPU.  Yonah would be model 14 / `6Ex` with SSE3.

### Host-bridge config from real HW (`atv-devicetree/pci/cfg-00:00.0.txt`, decoded)
```
00: 86 80 a0 27 ... 03     8086:27a0 Mobile 945GM/PM/GMS rev 03
40: EPBAR=0xfed19001   44: MCHBAR=0xfed14001   48: PCIEXBAR=0xf0000005
4c: DMIBAR=0xfed18001  50: 0x00020000          54: DEVEN=0x00000083
90: PAM0=0x30, PAM1..6=0x33  9c: TOLUD=0x10 (256 MB)  98: 0x000003ff
e0: CAPID0=0x91090009  e4: CAPID0+4=0xa10a2503 (== what our stub already seeds)
```
`iomem` on the box reports `PCI MMCONFIG 0000 [bus 00-3f]` (64 MB) — i.e.
**PCIEXBAR=0xf0000000**.

### ⇒ CORRECTION 7 retired: `0xf0000000` is MMCONF, not an "Apple NVRAM store"
The firmware writes `0x48 <- 0xf0000005` under QEMU too (see the `ATV MCH cfg WR` log),
i.e. identical to real hardware.  The "decoded platform structure at 0xf0000000+0x100,
4 KiB stride, 16-bit fields, 0xffff = empty" described in CORRECTION 7 is a **PCI
function walk** through MMCONF (1 MiB/bus, 32 KiB/device, 4 KiB/function, vendor
`0xffff` = no device).  Consequences:
- `ATV_NVRAM=flash` was never "feeding NVRAM": it fed flash bytes into memory-mapped PCI
  config space.  Do not use it.
- The atv stub's `atv-mchbar1`/`atv-nvram` region at `0xf0000000` (2 MB) only survives
  because i440fx has no MMCONF of its own; the fork also has a real `atv-mmcfg` window
  (`hw/pci-host/i440fx.c`) which is what the firmware actually uses.

### Real topology vs our `-machine pc,car-size=` disguise
| | real ATV1 | ours |
| --- | --- | --- |
| PEG root port | `00:01.0`, GPU behind it at **`01:00.0`** | PIIX3 occupies `00:01.0` |
| IGD | `00:02.0` present (DEVEN bit set), G72M is the only GPU | `atv-nvidia-gpu` at `00:02.0` |
| HD audio | `00:1b.0` ICH7 | absent |
| EHCI | `00:1d.7` | absent (UHCI at `00:1d.0` only) |
| SMBus | `00:1f.3` (ICH7, config `0x20`) | PIIX4 PM at `0x90` |
| DEVEN | `0x83` (D0F0 + D1F0 + D2F0) | i440fx has no DEVEN semantics |

DMI identifies the board as `AppleTV1,1 / Mac-F4228DC8`, firmware
`ATV11.88Z.00DA.B04.0912220919 12/22/09`.

### coreboot: `model_6dx` must be restored, it is not just "a new select"
`src/cpu/intel` in coreboot main has **no `model_6dx` and no socket-479**; commit
`cpu/intel: Remove models 69x and 6dx` (Nico Huber, 2019-02-27,
`I0cf7ece028baa6750b79f54d615e93e452aff2e1`) dropped them with "Socket 479 is not
supported anymore".  So the `apple/atv1` port = pre-2019 `model_6dx` + socket-479
Kconfig restored, plus the `i945_macs` variant.  `i945` raminit itself is fine with this
CPU: `fsbclk()` decodes 400 MHz for the I945GM subtype (`CLKCFG & 7 == 0`).

### New tool `atv/nvram.py` — the flash variable store parsed properly
Record format (verified against `MemoryTypeInformation` and `MTC`, which cross-check
exactly): `u16 sig('aa 55')`, `u16 flags(0x7c)`, `u32 state(7)`, `u32 namesize` (BYTES,
UTF-16 incl NUL), `u32 datasize`, `GUID[16]`, name, data — header 32 bytes, records are
packed back to back.
```
atv/venv/bin/python atv/nvram.py list|show NAME|hexdump NAME|strings [--start 0x1a0000]
```
The store volume is `0x1a0000-0x1d0000` (192 KiB, 254 records incl. the defaults block).
Variables present: `Setup`, `MTC`, `MemoryConfig`, `MemoryTypeInformation` — repeated
history copies.  **`MemoryConfig` = 264 bytes in the EFI *global* namespace**
(`8be4df61-93ca-11d2-aa0d-00e098032b8c`) and it ships in the flash defaults
(`0x1a02fc`), i.e. the timings are recoverable from the image, not only from NVRAM.
Fields are DDR2-timing-shaped (`tRFC`-ish `0x2a`=42 at `+0x0a`, `08 0c 02 0c` at
`+0x0b`), byte-identical across all 8 copies except one byte at `+0x100`.

### Apple PEI module map (the volume at flash `0x1d0000`, 192 KiB, 19 files)
`atv/fvpe.py --fv 0x1d0000 <idx>` (the `--fv` option is new) walks it.  Link-time XIP
addresses are in the PE headers:
| idx | GUID | entry | role |
| --- | --- | --- | --- |
| 0 | `52c05b14-0b98-496c-bc3b-04b50211d680` | `0xfffd02c4` | **PeiCore** |
| 2 | `f103a5a5-9345-4c3f-b496-da14f41b6269` | `0xfffd2de4` | **MCH/DRAM init** (this is the code at `0xfffd2e2e` that pokes MCH cfg `0x52`) |
| 5 | `77c0b0cb-0406-4868-aeb5-c36b01d42ff6` | `0xfffd54c4` | **`MemoryConfig` reader** |

Decoded reader at `0xfffd6483`: `GetVariable("MemoryConfig" @0xfffd9fec, GUID
@0xfffda038, 0, &size=0x108, buf)` then `n = data[0x0b]` used as a **count of 0x18-byte
(24-byte) records** for the caller's second buffer, plus a 0x22-byte and a 0xc-byte copy.
Only this module names the variable — the DRAM-init PEIM does **not** read it by name, so
the timing path is a PPIM/service indirection (next thing to decode).

### `atv/boarddump.sh` — capture the real box's straps and SPD (reads only)
```
sh atv/boarddump.sh /tmp/atvdump     # run ON the Apple TV as root
```
Collects CPU/DMI identity, `lspci -xxx`, raw `/proc/iomem|ioports`, the **MCHBAR block**
`0xfed14000` 4 KiB (`dd if=/dev/mem`, `devmem` fallback; needs `iomem=relaxed` only if
STRICT_DEVMEM objects), the **ICH7 SMBus SPD** at 0x50-0x57 (i2c-tools, or the
`new_device`+`eeprom` sysfs route), and the ACPI tables.  `board/iomem.txt` from the old
capture has every address zeroed, so it cannot substitute.

Why it matters: `CLKCFG` (`MCHBAR+0xc00`, FSB strap) is **0 in our stub**, which makes the
Apple firmware choose DDR2-400 timings (`CLKCFG` bits 6:4 = 2 with the I945GM offset) —
the real strap and the real timing registers only exist in that block.  `board/iomem` +
`devices.txt` also show the box has a live i2c/SMBus adapter, so the SPD answer (present
or absent, since the chips are soldered) is one command away.

### Next
1. Run `boarddump.sh` on the box → seed `CLKCFG`/`CAPID`/`DEVEN`/TOLUD in the QEMU ATV
   stub from real values, then re-trace the firmware's MCHBAR writes and compare with the
   dump (validates the stub model *and* recovers the timings coreboot must hardcode).
2. Decode the `MemoryConfig` → MCHBAR mapping (24-byte records) via the PPIM indirection.
3. Then the `apple/atv1` variant: `i945_macs` + restored `model_6dx`/socket-479.

## 2026-11-19 (late) — RAM identity, real MCHBAR from the box, coreboot restore recipe

Access to the physical box: `ssh orestes@192.168.25.92` (passwordless sudo,
`/sbin/modprobe` exists — a non-login shell has no `/sbin` in PATH, so `command -v
modprobe` lies).  Dump pulled to `/tmp/opencode/bd/` (mchbar.bin, lspci.txt, iomem.txt,
ioports.txt, smbios-*, acpi-*).

### `0xf0000000` proven to be MMCONF **from the box itself**
`mmap("/dev/mem", offset=0xf0000000)` returns `86 80 a0 27 06 00 90 20 03 00 00 06` =
vendor `8086`, device `27a0`, cmd `0006`, status `2090`, rev `03` — i.e. PCI bus 0,
device 0, function 0 through the MMCONF window.  **`CONFIG_STRICT_DEVMEM=y` does not
block it**: MMCONF works with `mmap`, while `dd` on `/dev/mem` at `0xfed14000` silently
copies 0 bytes (use python `mmap`, not `dd`).

### Real MCHBAR: 195 nonzero dwords (`bd/mchbar.bin`, decoded with a symbol table from
`src/northbridge/intel/i945/i945.h`)
```
CLKCFG  0xc00 = 0x00010020   FSB strap bits 2:0 = 0 -> 400 MHz; bits 6:4 = 2 -> DDR2-400
C0DRT0  0x110 = 0x987418c8   C0DRT2 0x118 = 0x80000230   C0DRT3 0x11c = 0x008c3088
C0DRC0  0x120 = 0x40000a06   C0DRC1 0x124 = 0xb10e1900   C0DRC2 0x128 = 0x0e000000
C0DMC   0x164 = 0x01000020   C0ODT  0x168 = 0x02024911 / 0xe0018798
DCC     0x200 = 0x000f0400   CCCFT  0x208 = 0x04020108
DQSMT   0x2f4 = 0x0000200c   RCVENMT 0x2f8 = 0x00050a0f
C0DRB0..3 0x100 = 08 08 08 08 (32 MB units -> one 256 MB rank)
C0DRA0  0x108 = 0x0003   (low byte 3 -> rows/cols 0xAD|0xAE = 10 cols x 13/14 rows; high byte 0 = no DIMM1)
C1*    0x180..  all zero  -> channel B unpopulated, single channel
CxDQS  0x300..0x3b4 = 0x26262626 (identical both channels = reset defaults)
```
⇒ The ATV is **1 rank, 256 MB, DDR2-400, single channel, 4-bank x8/x16 devices**.
`CLKCFG` matching `0x20` in bits 6:4 means the timings the firmware programmed **under
QEMU are the same memory frequency** as on hardware (the stub's zeroed CLKCFG was
accidentally faithful); the real dump remains authoritative for the timing registers.

### RAM brand/model: **not determinable in software** (`!!`)
1. SMBIOS Type 17 (`dmidecode.txt`, and the raw `smbios-dmi.bin`): `Size 256 MB`,
   `DDR2`, `400 MT/s`, `DIMM0/BANK0`, but Manufacturer/Serial/Part Number are **all
   zero bytes** and widths are `Unknown` — Apple never fills them.  DIMM1 = "No Module
   Installed".
2. **No SPD EEPROM**: after `modprobe i2c-dev`, an `I2C_SMBUS` read-byte ioctl at
   `0x50..0x57` (and `0x10/0x30/0x48` as controls) returns **`ENXIO`** = no ACK, on all
   of them.  (The `eeprom` driver's sysfs route shows `ff` — that is padding of failed
   reads, not a blank EEPROM; always confirm with the raw ioctl.)
3. The flash has **no vendor strings** (Samsung/Hynix/Micron/Elpida/Nanya/ProMOS/
   Infineon/Winbond/Kingston/Qimonda/Mosel: 0 hits, ASCII and UCS-2) and no SPD blob.
⇒ Only the physical chip markings can answer it.  **Retires** the "model the ICH7 SMBus
BAR (`00:1f.3` config `0x20`) + `smbus-eeprom`" idea from the fidelity list: the real
board has `8086:27da` with `0x20 = 0xefa1` (adapter `SMBus I801 adapter at efa0`) but no
devices, so the PEI's single `0xEFA0` write+read fails on hardware too and both machines
fall back to `MemoryConfig`.  The missing SMBus BAR is **not** a QEMU fidelity gap.

### Real topology confirmed from `lspci -nnvvvxxx`
`00:01.0` = `8086:27a1` **PCIe root port** (GPU behind it at `01:00.0`), `00:1c.0/1/2` =
ICH7 PCIe ports, `00:1f.3` = `8086:27da` SMBus.  (Our `pc` machine puts PIIX3 at
`00:01.0`; that stays a known divergence.)

### coreboot: exact restore recipe (paths confirmed from the commits' own file lists)
Dothan support lived under **`socket_m`**, not a separate socket-479 directory, and the
removal is two self-contained commits — so a revert is enough:
```bash
git clone --filter=blob:none https://review.coreboot.org/coreboot.git   # full history
cd coreboot && git submodule update --init --checkout 3rdparty/blobs
git revert --no-commit 1083a4d2324c   # model_6dx/{Kconfig,Makefile.inc,model_6dx_init.c} + src/cpu/intel/Kconfig
git revert --no-commit ec6d07a3309c   # src/cpu/intel/socket_m/{Kconfig,Makefile.inc}
```
`--filter=blob:none` is deliberate: main for current `i945`/`i82801gx`, full history so
the 2018 blobs can be fetched back.

## 2026-11-19 (night) — MCHBAR fidelity diff vs real HW, and the 89V54RD2 system MCU

### New knob `ATV_MCHBAR_SEED=<file>` (`hw/i386/pc.c`)
Preloads the 4 KiB MCHBAR register file from a real-hardware dump (`/tmp/opencode/bd/mchbar.bin`)
so the firmware's read-modify-write sees genuine strap bits; everything the firmware
programs afterwards still overwrites it, which is what makes a "replay the firmware's
writes and diff vs hardware" comparison meaningful.  Tools: `/tmp/opencode/seedtest.sh`
(masked seed + boot), `/tmp/opencode/diffmch.py` (replay `ATV MCHBAR WR` from
`atv/run/qemu.log`, diff vs the real dump).  Build note: the tree was stale against
`libibverbs.so.1.18.65.0`; `meson configure -Drdma=disabled` + ninja fixed it (unrelated to ATV).

### Only TWO registers are pure read-only strap inputs (`!!`)
From the full read/write log of one boot: exactly two registers are **read and never
written** by the firmware — `CLKCFG 0xc00` (read 14×) and `DFT_STRAP1 0xe08` (read 3×).
Everything else it either writes or read-modify-writes.  Both match the real box:
`CLKCFG = 0x00010020`, `DFT_STRAP1 = 0x000202fd`.

**Negative result — straps are not the divergence.**  Seeding `0xc00,0xe08` with the real
values changes the final MCHBAR diff by **zero** dwords (93 differing dwords before and
after).  `DFT_STRAP1` bit 20 only selects graphics voltage (coreboot: set → 1.5 V/400 MHz
CRCLK); the ATV has bit 20 **clear** → 1.05 V / 250 MHz.

**Seeding everything kills the boot.**  `SSKPD 0xc1c` is a scratchpad the firmware
*writes itself* (`0x2000` config code, then `0x2005`); preloading a post-boot `0x2005`
makes it believe DRAM is already initialised — boot stops right after the `0xc1c`/`0xf14`
reads.  Only strap/input registers may be seeded.

### The firmware's DRAM init already agrees with real HW where it matters
```
PLLMON 0xc34 = 0x80800000            CPCTL 0xc16 = 0x90   (= coreboot's FSB-400 value)
DCC    0x200 = 0x400                 CCCFT 0x208 = 0x04020108   == real HW exactly
C0/C1 DCCFT 0x138/0x1b8 = 0x00100401                          == real HW exactly
C0DMC  0x164 = 0x01000020            == real HW exactly (once the strap is seeded)
C0DRC0 0x120 = 0x40000a06            == real HW exactly (read-modify-write preserved)
C0DRT3 0x11c = 0x008c3088            == real HW exactly
C0DRT0 0x110 = 0x987418c0 vs 0x987418c8   (bit 3)
```
So the timings really are derived from the flash `MemoryConfig`, not blind defaults.
Residual diff = 93 dwords, in three classes:
1. **QEMU-only, 15 dwords** — 11 of them are **channel B** (`0x180..0x1ec`, incl.
   `C1DRB0..3 = 0x08080808`).  The firmware programs channel B **unconditionally**
   (writes then reads back its own value), while the real board's `C1*` are all zero and
   `C1DRB0` is absent ⇒ on real silicon the MCH drops/masks C1 accesses when channel B is
   unpopulated (CAPID0 `0xe0` is **never read** by the firmware, so it is not a software
   decision).  **This is a fidelity gap in our stub, not in the firmware.**
2. **Real-only, 59 dwords** — never written by the firmware; post-boot/OS-written state
   (e.g. `0x4d4..0x4e8` looks like text-ish scratch, `0x280..0x2a8`, `0x340/0x358`).
3. **Both-set, 19 dwords** — training results that depend on the above (`C0DRT2 0x118`
   bit 31, `C0ODT 0x168` bit 25, `MMARB1 0x224`, `SBTEST 0x230`, `SBOCC 0x238`,
   `DRTST 0x2a8`, `MIPMC4/6 0xf08/0xf0c`).

### `MemoryConfig` (264 B, EFI global ns) — first fields decoded
```
+0x04 u32 = 1          +0x08 = 1     +0x0a = 0x2a (42)   +0x0b = 8  <- record count the
+0x0c = 0x0c  +0x0d = 2  +0x0e = 0x0c  +0x10 u16 = 0     +0x12 u16 = 0x0190 = 400 (MT/s)
+0x1c u32 = 0x15       +0x20 u32 = 0x01000100           +0x40 u32 = 0x10000000
+0x100 = 0x01af0031 (differs between copies)
```
`0x0190 = 400` matches the DDR2-400 the MCHBAR shows.  8 copies in the flash defaults,
byte-identical except one byte at `+0x100`.

### CORRECTION — "no vendor strings in the flash" needed the decompressed FVs
A raw ASCII/UCS-2 grep of `atv/atv-pristine.bin` finds **zeroed** hits for even `Apple`,
`Boot`, `HFS`, `NVIDIA` — the DXE volume is Tiano-compressed, so raw greps there are
meaningless.  Re-tested with `fvgrep.py` (unwraps COMPRESSION/GUID_DEFINED): still **0
hits** for Samsung/Hynix/Micron/Elpida/Nanya/ProMOS/Infineon/Winbond/Kingston/Qimonda/Mosel
in either encoding, in both the DXE FV and the PEI FV.  The RAM-brand conclusion survives,
now on a valid method.  (Same caveat applies to any future "is string X in the flash" test.)

### The board has an autonomous system MCU: NXP 89V54RD2 (FlashFlex51, 8051) (`!!`)
New hardware fact from the physical board.  It is an 8-bit 8051-compatible MCU with
in-system-programmable flash (ISP over UART), UART/SPI/I2C and PCA (PWM/tach), 3.3 V —
**no USB device controller, no LPC/EC block**.

Measured: it has **no host-visible interface at all** —
| probe | result |
| --- | --- |
| ACPI embedded controller (`0x62/0x66`) | DSDT has no `EC0`/`H_EC`; `/proc/ioports` shows only `keyboard` at 0x60/0x64 |
| SMBus slave | full quick-write scan **0x03–0x77: 0 responders** on all four adapters (i2c-0..3) |
| ACPI device | 42 DSDT devices — `PWRB PRGB HUB1 PRT1 PRT2 HUB7 LAN0 HDEF PXS1 DMAC FWHD MATH RTC_ TIMR SBUS`; no `SMC`, no `_TZ_`, no LED device |
| kernel driver | dmesg has no applesmc/smsc/fan/pwm; the only hwmon is `nouveau` (GPU die temp); no thermal zone |
| USB IR receiver | **separate chip**: `05ac:8241 Apple, Inc. Built-in IR Receiver` on UHCI bus 2 |
⇒ It runs **autonomously**, and must be the board's power/thermal/LED supervisor (Apple's
"SMC" role): standby rail + rail sequencing + reset for the internal mains PSU, front
white LED (nothing in ACPI/OS drives it), and **fan PWM + thermistor** — Linux on the real
box has *no* fan or board-temperature driver at all, yet the unit runs quiet and cool,
which is only possible if something autonomous closes that loop.

Supporting: the Apple firmware configures the ICH7 GPIO block and Linux never touches it
(`gpio_ich` bails on a resource conflict at 0x500-0x53F).  Live values from the real box,
with the register offsets coreboot uses (`sb/intel/common/gpio.c`: **`GP_LVL` is 0x0c and
`GPI_INV` is 0x2c** — an earlier version of this note had those two swapped):
```
GPIO_USE_SEL 0x500 = 0x1f40f7c0   GP_IO_SEL  0x504 = 0xe0a8af83
GP_LVL       0x50c = 0xe0806ec3   GPI_INV    0x52c = 0x00000482
GPIO_USE_SEL2 0x530 = 0x000100c0  GP_IO_SEL2 0x534 = 0x00000030   GP_LVL2 0x538 = 0x00010000
GP_RST_SEL1/2/3 0x560/0x564/0x568 = 0xffffffff (unimplemented)
```
Decoded: 15 pins in use in bank 1 + 3 in bank 2 = 18; **9+3 = 12 outputs**
`{6,12,14,22,24,25,26,27,28} + {38,39,48}` (of which `GPIO6`, `GPIO14` and `GPIO48` are
driven high), 6 inputs `{7,8,9,10,13,15}`, invert bits `{1,7,10}`.
`setup_pch_gpios()` writes whole 32-bit registers, so any table supplied overwrites
unlisted pins too — a `NULL` entry is the only way to leave a register alone.

**Also found (FV idx 33 `ef0c99b6` = the SMBIOS generator, not a fan driver as the
"Fan/LED" keyword hit suggested) — a physical recovery jumper string:**
`J1D4:1-2,5-6,9-10Default;2-3CMOS clr,6-7Pswd clr,10-11Recovery`
⇒ the board has a jumper block with **CMOS clear / password clear / Recovery** positions.

### Consequences for the coreboot port
1. **Do not reprogram the ICH7 GPIO block** (I/O 0x500-0x53F and RCBA+0x3400).  The MCU
   handshake lives there; inherit Apple's configuration rather than zeroing it.
2. Keep MCU interaction **read-only / none**.  No driver is needed — the shipping OS works
   without one.
3. The MCU plausibly owns power sequencing/reset, which together with the `J1D4` Recovery
   jumper is the *hardware* recovery path for a bad flash.  The part is ISP-reprogrammable
   over UART but we have **no dump of its firmware and no way to read it from the host** —
   never send it anything.
