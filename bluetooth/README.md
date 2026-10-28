# MT6580 Bluetooth

The source-built AIDL V1 HCI service uses `/dev/stpbt` from the MT6580
WMT/STP kernel integration. It replaces the shipping Android 8.1 vendor
library interface; no original Bluetooth userspace library is installed.
The shared connectivity ROM patches and WMT configuration prepared by
`wifi/` remain required, including for BT-only cold startup.

Prepare this unit's original `BT_Addr` record outside Git:

```sh
python3 device/aic/a1260/tools/prepare_stock_bluetooth.py \
    /private/same-unit/BT_Addr \
    vendor/aic/a1260-bluetooth
```

The tool accepts the original 64-byte payload or 66-byte record, preserves
the first 64 bytes and generates the private product copy rule and hashes.
Do not substitute calibration from another device or commit the payload.
Early vendor init reads the real BT address at `proinfo+0x68`, validates it
and atomically hands it to the HAL in `/data/vendor/bluetooth/factory-address`
(root:bluetooth 0640). Factory partitions remain read-only; the HAL has no
factory block-device access. Wi-Fi and serial provisioning remain separate.

The HAL follows the original MT6580 consys sequence: FC1A address, FC79 RF,
FC93 TX power, FC7A sleep, then HCI Reset. It requires real successful
completions and verifies the resulting address and version. Initialization
fails closed if calibration, identity or the MT6580 controller is absent.
Client death/close cancels RX and releases only the BT power vote. STP reset
or runtime transport loss is reported through HAL death rather than silent
continued operation. H4 command/ACL/SCO boundaries and write sizes are checked;
ISO is unsupported on this controller.

For the verified HCI8/manufacturer70/revision1400/subversion0403 ROM only:

* Probe 0C5A and clear its false Supported Commands bit on actual UNKNOWN.
* Limit LE event-mask bits to 0x1f after reading the encryption-only feature
  bitmap and successfully programming that mask.
* Adapt failed Sniff Subrating 0811 Command Status into the required failed
  Command Complete, preserving the actual error/credits and submitted handle.
  The observed idle error remains 0x1a; neither success nor capabilities are
  fabricated. Ordinary Sniff and genuine SSR support remain available.

`bluetooth.rc` enables A2DP source, AVRCP target, HFP AG and BAS client at
boot. This no-telephony product still needs HFP control for headphone battery
indicators. A2DP alone does not provide that report. BAS is also enabled for
peers that expose the standard GATT battery service. Independent left/right
and case metadata is not implemented or claimed for the test headphone.

## Hardware checkpoint (kernel #199, 2026-10-28)

* Real controller Reset, five original preload commands, address/version
  readback, classic discovery and LE scanning pass.
* Phone SSP pairing and headphone bonding/reconnection pass; normal reboot
  preserves both bonds and automatically reconnects A2DP/HFP.
* Android AudioTrack routes to the headphone, AAC 44.1kHz stereo. The user
  heard the tone; 264600 submitted frames, zero AudioTrack underruns.
* HFP delivers real battery indicators. After normal reboot the actual
  Settings hierarchy displays `已开启。电量：50%。`; subsequent reports include
  40% and 50%. These are peer reports, not a local battery estimator.
* BT off preserves Wi-Fi; both off powers CONN down. BT-only cold startup,
  headphone auto reconnect, then Wi-Fi auto DHCP, five gateway pings and
  HTTPS 200 pass with both radios active. Network errors and kernel taint zero.
* Old error-envelope assertions were reproduced before the adapters. Actual
  post-playback status 0x1a is now handled without another stack crash during
  qualification. The observed normal final boot reports zero Bluetooth crashes.

Evidence is retained locally under `out/a1260-runtime-pm/bluetooth/`:
`raw-reset-test.txt`, `rea/bluetooth-initialization-findings.json`,
`audio-probe/playback-idle-fix.txt`, `hfp-battery-logcat.txt`,
`power-qualification.json`, `manager-final-boot.txt`, `battery-ui.xml` and
`logcat-final-boot.txt`. Personal peer identities and raw factory calibration
are deliberately kept outside the source repositories.

SCO/microphone/call audio, HID, long playback and actual Bluetooth remote wake
from system suspend remain unqualified. CONN retains its existing SPM guard;
radio-on retained standby is not full connected deep power-off. Primary physical
audio is a separate unfinished task; successful Bluetooth audio does not close it.
