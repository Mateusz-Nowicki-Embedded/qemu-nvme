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

