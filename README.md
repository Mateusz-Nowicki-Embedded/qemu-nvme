# qemu-nvme

Personal fork of QEMU used as a testbed for NVMe driver / controller
debugging extensions.

All NVMe-specific work lives on the [`nvme`](../../tree/nvme) branch.

## `info nvme` / `info nvme-queues` HMP commands

Two new monitor commands for inspecting an emulated NVMe controller
without attaching gdb to QEMU.

* `info nvme` — per-controller summary: PCI BDF/BAR0, identify fields
  (SN/MN/FR/CNTLID), `CC`/`CSTS`/`AQA` registers, admin + I/O queue counts.
* `info nvme-queues` — every active SQ/CQ with size, head, tail, the
  ring's PRP1, the BAR0-relative doorbell offset (`SQyTDBL` / `CQyHDBL`)
  and the CQ phase tag.

Both commands accept an optional controller name (e.g. `info nvme nvme0`).
The name maps to `/machine/peripheral/<name>`; a value starting with `/`
is taken verbatim as a canonical QOM path. When omitted, all NVMe
controllers in the machine are reported.

```
(qemu) info nvme
/machine/peripheral-anon/device[0]
  PCI:    BDF 00:04.0  VID=8086 DID=5845  BAR0=0x00000000feb50000
  ID:     SN=NVME0001  MN=QEMU NVMe Ctrl  FR=...  CNTLID=0x0000
  CC:     0x00460001
  CSTS:   0x00000001
  AQA:    0x001f001f
  Queues: 1 admin + 4 IO SQ / 4 IO CQ

(qemu) info nvme-queues
/machine/peripheral-anon/device[0]
  SQ 0  size=32    head=9     tail=9     cqid=0    prp1=0x000000007f7f0000  SQTDBL=BAR0+0x1000
  CQ 0  size=32    head=8     tail=8     iv=0      prp1=0x000000007f7f1000  CQHDBL=BAR0+0x1004  phaseTag=1
  SQ 1  size=1024  head=128   tail=128   cqid=1    prp1=0x000000007e000000  SQTDBL=BAR0+0x1008
  CQ 1  size=1024  head=128   tail=128   iv=1      prp1=0x000000007e010000  CQHDBL=BAR0+0x100c  phaseTag=0
  SQ 2  size=1024  head=64    tail=64    cqid=2    prp1=0x000000007e020000  SQTDBL=BAR0+0x1010
  CQ 2  size=1024  head=64    tail=64    iv=2      prp1=0x000000007e030000  CQHDBL=BAR0+0x1014  phaseTag=0
  SQ 3  size=1024  head=32    tail=32    cqid=3    prp1=0x000000007e040000  SQTDBL=BAR0+0x1018
  CQ 3  size=1024  head=32    tail=32    iv=3      prp1=0x000000007e050000  CQHDBL=BAR0+0x101c  phaseTag=0
  SQ 4  size=1024  head=16    tail=16    cqid=4    prp1=0x000000007e060000  SQTDBL=BAR0+0x1020
  CQ 4  size=1024  head=16    tail=16    iv=4      prp1=0x000000007e070000  CQHDBL=BAR0+0x1024  phaseTag=0
```

## `nvme_completion_delay` HMP command

Inject an artificial delay before posting completions on a given NVMe
Submission Queue. Each command submitted on that SQ has its completion
held back by *delay_ms* milliseconds, counted from the moment the
controller finished executing it.

```
(qemu) nvme_completion_delay <sqid> <delay_ms> [<name>]
```

* *sqid* — Submission Queue ID. `0` is the admin queue. I/O SQ IDs
  start at `1`; use `info nvme-queues` to see what is currently
  allocated.
* *delay_ms* — delay in milliseconds. `0` disables the delay on that
  SQ.
* *name* — optional controller name (e.g. `nvme0`). When omitted, the
  setting is applied to **every** NVMe controller in the machine.
  Same name mapping as `info nvme [name]`.

The delay only affects completion posting; the command itself is
fetched and executed normally. Its real service time is preserved, so
the total host-visible latency is roughly `service_time + delay_ms`.

Typical use: drive the Linux NVMe driver into its timeout/abort/reset
paths without patching the controller code. With the default
`io_timeout=30s`, setting a delay above 30 seconds on an I/O SQ that
the driver will use makes the next command timeout, abort, and
trigger a controller reset:

```
(qemu) info nvme-queues                       # find a populated I/O SQ
(qemu) nvme_completion_delay 1 35000          # 35s on SQ 1
```

then from the guest:

```
~ # taskset -c 0 dd if=/dev/nvme0n1 of=/dev/null bs=4k count=1 iflag=direct
# hangs ~30s, dmesg shows timeout + abort + controller reset
```

To clear:

```
(qemu) nvme_completion_delay 1 0
```

**Caveat:** the delay does not survive a controller reset. After CC.EN
1→0 (or PCI FLR / NSSR), the per-SQ `delay_ns` is reset to 0 and the
backing `QEMUTimer` is freed; re-apply with `nvme_completion_delay`
once the controller is re-enabled and the I/O queues are re-created.

## TODO

Ideas to make controller reset paths properly testable from a driver
perspective.

* **NSSR (NVM Subsystem Reset) support.** Today QEMU only logs a
  `LOG_GUEST_ERROR` when the host writes the NSSR magic value
  (`0x4e564d65`) to the NSSR register (`hw/nvme/ctrl.c`, NVME_REG_NSSR
  case in `nvme_write_bar`). A real subsystem reset should tear down all
  controllers in the subsystem, not just the one being written to.
  Useful for exercising driver-side subsystem-reset recovery code.

* **Posting completions during controller reset.** On CC.EN 1→0 the
  spec allows the controller to post outstanding CQEs before clearing
  CSTS.RDY — including with status `Successful Completion` for commands
  that actually finished (e.g. AIO callbacks fired during the implicit
  drain). QEMU currently drops `cq->req_list` silently when freeing the
  CQ. Adding an opt-in `reset_flush_completions=on` device parameter
  that flushes pending CQEs (with their real status, not synthesised
  abort) between `nvme_ns_drain` and `nvme_free_cq` would let drivers
  exercise the "late completion during reset window" path that real
  hardware sometimes takes.

