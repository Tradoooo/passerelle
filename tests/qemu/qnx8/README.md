<!-- SPDX-License-Identifier: Apache-2.0 -->
# QNX 8.0 guest — booting and enumerating the virtio-9p device

This directory lays the ground for running the Passerelle drivers (`fs9p`,
`fsfuse`) inside a real QNX 8.0 guest under QEMU. What is proven here, on this
PC (QEMU 8.2.2, the SDP 8.0 at `~/qnx800`, `/dev/kvm` usable):

1. a **QNX 8.0 guest that boots** and **enumerates the virtio-9p-pci device**
   (`vid/did 1af4/1009`);
2. the **exact QEMU command line** that maps the host share onto that device;
3. the **interrupt line** QEMU really wires to the device, which matches
   `image/pci_hw.cfg` already.

`boot.log` is the serial console of that boot, captured and sanitized (control
characters stripped). It shows the kernel banner, the PCI server starting, and
`pci-tool -v` listing the device.

## The QEMU command line

```sh
qemu-system-x86_64 \
  -machine q35 -cpu host -smp 1 -m 512 -enable-kvm \
  -kernel <ifs_image> \
  -fsdev "local,id=host,path=<host_share_dir>,security_model=mapped-xattr" \
  -device "virtio-9p-pci,fsdev=host,mount_tag=passerelle,addr=0x05" \
  -display none -serial file:<console.log> -monitor none -no-reboot
```

* `-machine q35` (ICH9, PCI Express) and **not** `pc` (i440fx): see "Why q35"
  below. No `/dev/kvm` → replace `-enable-kvm -cpu host` with
  `-accel tcg,thread=multi -cpu max`.
* `addr=0x05` pins the device to PCI slot 5, so its bus/device/function and its
  interrupt line are stable (below). Without it, the slot and IRQ shift with the
  other `-device` lines.
* `security_model=mapped-xattr` needs no root (SEC-03, `host/qshare/options.py`).
* `mount_tag` ≤ 31 bytes for 9P (QEMU-enforced).

`run-guest.sh` wraps this line and captures the console to a file; `console.py`
drives it back through a `-serial unix:` socket when interaction is needed.

## The virtio-9p device as QNX sees it

`pci-tool -v` (in `boot.log` in this directory) reports:

```
B000:D05:F00 @ idx 6
	vid/did: 1af4/1009
	class/subclass/reg: 00/02/00
		<Classcode is undefined>
```

`B000:D05:F00` is the device QEMU places on `addr=0x05`; `1af4/1009` is the
virtio PCI vendor/device id, the same one `image/pci_hw.cfg` names.

## The interrupt line: measured 10, not a guess

`info pci` in the QEMU monitor (machine running, SeaBIOS IRQ routing applied)
reports for the device:

```
Bus  0, device   5, function 0:
    Class 0002: PCI device 1af4:1009
    IRQ 10, pin A
```

So QEMU wires **IRQ 10, pin A** to `B0:D5:F0`, on `q35` as well as on `pc`
(measured on both). That is exactly the entry the project already carries:

```
# image/pci_hw.cfg
[interrupts]
B0:D5:F0    A    10
```

**No correction was needed** — the existing `image/pci_hw.cfg` is right once
the device is pinned with `addr=0x05`. The same entry was added to the
`pci_hw.cfg` block that `image/passerelle.build` embeds.

## Building the guest image

`image/passerelle.build` is a fragment. `mkifs` (the tool `mkqnximage` itself
calls to build the IFS) builds a complete image from the fragment plus a search
path that resolves the SDP 8.0 layout (`$QNX_TARGET/x86_64/...`):

```sh
./build-image.sh          # -> build/qnx8/passerelle.ifs
```

`build-image.sh` sources `~/qnx800/qnxsdp-env.sh` and runs:

```sh
mkifs -v -v tests/qemu/qnx8/passerelle.build build/qnx8/passerelle.ifs
```

`tests/qemu/qnx8/passerelle.build` sets the search path and includes the
fragment (`[+include] image/passerelle.build`). The fragment's driver lines are
`[+optional]`, so the image builds **before** `fs9p`/`fsfuse` exist and still
carries the kernel, the C library, the PCI server and the tools.

