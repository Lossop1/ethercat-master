#!/usr/bin/env python3
"""从一个 ELF 里取符号的"文件偏移"，给 ftrace 的 uprobe 用。

uprobe 注册按 inode + 文件偏移，`nm` 给的是虚拟地址；段对齐不齐时两者不等，
所以不能直接抄 nm 的数——要按 PT_LOAD 段换算一次。

输出每行：<符号名> <十六进制文件偏移> <十六进制虚拟地址>
取不到的行标 MISSING，不猜。

用法：python3 scripts/analysis/uprobe_offsets.py <elf 文件> <符号名> [符号名...]
"""
import struct
import sys


def load_elf(path):
    with open(path, "rb") as handle:
        return handle.read()


def segments(data):
    """PT_LOAD 段：(vaddr, file_offset, filesz)。"""
    e_phoff, = struct.unpack_from("<Q", data, 0x20)
    e_phentsize, = struct.unpack_from("<H", data, 0x36)
    e_phnum, = struct.unpack_from("<H", data, 0x38)
    out = []
    for index in range(e_phnum):
        head = e_phoff + index * e_phentsize
        p_type, = struct.unpack_from("<I", data, head)
        if p_type != 1:
            continue
        p_offset, p_vaddr, _paddr, p_filesz = struct.unpack_from("<QQQQ", data, head + 8)
        out.append((p_vaddr, p_offset, p_filesz))
    return out


def symbols(data):
    """产出 (名字, 值)，只扫 SYMTAB / DYNSYM。"""
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, = struct.unpack_from("<H", data, 0x3A)
    e_shnum, = struct.unpack_from("<H", data, 0x3C)
    for index in range(e_shnum):
        head = e_shoff + index * e_shentsize
        sh_type, = struct.unpack_from("<I", data, head + 4)
        if sh_type not in (2, 11):
            continue
        sh_offset, = struct.unpack_from("<Q", data, head + 0x18)
        sh_size, = struct.unpack_from("<Q", data, head + 0x20)
        sh_link, = struct.unpack_from("<I", data, head + 0x28)
        sh_entsize, = struct.unpack_from("<Q", data, head + 0x38)
        if sh_entsize == 0:
            continue
        str_head = e_shoff + sh_link * e_shentsize
        strtab, = struct.unpack_from("<Q", data, str_head + 0x18)
        for entry in range(sh_size // sh_entsize):
            at = sh_offset + entry * sh_entsize
            st_name, = struct.unpack_from("<I", data, at)
            st_value, = struct.unpack_from("<Q", data, at + 8)
            if st_name == 0 or st_value == 0:
                continue
            end = data.find(b"\0", strtab + st_name)
            yield data[strtab + st_name:end].decode("utf-8", "replace"), st_value


def main():
    path = sys.argv[1]
    wanted = sys.argv[2:]
    data = load_elf(path)
    loads = segments(data)
    table = {}
    for name, value in symbols(data):
        table.setdefault(name, value)
    for name in wanted:
        value = table.get(name)
        if value is None:
            print(f"{name} MISSING")
            continue
        offset = None
        for vaddr, file_offset, filesz in loads:
            if vaddr <= value < vaddr + filesz:
                offset = value - vaddr + file_offset
                break
        if offset is None:
            print(f"{name} MISSING(段外)")
            continue
        print(f"{name} {offset:#x} {value:#x}")


main()
