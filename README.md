# aminesiac

**Volatile Copy-on-Write snapshots for AmigaOS on PiStorm / Emu68**

*Make changes. Reboot. Keep working. Power off. Start again from the original disk.*

`aminesiac` is a native AmigaOS 68k utility that places a RAM-based, sector-level Copy-on-Write (COW) overlay in front of the Raspberry Pi microSD storage exposed by Emu68's `brcm-sdhc.device`. Writes intercepted by the overlay are retained in RAM; reads see those changes as if they had been written to the disk. A normal Ctrl-Amiga-Amiga warm reset retains the snapshot, while a **complete power-off of both the Amiga and Raspberry Pi** discards it.

This is not a file copier, a filesystem driver, an HDF emulator, or a modified Emu68 build. The existing AmigaOS filesystem continues to operate on what appears to be an ordinary block device.

> [!WARNING]
> **Experimental storage interception — use a disposable, fully backed-up clone of your microSD.** The observed rollback is a functional property of the tested configuration, **not a guarantee that the physical card is never written**. See [Safety and limitations](#safety-and-limitations) before enabling it.

## Why this matters

A modern storage-snapshot technique can run **inside an authentic 1990s operating system**, without replacing AmigaDOS, replacing the filesystem, running Linux underneath it, or modifying the SD driver. The project combines three mechanisms that are particularly well suited to PiStorm/Emu68:

1. **Native Exec device interception.** Standard AmigaOS `SetFunction()` hooks intercept the device I/O path below the filesystem. The same overlay can therefore cover ordinary file writes *and* filesystem metadata updates, without needing to understand PFS3, FFS, or individual AmigaDOS operations.
2. **Warm-reset persistence.** A COLDSTART resident and carefully registered `KickMem` preserve the COW mapping and its FAST RAM allocation across supported Ctrl-Amiga-Amiga resets, then re-establish the necessary hooks as AmigaOS starts again.
3. **Two different lifetimes.** AmigaOS can restart while the PiStorm/Emu68 memory holding the snapshot remains available. A full hardware power-off ends that memory lifetime and restores the underlying disk's observable state, apart from any actual physical writes made outside the protection window.

The result is a practical demonstration of how the original AmigaOS/Exec architecture can support a surprisingly sophisticated, modern-style storage service using native interfaces alone. PiStorm supplies the abundant RAM and the favorable reset behavior; AmigaOS supplies the resident and device abstractions.

### I/O model

```text
Amiga applications / AmigaDOS
               |
     PFS3 / FFS / other filesystem
               |
       Exec device I/O
               |
   aminesiac BeginIO/AbortIO hooks
          /              \
  COW sector present?     Original brcm-sdhc.device
          |                       |
  Read from RAM            Read missing sectors
  Write into RAM           from physical microSD
          \_______________________/
               |
       Coherent logical disk
```

The COW is **sector-based (512 bytes)**. For writes, the resident records modified logical sectors in a preallocated FAST RAM arena; for reads, it combines original sectors with the modified sectors from RAM. Both device unit 0 (whole-card offsets) and unit 1 (offsets relative to the single recognized Amiga partition) are translated into one physical-sector address space so aliases remain coherent.

The resident uses a serialized I/O worker and avoids allocating storage on the write path. Unknown or potentially write-capable device commands are rejected instead of silently passing through to the physical device. These are important safeguards, but they do not turn the project into a hardware write blocker.

## Requirements and compatibility

| Component | Requirement |
| --- | --- |
| Computer | Amiga with PiStorm and Emu68; hardware-tested on an Amiga 600 / Raspberry Pi 3A+ configuration |
| Operating system | AmigaOS with Exec version 39 or newer; other ROM versions have not all been individually validated |
| Storage driver | `brcm-sdhc.device` exposing compatible standard Exec `BeginIO`/`AbortIO` vectors |
| Media geometry | 512-byte sectors; MBR partition table; **exactly one primary type `0x76` partition** |
| Device units | Unit 0 exposes the whole card; unit 1 exposes that `0x76` partition; their sampled contents and reported geometry must agree |
| Memory | Sufficient **contiguous** FAST+KICK RAM plus a small CHIP RAM allocation for resident state/code |
| Filesystems | Transparent at the supported block-device interface; PFS3 has been exercised on real hardware |

The implementation does **not** bind itself to the driver's version number, Git revision, absolute function addresses, or private structure offsets. Instead, it checks the expected Exec vector format, device geometry, unit mapping, and sampled media contents. That makes it *build-independent by design*, **not automatically compatible with every old or future `brcm-sdhc.device` release**.

It does not currently target `brcm-emmc.device` (used by some Pi 4/CM4 configurations), other storage drivers, GPT layouts, multiple `0x76` partitions, or arbitrary SCSI-direct commands. A successful `PROBE` is necessary, but not sufficient, evidence of compatibility.

## Quick start

**First make a complete, independently verified backup, and use a spare clone.** Completely power off both the Amiga and Raspberry Pi before inserting it or switching between installations.

Place **both** files in the same Amiga directory and execute the commands *from that directory*:

```text
aminesiac
aminesiac.R3Module
```

Set a sufficiently large CLI stack, then run the non-installing compatibility check:

```text
Stack 65536
aminesiac PROBE
```

A successful probe ends with:

```text
[PROBE] PASS: read-only compatibility checks. No hooks installed.
```

If, and only if, the probe passes and you accept the risks, activate a 32 MiB snapshot:

```text
aminesiac ARM MEM=32 I_ACCEPT_RISK
aminesiac STATUS
```

Look for a successful COLDSTART initialization, worker marker `52445932` (`RDY2`), `faults=0`, and all four gates reporting `PASS` immediately after activation. The overall state should be:

```text
[STATUS] ACTIVE-GATED: vectors match; physical isolation NOT independently proven
```

**Do not run `ARM` again after a warm reset.** Recovery is handled by the resident. There is no `DISARM` command; completely remove power from *both* machines to discard the volatile snapshot.

## Command reference

The CLI accepts exactly these forms (commands and tokens shown as used by the executable):

```text
aminesiac PROBE
aminesiac ARM MEM=N I_ACCEPT_RISK
aminesiac STATUS
```

| Command | What it does |
| --- | --- |
| `PROBE` | Performs non-installing driver-vector, geometry, partition and sampled-read checks. Requires no pre-existing active resident. |
| `ARM MEM=N I_ACCEPT_RISK` | Allocates the RAM arena, loads `aminesiac.R3Module`, registers the persistent resident, and installs the I/O interception gates. **Changes system state.** |
| `STATUS` | Inspects the resident, COW counters, sampled media fingerprint and current interception vectors. Does not arm or disarm the overlay. |

### `MEM=N` — snapshot capacity

`N` is any **whole number from 4 through 128**, expressed in **MiB**, inclusive; it is **not** restricted to powers of two.

Examples:

```text
aminesiac ARM MEM=16 I_ACCEPT_RISK
aminesiac ARM MEM=32 I_ACCEPT_RISK
aminesiac ARM MEM=100 I_ACCEPT_RISK
```

These are alternative one-time invocations on **separate, freshly powered-on sessions**, not commands to run one after another. `MEM=100` provides exactly **100 MiB of modified-sector capacity** (`204800` sectors), rather than silently rounding the COW payload to 128 MiB.

The hash table is sized separately to a power of two. For `MEM=100`, the data/entry area plus hash table reserve approximately **103.6 MiB** of FAST RAM, in addition to resident overhead. Memory is allocated up front; the allocation must succeed with appropriate FAST+KICK attributes. Choose a capacity that leaves sufficient RAM for AmigaOS and applications.

### Reading `STATUS`

A representative live status looks like this (addresses and counters vary):

```text
[RESIDENT] boots=1 arms=1 worker=52445932 faults=0
[COW] used=115/65536 writes=10 reads=21 changes=10 busy=00000000
[GATE] Exec AddDevice=... expected=... PASS
[GATE] Exec OpenDevice=... expected=... PASS
[GATE] brcm device=... BeginIO=... expected=... PASS
[GATE] brcm AbortIO=... expected=... PASS
[STATUS] ACTIVE-GATED: vectors match; physical isolation NOT independently proven
```

- `boots`: resident initialization count, including warm-restart activations.
- `arms`: installation/arming count.
- `worker=52445932`: ready marker; `faults=0` is the expected healthy condition.
- `used/capacity`: **unique sectors currently held in the COW**, not bytes and not the number of write requests. Capacity is `N × 2048` sectors.
- `writes`, `reads`, `changes`: activity counters; not proof that physical media is unchanged.
- `busy=0`: no current outstanding persistent-state busy marker at the instant sampled.
- `GATE ... PASS`: the installed vector still equals the resident's expected hook address. Any `FAIL` means that interception status is **not verified**; it must not be disregarded.
- `fingerprint`: a small sampled-media consistency value, **not** a whole-card SHA-256 or a cryptographic proof of immutability.

The current runtime checks explicitly recognize a particular 68k Exec jump-vector encoding; other implementations may be rejected even if they are functionally similar.

## Recommended operational procedure

The following procedure is deliberately conservative. Use **only disposable data on a cloned card**.

1. **Cold start:** power off the Amiga and Pi completely, then boot from the clone. Remember that any filesystem activity *before* `ARM` is outside the protected session.
2. **Preflight:** open a CLI, `Stack 65536`, and run `aminesiac PROBE`. Stop if anything fails.
3. **Arm once:** run `aminesiac ARM MEM=32 I_ACCEPT_RISK` (or another chosen capacity) and immediately inspect `aminesiac STATUS`.
4. **Exercise the overlay:** write and read a disposable test file on a known volume backed by the `0x76` partition; for example, if your mounted volume is actually named `SDH1:`:

   ```text
   Echo "AMINESIAC_TEST" >SDH1:AMINESIAC.TEST
   Type SDH1:AMINESIAC.TEST
   aminesiac STATUS
   ```

5. **Warm reset:** perform Ctrl-Amiga-Amiga without removing power from PiStorm. **Do not run `ARM` again.** Inspect `STATUS` and read the file again. The COW content should survive the reset in the supported configuration.
6. **End the session:** switch off both Amiga and Raspberry Pi completely. On the next cold boot, verify that the test file has disappeared; only physical changes made outside/bypassing the overlay can remain.
7. **Check filesystem health:** after returning to the unarmed, original view, use a non-destructive filesystem check (for PFS3, a PFSDoctor inspection is useful). Keep backups and compare against a known baseline if physical integrity matters.

Do not treat an unexpected failure, a missing file, or a gate mismatch as a reason to keep writing. Stop I/O and perform a complete power-off of the clone instead.

## Safety and limitations

### Not a physical write-protect mechanism

`aminesiac` hooks the AmigaOS device path; it does **not** modify the Raspberry Pi SD controller or establish hardware-level write protection. In particular:

- Writes made **before** activation may already have reached the physical card. Filesystems such as PFS3 can update metadata during mount, even without an explicit user file operation.
- Driver-private I/O, unforeseen commands, code paths bypassing the hooks, or unexpected boot ordering may still reach the underlying storage.
- A successful `PROBE`, `ACTIVE-GATED` status, clean PFSDoctor result or functional rollback test cannot by itself prove sector-for-sector physical immutability.
- The unit/partition audit verifies only the layout and sample sectors it examines; it does not verify the entire microSD.

A whole-device SHA-256 taken before and after a boot can differ because of legitimate **pre-ARM** changes. That difference is worth investigating at the sector level; it is neither automatic proof of a COW leak nor proof that the COW prevented all physical writes.

### No live deactivation, commit or automatic recovery

There is **no `DISARM`, `COMMIT` or save-snapshot operation**. The intended behavior is to discard the overlay at full power-off. A full power-off is also the safe reset boundary before changing binaries or trying another capacity. A warm reset retains the overlay and may retain any resulting filesystem problems within the current RAM session.

Do **not** run disk repair, partitioning, formatting, firmware updates or other high-risk storage operations while armed. If a gate fails, `faults` becomes nonzero, or the COW fills up, stop further I/O; the filesystem may have received errors, and subsequent operations must not be trusted. The COW is finite and is **not** permitted to fall back to writing the original disk when full.

### Supported I/O surface is deliberately narrow

The current worker handles the ordinary supported read/write commands plus selected flush/update operations and passes only known safe control commands. Unknown write-capable commands, unsupported SCSI-direct traffic, unusual unit mappings and unexpected media changes are not treated as safe. Some utilities may therefore receive I/O errors instead of working transparently.

The warm-reset reinstallation also depends on real boot ordering and Exec behavior. In particular, other system components can replace an Exec vector during startup. Recheck `STATUS` after a reset: **a `FAIL` is a warning, not an endorsement of continued use**, even if the filesystem appears functional.

### Power and data loss

Full power removal discards **all** changes in the RAM snapshot, including anything created, installed or configured while it was active. An abrupt power loss or crash has the same risk of losing the snapshot and may interrupt an operation in progress. Never treat the overlay as durable storage, and never test on irreplaceable media.

## Build from source

On a Linux cross-build host, install `m68k-amigaos-gcc`, Amiga NDK headers, `make`, Python 3 and a host C compiler. From the source directory:

```sh
make clean
make test-host
make CC=m68k-amigaos-gcc \
     NDK_INCLUDE=/path/to/ndk-include \
     AMIGA_CRT=-mcrt=nix20 \
     all

ls -lh aminesiac aminesiac.R3Module
```

The two resulting files must be deployed **together**. `aminesiac` loads `aminesiac.R3Module` from the current working directory when arming; renaming or mixing different protocol versions can make the installation fail. The build includes a resident ABI check to reject an incompatible module.

No Emu68 patch, Linux service, replacement filesystem, or AmigaOS boot script is needed for normal installation. The source tree provides host-side tests, but **successful compilation and host tests are not a substitute for hardware validation**.

## Validation scope

The COW behavior has been tested on real Amiga/PiStorm/Emu68 hardware using `brcm-sdhc.device` 1.9, including installation, active read/write interception, preservation across warm reset, and disappearance of virtual changes after a complete power-off. PFS3 integrity checks with PFSDoctor have also passed on the tested volumes. The configurable-memory implementation has been hardware-tested, including the non-power-of-two setting `MEM=100`.

These observations establish useful **functional validation on that configuration**. They do not certify every Kickstart, all Emu68 drivers/builds, all filesystem layouts, or absolute physical write isolation.

## Credits and acknowledgments

**Michal Schulz** ([`michalsc`](https://github.com/michalsc)) is the author of **Emu68 and `brcm-sdhc.device`**, whose native AmigaOS storage interface makes this project possible. His design and work on the Raspberry Pi SD host driver and Emu68's integration with AmigaOS are fundamental to `aminesiac`.

- [Emu68 project](https://github.com/michalsc/Emu68)
- [Emu68 SD card preparation and device-unit documentation](https://github.com/michalsc/Emu68/blob/master/docs/tutorials/SD_Preparation.md)
- [Emu68 configuration options](https://michalsc.github.io/Emu68/Options.html)

`aminesiac` is an independent AmigaOS-side utility, not an official Emu68 component. No endorsement by the upstream author is implied.

---

**The essential idea:** AmigaOS still sees an ordinary disk; the snapshot lives entirely in RAM, survives the Amiga's warm reboot, and disappears when the underlying PiStorm memory loses power. That is both a useful experimental tool and a demonstration of the continuing technical relevance of AmigaOS's original resident, memory and device interfaces.