## Why q35: the i440fx machine cannot run a libpci client

Measured here, on the SDP 8.0, QEMU 8.2.2, KVM:

| machine | firmware tables | `pci_device_read_vid(B0:D5:F0)` |
|---|---|---|
| `pc` (i440fx) | no MCFG | **SIGSEGV** on a randomised address, from the first call |
| `q35` (ICH9, PCIe) | MCFG → ECAM at `0xb0000000` | `0x1af4` |

On `pc` the start-up logs `ACPI table not found (MCFG)`; the PCI server then
announces `ECAM base could not be found, OK, I/O accesses only`, and the
hardware module that libpci loads **inside the driver process** dereferences a
wild pointer as soon as the driver reads configuration space. `pci_device_attach`
succeeds first, so the failure looks like a device problem when it is a
platform one. `pci-tool` still works there, because it reads the server's shared
database rather than the bus.

With `q35` the server finds the ECAM, every libpci call answers, and the same
driver code runs unchanged. The device keeps `B0:D5:F0` and IRQ 10 pin A.

## What libpci will and will not give a driver (SDP 8.0)

Four rules, each one paid for here:

1. **`pci_device_cfg_rd*()` reaches only offsets `0x40..0xFF`** (the
   device-dependent area). The common header has one typed accessor per
   register (`pci_device_read_vid/did/cmd/status`). Reading `0x00` with
   `cfg_rd32()` does not return an error on this stack: on `pc` it faults.
2. **The capability chain comes from the capability modules**, not from the
   `next` bytes: `pci_device_read_capid()` + `pci_device_read_cap()` +
   `cap_vend_get_offset()` (the capability start is that offset minus 3, where
   `cfg_type` sits) + `cap_vend_get_num_bytes()` (capability length minus 3)
   + `cap_vend_read_bytes()`. The modules are looked up in
   `PCI_CAP_MODULE_DIR`, which is why the image installs
   `pci_cap-0x09-ffffffff.so` under the name QNX asks for
   (`pci_cap-0x09-1af4ffff.so`).
3. **A device-specific region is usually shorter than its capability.** QEMU
   advertises a `0x1000`-byte DEVICE region for virtio-9p but only holds
   `virtio_9p_config` = 2 + tag length bytes (11 for `hostshare`), and answers
   `0xff` to any read that crosses the real end. A reader that fetches 32 bits
   at a time therefore sees `hostsh` followed by three `0xff` and refuses the
   device. The driver now reads that region one byte at a time
   (`pgw_pci_bar_io.r8`), which is also what the virtio specification allows.
   The same trap hides in the 1-byte ISR region, where a 32-bit read reads as
   "an interrupt is always pending".
4. **BARs must be mapped by type.** The transitional virtio-9p-pci exposes BAR0
   as an I/O BAR (the legacy register file); mapping it with
   `mmap_device_memory()` maps an unrelated page. Only memory BARs are mapped.

## Two things the guest showed, and one it still needs

**The unmount used to deadlock.** `resmgr_detach()` blocks until the mount
point's attribute is free, and the attribute never was: the driver now lets the
kernel remove the namespace entry when the process exits, and a supervisor
remounts it. `slay fs9p` returns, the console keeps answering, and the mount is
back.

**Executing a program of the share does not work yet — and it is not the exec
reply.** Measured on this guest on 14 November 2026, with the tools in this
directory:

* the bytes are right. `pread` at offsets 0, 64, 512, 1024, 2048 and 4096
  returns the host's own bytes, and a copy of the binary made *through the
  driver* executes from `/dev/shmem` (`mapprobe`, `fs6_ab.py`);
* the mount does not forbid it. `statvfs` reports `RDONLY|NOSUID` and no
  `NOEXEC`, and `access(X_OK)` returns 0 (`mapprobe`);
* the mapping is not refused. `mmap` with `PROT_READ|PROT_EXEC` on an `O_RDONLY`
  descriptor is answered, and the ELF magic is read through it — and the same
  request on an `O_EXEC` descriptor is refused **by the control too**, so that
  refusal is QNX and not this driver (`mmap_matrix.py`);
