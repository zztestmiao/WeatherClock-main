#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pack_firmware.py —— 把 Keil 生成的 bin 打包成带 1KB 头的 OTA 镜像

镜像布局：
    偏移 0x000  1KB 镜像头（本脚本生成）
    偏移 0x400  Keil 生成的裸 bin（必须链接到 slot_base + 0x400）

用法：
    # 生成可在 Slot A 运行的镜像
    python pack_firmware.py Objects/app.bin -o out/app_slotA.bin --slot A --version 1.2.3

    # 生成可在 Slot B 运行的镜像（同一份源码，链接地址不同）
    python pack_firmware.py Objects/app.bin -o out/app_slotB.bin --slot B --version 1.2.3

注意：Slot A 和 Slot B 的链接地址不同，所以必须编译两次！
      见 docs/OTA-双区Bootloader实施手册.md 第 2 章。
"""
import argparse
import hashlib
import json
import os
import struct
import time
import zlib

HEADER_SIZE = 0x400
IMG_MAGIC = 0x494D4731          # 'IMG1'
HEADER_VERSION = 1
MAX_PAYLOAD = 207 * 1024        # SLOT_A_SIZE(208KB) - 1KB

SLOT_BASE = {
    "A": 0x0800C000,
    "B": 0x08040000,
}


def parse_version(text):
    parts = text.lstrip("vV").split(".")
    if len(parts) != 3:
        raise ValueError("版本号格式应为 major.minor.patch，例如 1.2.3")
    major, minor, patch = (int(p) for p in parts)
    for v in (major, minor, patch):
        if not 0 <= v <= 255:
            raise ValueError("版本号每段必须在 0~255 之间")
    return (major << 16) | (minor << 8) | patch


def build_header(payload, load_addr, version, flags, git_hash, build_time):
    hdr = bytearray(HEADER_SIZE)

    crc = zlib.crc32(payload) & 0xFFFFFFFF

    struct.pack_into(
        "<IIIIIIII",
        hdr, 0,
        IMG_MAGIC,
        HEADER_VERSION,
        version,
        load_addr,
        len(payload),
        crc,
        build_time,
        flags,
    )
    hdr[32:40] = git_hash[:8].ljust(8, b"\x00")

    # header_crc32 覆盖前 1020 字节
    header_crc = zlib.crc32(bytes(hdr[:HEADER_SIZE - 4])) & 0xFFFFFFFF
    struct.pack_into("<I", hdr, HEADER_SIZE - 4, header_crc)
    return bytes(hdr), crc


def main():
    ap = argparse.ArgumentParser(description="打包 OTA 镜像（加 1KB 头 + CRC32）")
    ap.add_argument("bin", help="Keil 生成的裸 bin")
    ap.add_argument("-o", "--out", required=True, help="输出文件")
    ap.add_argument("--slot", choices=["A", "B"], default="A", help="目标槽")
    ap.add_argument("--base", default=None,
                    help="镜像加载地址，默认按 --slot 推导（A=0x0800C400, B=0x08040400）")
    ap.add_argument("--version", default="1.0.0", help="版本号 major.minor.patch")
    ap.add_argument("--flags", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--git-hash", default="", help="可选，前 8 位 commit")
    args = ap.parse_args()

    with open(args.bin, "rb") as f:
        payload = f.read()

    if not payload:
        raise SystemExit("输入 bin 为空")

    # 对齐到 4 字节（Flash 按字编程）
    pad = (-len(payload)) % 4
    if pad:
        payload += b"\xFF" * pad

    if len(payload) > MAX_PAYLOAD:
        raise SystemExit(
            f"镜像太大：{len(payload)} 字节，上限 {MAX_PAYLOAD} 字节（207KB）。\n"
            f"当前固件把字库和图标都放在内部 Flash，需要先瘦身，见手册第 1 章。"
        )

    base = SLOT_BASE[args.slot] + HEADER_SIZE
    if args.base:
        base = int(args.base, 0)

    # 健壮性检查：裸 bin 的第一个字必须是栈顶指针（0x2000xxxx）
    sp = struct.unpack_from("<I", payload, 0)[0]
    if (sp & 0xFFF00000) != 0x20000000:
        raise SystemExit(
            f"这个 bin 的第一个字是 0x{sp:08X}，不像合法的栈顶指针。\n"
            f"说明它不是链接到 0x{base:08X} 的镜像（可能还链接在 0x08000000）。\n"
            f"请在 Keil 里把 IROM1 起始地址改成 0x{base:08X} 后重新编译并生成 bin。"
        )

    version = parse_version(args.version)
    hdr, crc = build_header(
        payload,
        load_addr=base,
        version=version,
        flags=args.flags,
        git_hash=args.git_hash.encode()[:8],
        build_time=int(time.time()),
    )

    image = hdr + payload

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(image)

    manifest = {
        "file": os.path.basename(args.out),
        "slot": args.slot,
        "version": args.version,
        "version_u32": f"0x{version:06X}",
        "load_addr": f"0x{base:08X}",
        "image_size": len(image),
        "payload_size": len(payload),
        "crc32": f"0x{crc:08X}",
        "sha256": hashlib.sha256(image).hexdigest(),
        "build_time": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    with open(args.out + ".json", "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    print(f"[OK] {args.out}")
    print(f"     slot      : {args.slot}")
    print(f"     version   : {args.version}  (0x{version:06X})")
    print(f"     load_addr : 0x{base:08X}")
    print(f"     payload   : {len(payload)} bytes")
    print(f"     image     : {len(image)} bytes")
    print(f"     crc32     : 0x{crc:08X}")
    if pad:
        print(f"     (payload 补齐了 {pad} 字节 0xFF)")


if __name__ == "__main__":
    main()
