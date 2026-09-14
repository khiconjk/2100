#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
GHOST KERNEL - HARDWARE PROFILE RANDOMIZER (Python 3)
Generates authentic Samsung Galaxy S21 profiles with verified Luhn Checksums
==============================================================================
"""

import random
import os
import sys

def calc_luhn(num_str: str) -> int:
    """Computes the standard Luhn (Mod 10) check digit for IMEI."""
    digits = [int(d) for d in num_str]
    checksum = 0
    double = True
    for d in reversed(digits):
        if double:
            val = d * 2
            if val > 9:
                val -= 9
            checksum += val
            double = False
        else:
            checksum += d
            double = True
    return (10 - (checksum % 10)) % 10

def generate_samsung_sn() -> str:
    """Generates 11-char Samsung serial: R5 + Year + Month + 7 alphanumeric."""
    years = ['R', 'T', 'W', 'X', 'Y']  # 2021..2025
    months = ['1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C']
    chars = '0123456789ABCDEFGHJKLMNPQRSTUVWXYZ'
    y = random.choice(years)
    m = random.choice(months)
    body = ''.join(random.choice(chars) for _ in range(7))
    return f"R5{y}{m}{body}"

def generate_imei(tac: str) -> str:
    """Generates 15-digit IMEI from a valid TAC with Luhn check digit."""
    snr = ''.join(str(random.randint(0, 9)) for _ in range(6))
    body = f"{tac}{snr}"
    check = calc_luhn(body)
    return f"{body}{check}"

def generate_macs():
    """Generates authentic Wi-Fi & Bluetooth MAC with Samsung OUI."""
    ouis = [
        "A4:75:B9", "00:12:FB", "24:4B:FE", "8C:77:12",
        "5C:E9:1E", "E8:50:8B", "30:CD:A7", "F4:7B:5E"
    ]
    oui = random.choice(ouis)
    b4 = f"{random.randint(0, 255):02X}"
    b5 = f"{random.randint(0, 255):02X}"
    b6_int = random.randint(0, 253)
    b6 = f"{b6_int:02X}"
    b6_bt = f"{(b6_int + 1):02X}"
    return f"{oui}:{b4}:{b5}:{b6}", f"{oui}:{b4}:{b5}:{b6_bt}"

def generate_ghost_conf() -> tuple:
    """Builds the complete ghost.conf content."""
    # Authentic Samsung Galaxy S21 5G (SM-G991B) TACs
    tacs = [
        "35971387", "35966984", "35918923", "35906784", "35895793",
        "35844692", "35833295", "35814433", "35787240", "35471978"
    ]
    tac1, tac2 = random.sample(tacs, 2)
    imei1 = generate_imei(tac1)
    imei2 = generate_imei(tac2)
    sn = generate_samsung_sn()
    wifi_mac, bt_mac = generate_macs()

    ufs_models = [
        "KLUDG8UHDB-C2D1", "KLUEG8UHDB-C2D1", "KLUCG4J1ED-B0C1",
        "HN8T05BZGKX015", "KM5V7001DM-B621"
    ]
    ufs_model = random.choice(ufs_models)
    ufs_serial = f"0x{random.randint(0x10000000, 0xffffffff):08x}"

    uptime_days = random.randint(4, 45)
    boot_count = random.randint(15, 80)
    battery_cycle = random.randint(45, 320)
    battery_health = max(86, min(99, 100 - (battery_cycle // 40)))
    sensor_bias = f"{random.randint(-20, 20)},{random.randint(-20, 20)},{random.randint(-20, 20)}"
    tcp_isn = f"0x{random.randint(0x10000000, 0xffffffff):08x}"

    conf = f"""# ==============================================================================
# GHOST KERNEL 100% PURE KERNEL CONFIGURATION FILE
# ==============================================================================

[DEVICE]
model = SM-G991B
product = o1sxeea
device = o1s
manufacturer = samsung
brand = samsung
soc_machine = Exynos
soc_family = samsung

[FINGERPRINT]
build_fingerprint = samsung/o1sxeea/o1s:12/SP1A.210812.016/SM-G991BXXS3BULC:user/release-keys
build_desc = o1sxeea-user 12 SP1A.210812.016 SM-G991BXXS3BULC release-keys
build_id = SP1A.210812.016
security_patch = 2024-08-01

[HARDWARE_IDS]
serialno = {sn}
imei = {imei1}
imei2 = {imei2}
wifi_mac = {wifi_mac}
bt_mac = {bt_mac}
ufs_serial = {ufs_serial}
ufs_model = {ufs_model}

[ENVIRONMENT_METRICS]
uptime_days = {uptime_days}
boot_count = {boot_count}
battery_cycle = {battery_cycle}
battery_health = {battery_health}
sensor_bias = {sensor_bias}
tcp_isn_offset = {tcp_isn}
"""
    return conf, {
        "Serial Number": sn,
        "Cellular IMEI 1": imei1,
        "Cellular IMEI 2": imei2,
        "Wi-Fi MAC": wifi_mac,
        "Bluetooth Address": bt_mac,
        "UFS Chip Model": ufs_model,
        "UFS Serial": ufs_serial,
        "Battery Stats": f"{battery_cycle} cycles ({battery_health}% health)",
        "Uptime": f"{uptime_days} days ({boot_count} boots)"
    }

if __name__ == "__main__":
    conf_text, metadata = generate_ghost_conf()
    out_file = sys.argv[1] if len(sys.argv) > 1 else "ghost.conf"
    with open(out_file, "w", encoding="utf-8") as f:
        f.write(conf_text)
    print(f"[+] Generated authentic ghost.conf at: {os.path.abspath(out_file)}")
    print("-------------------------------------------------------------")
    for k, v in metadata.items():
        print(f"  * {k:18}: {v}")
    print("-------------------------------------------------------------")
