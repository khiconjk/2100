# Antigravity Handoff: Post-Flash Device Audit Report

## Purpose

This file reports the read-only runtime audit performed after flashing the latest build to the target device. Use these results to update the implementation plan before proposing any code changes.

Do not treat this report as approval to continue adding virtualization features. The build boots, but several stated requirements remain unfulfilled or unproven.

## Safety Boundary Used During Audit

- Device checks were read-only.
- No flash, reboot, wipe, reset or format command was executed.
- No system setting, clock, property, filesystem or EFS content was intentionally changed.
- No source code or original implementation plan was modified.
- Sensitive serial, IMEI and MAC values are redacted in this report.

## Executive Result

The flashed kernel is bootable and no strict kernel crash signature was observed. Profile loading, primary serial presentation, basic UFS presentation and battery barcode masking work.

The build does **not** satisfy the complete K-DIV plan. Security patch presentation, Wi-Fi/P2P/Bluetooth profile alignment, Android Telephony integration, full UFS identity coverage and cross-profile uptime consistency failed or remain unproven.

Status:

```text
BOOTABILITY=PASS
ORIGINAL_RUNTIME_ACCEPTANCE_ITEMS=4_PASS_1_FAIL
EXTENDED_CROSS_SURFACE_AUDIT=FAIL
ZERO_DISK_CLAIM=NOT_PROVEN
RELEASE_ACCEPTANCE=FAIL
```

## Target Environment

```text
device=o1s
model=SM-G991B
Android=12
kernel=5.4.129-22936777-abG991BXXS3BULC
SELinux=Enforcing
sys.boot_completed=1
root_context=u:r:ksu:s0
KernelSU userspace=3.2.0
SuSFS=v2.1.0
SuSFS variant=GKI
```

Core Android services found:

```text
media.camera=found
wifi=found
phone=found
bluetooth_manager=found
```

Strict dmesg scan result:

```text
BUG/Oops/Kernel panic/Call trace/filesystem fatal error count=0
```

This proves basic bootability only. It does not prove long-term stability.

## Acceptance Matrix

| Area | Result | Runtime evidence | Required interpretation |
| --- | --- | --- | --- |
| Profile loader | PASS | `/proc/ghost_config` reports `Source Status: /efs/ghost.conf` | The previous fallback symptom is no longer present on this boot. |
| Serial properties | PASS | `ro.serialno` and `ro.boot.serialno` match the active profile | Primary property surfaces are aligned. |
| EFS serial output | PASS for presentation | Root read of `/efs/FactoryApp/serial_no` matches the profile | This does not prove underlying EFS was not modified. |
| USB gadget serial | PASS | ConfigFS gadget serial matches the profile | USB surface is aligned for this boot. |
| Serial partial reads | PASS for observed cases | Read sizes 1, 4, 11, 12 and 4096 bytes produced the same 12-byte result and SHA-256 | No partial-read disclosure was observed, but raw disk integrity remains unproven. |
| UFS vendor/model/rev | PASS | `SAMSUNG`, expected model, revision `0100` | Only the three basic sysfs attributes passed. |
| Full UFS/SCSI identity | PARTIAL/FAIL | WWID and VPD pages remain independently populated | Do not claim complete UFS virtualization. |
| Battery barcode | PASS | `batt_type` contains the masked token `SEC1000AA` | Basic battery serial masking works. |
| Security patch display | FAIL | Build and vendor security patch properties both report `2022-01-01` | Expected `2024-05-01` was not applied. |
| Ghost IMEI proc endpoint | PASS | `/proc/ghost_imei` contains two profile IMEIs | This validates only the private Ghost endpoint. |
| Android Telephony IMEI | FAIL/UNPROVEN | `dumpsys iphonesubinfo` did not expose corresponding profile data | `/proc/ghost_imei` is not proof of Binder/RIL integration. |
| Telephony registration | FAIL | Framework reports `OUT_OF_SERVICE` and `NOT_REG_OR_SEARCHING`; network type is unknown | Telephony state is not aligned with the proposed profile. |
| Wi-Fi MAC profile alignment | FAIL | `wlan0` uses an Android randomized MAC different from `ghost_active_profile.wifi_mac` | This may be normal Android privacy behavior, but it fails the plan's profile-equality requirement. |
| P2P MAC profile alignment | FAIL | `p2p0` uses another address, also different from the profile | Current `wlan*` matching does not cover `p2p0`. |
| Bluetooth MAC profile alignment | FAIL | Framework Bluetooth address differs from `ghost_active_profile.bt_mac` | Proposed plan has no complete Bluetooth owner/hook. |
| Ghost uptime mechanism | PASS/PARTIAL | `/proc/ghost_uptime` reports ready, session mode and an offset inside 15-25 days | Runtime offset works, but it does not match the configured `Uptime Age`. |
| Time consistency | PASS for proc surface | `/proc/uptime` and `btime` agree with the runtime offset | Additional filesystem timestamps remain inconsistent. |
| Kernel crash signatures | PASS | Strict filtered count is zero | Short observation window only. |
| Reproducible build of six packages | NOT TESTED ON DEVICE | One flashed artifact boots | This cannot validate the other packages or source provenance. |