* the loader never asks for a mapping. The kernel trace of an exec attempt
  (`exec_two.py`, then `trace_report.py`) carries opens, lookups, stats and 41
  reads, and **no `_IO_MMAP` event at all**. 41 reads on a 9 KiB binary is the
  shape of `/bin/sh` swallowing it as a script, which is the "no closing quote"
  the console shows.

What the exec open actually is, read off the message the process manager sends:
a `_IO_CONNECT_COMBINE_CLOSE` with `_IO_FLAG_EX|O_PERMCHECK|O_REALIDS` — a
permission check, not an open — then a `_IO_CONNECT_COMBINE` whose 8-byte second
message is an `_IO_STAT` (`access()`). `_IO_FLAG_EX` is `0x04` on the wire and
`000003` in `fcntl.h`; conflating the two is what an earlier note in this file
did, and the decoder (`decode_exec_open.py`) now reads them apart.

So the missing piece is **not** a link reply with an invented exec-info format.
It is upstream of that: the process manager declines the file before it maps
anything, and every candidate this directory can measure has been eliminated.
The `_IO_MMAP` callout is in place (`iofunc_mmap_default()`, PR #79) and traced,
so the next run can see a mapping request if one ever arrives. `fs6-demo.c` is
the program; `mapprobe.c`, `fs6_ab.py`, `mmap_matrix.py`, `exec_two.py` and
`decode_exec_open.py` are the measurements, kept here because each one closed a
hypothesis that reading the code could not close.

**`clockprobe.c`** answers, at boot, the two calls the driver's deadlines are
built on (`clock_gettime(CLOCK_MONOTONIC)` and `nanosleep()`): both work in the
guest, so a wait that does not expire is a defect of the driver, not of the
platform.

## Capturing the kernel trace (QNX-09)

The kernel trace of our own request events is now a repeatable measurement, not
a manual exercise. Three pieces:

* `image/qtrace.sh` (in the guest): starts `tracelogger` on the kernel ring,
  runs the benchmark at two priorities, stops the recorder and prints the user
  events `traceprinter` decoded;
* `tests/qemu/qnx8/capture_trace.py` (on the host): boots the guest, waits for
  the share to answer a read, runs the script and records the console. It ends
  on the script's own last line, not on a gap in the console, because a
  measurement script is silent for seconds while its benchmark runs;
* `tests/qemu/qnx8/trace_report.py` (on the host): pairs each submission with
  its reply, decodes the operation and the priority out of the payload, and
  refuses a trace whose two decodings disagree.

```
python3 tests/qemu/qnx8/capture_trace.py --keep --workdir /tmp/pgw-trace
python3 tests/qemu/qnx8/trace_report.py /tmp/pgw-trace/transcript.log
```

Two traps measured on this guest, both now avoided by the scripts:

* **the guest's shell cannot create a pipe** once the driver is serving (`sh:
  can't create pipe - try again`). It is not caused by the driver: a guest
  booted without a matching mount tag fails the same way. So `qtrace.sh` uses no
  command substitution and no pipe at all - only redirections, exit statuses and
  shell variables;
* **`tracelogger` needs an instrumented kernel**, which this image has
  (`procnto-smp-instr`), and it must run as root in the guest.

The measured result of 14 November is in
[`docs/measurements/qnx8-guest.md`](../../../docs/measurements/qnx8-guest.md):
3,207 user events, priorities 10 and 63 read out of the payload, and every
submission paired with its reply.

## Where this stands

Proven, and captured in this directory:

* a **QNX 8.0 guest that boots**, enumerates the virtio-9p-pci device
  (`vid/did 1af4/1009`) and reports the interrupt QEMU really wires to it
  (`boot.log`);
* the exact QEMU command line, on the machine that can run a libpci client;
* `fs9p` opening that device from inside the guest: capability walk, BAR
  mapping, queue setup. `run-guest.sh` boots it and `qshare-mount.sh` mounts
  the PC's folder at `/host`.

Next, and only measurable now that the driver runs: the priority, latency,
throughput and mount-time figures (`docs/measurements.md`), and the kernel
trace.
