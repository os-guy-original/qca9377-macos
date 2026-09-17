# Firmware provenance and fetch

The files in `QCA9377-hw1.0/` are the Qualcomm Atheros firmware images for
QCA9377 (hw1.0), as shipped by the `linux-firmware` project and loaded by
Linux `ath10k` into the card's RAM on every boot:

| File | Role |
|---|---|
| `firmware-6.bin` | the firmware ath10k actually loads on api-6 (what modern kernels use) |
| `firmware-5.bin` | older api-5 image, kept for study/fallback |
| `board-2.bin` | board data (per-OEM calibration container), api-2 format |
| `board.bin` | legacy single-board file |
| `notice_ath10k_firmware-*.txt` | the license terms that govern redistribution of these binaries |

The firmware runs **on the card**, uploaded from the host each boot — it is
not flashed, and using it does not modify any hardware.

## Verify against upstream

Firmware release source: <https://github.com/kvalo/ath10k-firmware>
(`QCA9377/hw1.0/`). The distribution copy is `linux-firmware`
(<https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/>,
path `ath10k/QCA9377/hw1.0/`), which is what distros ship and what these
files were taken from (decompressed from the zst-compressed distro files).

sha256 of the exact files in this directory:

```
0fdcc7838f478da81704de88f7b33e28862110c6d5decf7818543f8e37e6cd98  board-2.bin
127d35d82edb46278f30c448cbca664d755ff0d5fed57b649959cdbc4208c768  board.bin
95ed94c24795c31dbdf8c97ab7278dd3a107673ea7330dfe4d01b1c65965f7a8  firmware-5.bin
8f8b002fccfe81d42238f27dd1f56d189604f180bd4772c7c8e75ae1fef16f01  firmware-6.bin
```

To fetch and verify on any Linux machine with the distro firmware
installed:

```
sha256sum -c <(grep -v notice <(sha256sum QCA9377-hw1.0/*) )
```

or simply compare against the table above.

The `board-2.bin` here contains entries for many OEM subsystem IDs; the
driver selects the entry matching the card's subsystem at load time
(`11ad:08a6` on the development machine — selection happens at runtime
from the PCIe config space, nothing is hardcoded).