## Detailed Findings

### 1. Profile Loading Is Fixed for This Boot

Observed:

```text
Source Status: /efs/ghost.conf
```

The prior `[DEFAULT_FALLBACK]` result is gone. Do not implement another PID-1 root namespace change without first checking whether the current source already contains it. It does.

Required plan update:

- Mark the original fallback issue as resolved for this boot.
- Retain diagnostic/error telemetry for missing, malformed or late-mounted config files.
- Do not rewrite the same loader mechanism as a new proposed change.

### 2. Primary Serial Surfaces Are Aligned

The following returned the same redacted Samsung-format serial:

```text
ADB device serial
ro.serialno
ro.boot.serialno
/efs/FactoryApp/serial_no output
/config/usb_gadget/g1/strings/0x409/serialnumber
```

Partial-read checks also returned an identical 12-byte value for all tested read sizes.

However, this is only presentation-level consistency.

### 3. Zero-Disk Is Still Not Proven

Reading `/efs/FactoryApp/serial_no` through the active kernel can be intercepted. The same read path therefore cannot prove what is physically stored on EFS.

The inspected source also contains calls that write serial/seed values to `/data` and `/efs`, including the factory serial path. Therefore:

```text
ZERO_DISK_RESULT=NOT_PROVEN
```

Required plan update:

- Do not state that zero-disk has passed.
- Inventory and remove all Ghost write paths before using the RAM-only label.
- Define whether identity is session-only or persistent. It cannot be both persistent-on-disk and zero-disk.
- Require a verification method independent of the active interceptor and based on an authorized pre-flash baseline.

### 4. Basic UFS Values Pass, Full Identity Does Not

Observed basic values:

```text
vendor=SAMSUNG
model=KLUDG8UHDB-C2D1
rev=0100
```

Additional surfaces remain populated independently:

```text
wwid=present
vpd_pg80=present and non-empty
vpd_pg83=present and non-empty
```

Required plan update:

- Change “complete UFS virtualization” to “basic sysfs vendor/model/rev override” unless every additional surface is inventoried and validated.
- Do not assume `/sys/class/scsi_host/...` follows these three overrides.
- Validate consistency among model, revision, WWID, VPD data and capacity before claiming a coherent device profile.

### 5. Battery Barcode Masking Passes

Observed `batt_type` contains:

```text
+SEC1000AA+
```

The previously visible battery barcode segment is not present in the tested output.

Required plan update:

- Mark the basic battery output test as passed.
- Identify one single implementation owner. Do not add a duplicate VFS layer if the driver sysfs show function already owns the transformation.

### 6. Security Patch Override Fails

Observed:

```text
ro.build.version.security_patch=2022-01-01
ro.vendor.build.security_patch=2022-01-01
expected=2024-05-01
```

Required interpretation:

- The current property patcher did not produce the expected result.
- Do not describe the device as patched to May 2024.
- Changing the displayed property would not backport any actual CVE fix.

Required plan update:

- Prefer removing this item from K-DIV acceptance.
- If retained for a controlled lab, name it `display-only property override` and explicitly state that the true firmware patch level remains unchanged.
- Do not use it as a security or compatibility guarantee.

### 7. IMEI Works Only in the Private Proc Endpoint

Observed:

```text
/proc/ghost_imei contains IMEI1 and IMEI2 from the profile.
dumpsys iphonesubinfo does not demonstrate those values.
```

This confirms the architecture concern from the previous plan review: Android Telephony does not use `/proc/ghost_imei` as its authoritative data source.

Required plan update:

- Separate `Ghost diagnostic endpoint` from `Android Telephony identity`.
- Do not mark telephony integration PASS based on `/proc/ghost_imei`.
- Define the actual consumer and authoritative source before proposing any implementation.

### 8. Telephony State Fails

Observed framework state includes:

```text
mVoiceRegState=OUT_OF_SERVICE
mDataRegState=OUT_OF_SERVICE
registrationState=NOT_REG_OR_SEARCHING
MobileData=OUT_OF_SERVICE
network type=Unknown
```

SIM properties identify Viettel/45204 in some fields, but operator properties are empty or malformed and framework state remains unregistered.

Required plan update:

- Do not claim telephony alignment.
- Do not treat property strings as proof of actual modem registration.
- Keep genuine radio/modem state separate from any display-only profile field.

### 9. Wi-Fi, P2P and Bluetooth Do Not Match the Profile

Observed:

- `wlan0` sysfs, `ip link` and Wi-Fi framework agree with each other, but use Android's randomized MAC rather than the Ghost profile MAC.
- `p2p0` uses a separate address and is not covered by the existing `wlan*` condition.
- Bluetooth framework address differs from the Ghost profile Bluetooth address.

Required interpretation:

- The Wi-Fi value may be legitimate per-network MAC randomization rather than a physical-MAC leak.
- It still fails the plan's explicit “all surfaces equal the profile” requirement.
- Bluetooth was present in the audit finding but absent from the proposed implementation details.

Required plan update:

- Decide whether Android randomized MAC should remain authoritative. Do not override privacy behavior merely to force profile equality without a justified requirement.
- Add a specific Bluetooth scope/owner/test or remove Bluetooth from the claim.
- Do not claim complete MAC virtualization based only on sysfs `address_show()`.

### 10. Uptime Runtime Works but Conflicts With Profile

Observed:

```text
mode=session
ready=1
offset_secs=1464616
range_days=15..25
realtime_mode=off
rtc_policy=unchanged
```

`1464616` seconds is approximately `16.95` days. `/proc/uptime` and `btime` agree with this offset.

The loaded Ghost profile reports:

```text
Uptime Age=24 days
```

Required plan update:

- Define whether `uptime_days` is authoritative or informational.
- Use one owner for runtime offset and reported profile age.
- Do not display 24 days while runtime surfaces use approximately 16.95 days if cross-surface consistency is a product requirement.

### 11. Kernel Stability Observations

No strict crash signatures were found:

```text
BUG:=0
Oops=0
Kernel panic=0
Call trace=0
EXT4/F2FS fatal error=0
```

Non-fatal observations:

- Kernel audit reported lost events and queue overflow/rate limiting.
- One transient I2C `NO ACK` sequence was observed.
- Kernel log is very noisy, which can displace early Ghost initialization evidence from the ring buffer.

Required plan update:

- Add a longer soak test.
- Track audit queue loss separately from crash status.
- Do not declare stability from one short post-boot observation.

## Corrected Overall Verdict

Do not mark the latest build as fully accepted.

The defensible verdict is:

```text
Latest build is bootable.
Profile loader: fixed for observed boot.
Primary serial surfaces: aligned.
Basic UFS attributes: aligned.
Battery barcode output: masked.
Security patch override: failed.
Android Telephony integration: failed or unproven.
Wi-Fi/P2P/Bluetooth profile alignment: failed.
Full UFS identity consistency: unproven.
Zero-disk guarantee: unproven and contradicted by inspected write paths.
Long-term stability and six-package reproducibility: unproven.
```

## Required Next Action for Antigravity

Do not modify code immediately from the old implementation plan.

First update the plan using these rules:

1. Mark already-working items as observed behavior, not proposed changes.
2. Add a Phase 0 source/build integrity gate because the Windows working tree contains missing Ghost components and stale build references.
3. Remove or explicitly resolve every Ghost write path before claiming zero-disk.
4. Treat security patch display separately from actual security updates.
5. Treat `/proc/ghost_imei` separately from Android Telephony.
6. Decide whether Android Wi-Fi MAC randomization should remain authoritative.
7. Add the missing Bluetooth scope or remove Bluetooth from the claim.
8. Inventory WWID/VPD and other UFS/SCSI surfaces before claiming complete storage virtualization.
9. Reconcile configured uptime age with the runtime session offset.
10. Require clean reproducible builds and artifact provenance before another flash candidate.

For every unresolved item, use this template:

```text
Observed evidence:
Acceptance result: PASS / FAIL / PARTIAL / NOT PROVEN
Root cause status: proven / hypothesis / unknown
Authoritative source:
Consumer/API actually tested:
Smallest diagnostic step:
Safety boundary:
Success criterion:
Stop condition:
```

Do not convert a failed or unproven item into a code change until the authoritative data source and root cause are identified.
