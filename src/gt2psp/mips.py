"""MIPS R3000A (PlayStation CPU) instruction decoder, including COP0 and the GTE (COP2).

The decoder is shared by the analysis tools and the recompiler. ``decode(word, pc)`` returns an ``Insn`` with the
mnemonic, raw fields and control-flow information; ``Insn.text()`` renders assembler syntax.
"""

from __future__ import annotations

from dataclasses import dataclass, field

REG = [
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
]

# GTE commands (COP2 with bit 25 set), keyed by the low 6 bits of the instruction.
GTE_OPS = {
    0x01: "rtps", 0x06: "nclip", 0x0C: "op", 0x10: "dpcs", 0x11: "intpl", 0x12: "mvmva", 0x13: "ncds",
    0x14: "cdp", 0x16: "ncdt", 0x1B: "nccs", 0x1C: "cc", 0x1E: "ncs", 0x20: "nct", 0x28: "sqr",
    0x29: "dcpl", 0x2A: "dpct", 0x2D: "avsz3", 0x2E: "avsz4", 0x30: "rtpt", 0x3D: "gpf", 0x3E: "gpl",
    0x3F: "ncct",
}

# Control-flow kinds
K_NONE = 0
K_BRANCH = 1      # conditional branch, target = imm (has delay slot)
K_JUMP = 2        # j target (unconditional, has delay slot)
K_CALL = 3        # jal / bltzal / bgezal (has delay slot)
K_JR = 4          # jr rs (return or computed jump)
K_JALR = 5        # jalr rd, rs (computed call)
K_TRAP = 6        # syscall / break


@dataclass
class Insn:
    pc: int
    word: int
    op: str
    rs: int = 0
    rt: int = 0
    rd: int = 0
    sa: int = 0
    imm: int = 0          # sign-extended 16-bit immediate (or zero-extended for logical ops)
    target: int | None = None
    kind: int = K_NONE
    fmt: str = ""         # operand layout used by text()
    extra: dict = field(default_factory=dict)

    @property
    def has_delay_slot(self) -> bool:
        return self.kind in (K_BRANCH, K_JUMP, K_CALL, K_JR, K_JALR)

    def text(self) -> str:
        r = REG
        f = self.fmt
        if f == "":
            return self.op
        if f == "d,s,t":
            return f"{self.op} {r[self.rd]}, {r[self.rs]}, {r[self.rt]}"
        if f == "d,t,s":
            return f"{self.op} {r[self.rd]}, {r[self.rt]}, {r[self.rs]}"
        if f == "d,t,sa":
            return f"{self.op} {r[self.rd]}, {r[self.rt]}, {self.sa}"
        if f == "s,t":
            return f"{self.op} {r[self.rs]}, {r[self.rt]}"
        if f == "d":
            return f"{self.op} {r[self.rd]}"
        if f == "s":
            return f"{self.op} {r[self.rs]}"
        if f == "d,s":
            return f"{self.op} {r[self.rd]}, {r[self.rs]}"
        if f == "t,s,i":
            return f"{self.op} {r[self.rt]}, {r[self.rs]}, {self.imm}"
        if f == "t,s,u":
            return f"{self.op} {r[self.rt]}, {r[self.rs]}, 0x{self.imm:x}"
        if f == "t,u":
            return f"{self.op} {r[self.rt]}, 0x{self.imm:x}"
        if f == "t,o(s)":
            return f"{self.op} {r[self.rt]}, {self.imm}({r[self.rs]})"
        if f == "c2t,o(s)":
            return f"{self.op} ${self.rt}, {self.imm}({r[self.rs]})"
        if f == "s,t,b":
            return f"{self.op} {r[self.rs]}, {r[self.rt]}, 0x{self.target:08x}"
        if f == "s,b":
            return f"{self.op} {r[self.rs]}, 0x{self.target:08x}"
        if f == "j":
            return f"{self.op} 0x{self.target:08x}"
        if f == "t,cd":
            return f"{self.op} {r[self.rt]}, ${self.rd}"
        if f == "code":
            return f"{self.op} 0x{self.extra.get('code', 0):x}"
        if f == "gte":
            return f"{self.op} 0x{self.word & 0x1FFFFFF:07x}"
        return f"{self.op} ?"


def _sx16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


_SPECIAL = {
    0x00: ("sll", "d,t,sa"), 0x02: ("srl", "d,t,sa"), 0x03: ("sra", "d,t,sa"),
    0x04: ("sllv", "d,t,s"), 0x06: ("srlv", "d,t,s"), 0x07: ("srav", "d,t,s"),
    0x08: ("jr", "s"), 0x09: ("jalr", "d,s"), 0x0C: ("syscall", "code"), 0x0D: ("break", "code"),
    0x10: ("mfhi", "d"), 0x11: ("mthi", "s"), 0x12: ("mflo", "d"), 0x13: ("mtlo", "s"),
    0x18: ("mult", "s,t"), 0x19: ("multu", "s,t"), 0x1A: ("div", "s,t"), 0x1B: ("divu", "s,t"),
    0x20: ("add", "d,s,t"), 0x21: ("addu", "d,s,t"), 0x22: ("sub", "d,s,t"), 0x23: ("subu", "d,s,t"),
    0x24: ("and", "d,s,t"), 0x25: ("or", "d,s,t"), 0x26: ("xor", "d,s,t"), 0x27: ("nor", "d,s,t"),
    0x2A: ("slt", "d,s,t"), 0x2B: ("sltu", "d,s,t"),
}

_IMM = {
    0x08: ("addi", "t,s,i"), 0x09: ("addiu", "t,s,i"), 0x0A: ("slti", "t,s,i"), 0x0B: ("sltiu", "t,s,i"),
    0x0C: ("andi", "t,s,u"), 0x0D: ("ori", "t,s,u"), 0x0E: ("xori", "t,s,u"), 0x0F: ("lui", "t,u"),
    0x20: ("lb", "t,o(s)"), 0x21: ("lh", "t,o(s)"), 0x22: ("lwl", "t,o(s)"), 0x23: ("lw", "t,o(s)"),
    0x24: ("lbu", "t,o(s)"), 0x25: ("lhu", "t,o(s)"), 0x26: ("lwr", "t,o(s)"),
    0x28: ("sb", "t,o(s)"), 0x29: ("sh", "t,o(s)"), 0x2A: ("swl", "t,o(s)"), 0x2B: ("sw", "t,o(s)"),
    0x2E: ("swr", "t,o(s)"),
    0x32: ("lwc2", "c2t,o(s)"), 0x3A: ("swc2", "c2t,o(s)"),
}

LOADS = {"lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr"}
STORES = {"sb", "sh", "swl", "sw", "swr"}


def decode(word: int, pc: int) -> Insn:
    opc = word >> 26
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    rd = (word >> 11) & 31
    sa = (word >> 6) & 31
    fn = word & 63
    imm16 = word & 0xFFFF

    if word == 0:
        return Insn(pc, word, "nop")

    if opc == 0x00:
        ent = _SPECIAL.get(fn)
        if ent is None:
            return Insn(pc, word, "invalid", fmt="")
        op, fmt = ent
        ins = Insn(pc, word, op, rs, rt, rd, sa, fmt=fmt)
        if op == "jr":
            ins.kind = K_JR
        elif op == "jalr":
            ins.kind = K_JALR
        elif op in ("syscall", "break"):
            ins.kind = K_TRAP
            ins.extra["code"] = (word >> 6) & 0xFFFFF
        return ins

    if opc == 0x01:  # REGIMM
        sub = rt
        names = {0x00: "bltz", 0x01: "bgez", 0x10: "bltzal", 0x11: "bgezal"}
        # The R3000A decodes only bit 0 and bit 4 of rt (bits 1-3 ignored); stay strict here and
        # flag unusual encodings as invalid so analysis notices data.
        if sub not in names:
            return Insn(pc, word, "invalid")
        op = names[sub]
        tgt = (pc + 4 + (_sx16(imm16) << 2)) & 0xFFFFFFFF
        return Insn(pc, word, op, rs, rt, imm=_sx16(imm16), target=tgt,
                    kind=K_CALL if sub & 0x10 else K_BRANCH, fmt="s,b")

    if opc in (0x02, 0x03):
        tgt = ((pc + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
        return Insn(pc, word, "j" if opc == 2 else "jal", target=tgt,
                    kind=K_JUMP if opc == 2 else K_CALL, fmt="j")

    if opc in (0x04, 0x05, 0x06, 0x07):
        op = {4: "beq", 5: "bne", 6: "blez", 7: "bgtz"}[opc]
        tgt = (pc + 4 + (_sx16(imm16) << 2)) & 0xFFFFFFFF
        if opc in (6, 7):
            if rt != 0:
                return Insn(pc, word, "invalid")
            return Insn(pc, word, op, rs, rt, imm=_sx16(imm16), target=tgt, kind=K_BRANCH, fmt="s,b")
        # canonical aliases
        if opc == 4 and rs == 0 and rt == 0:
            return Insn(pc, word, "b", rs, rt, imm=_sx16(imm16), target=tgt, kind=K_BRANCH,
                        fmt="j", extra={"always": True})
        return Insn(pc, word, op, rs, rt, imm=_sx16(imm16), target=tgt, kind=K_BRANCH, fmt="s,t,b")

    if opc in _IMM:
        op, fmt = _IMM[opc]
        if fmt in ("t,s,u", "t,u"):
            v = imm16
        else:
            v = _sx16(imm16)
        return Insn(pc, word, op, rs, rt, imm=v, fmt=fmt)

    if opc == 0x10:  # COP0
        if rs == 0x00:
            return Insn(pc, word, "mfc0", rs, rt, rd, fmt="t,cd")
        if rs == 0x04:
            return Insn(pc, word, "mtc0", rs, rt, rd, fmt="t,cd")
        if rs == 0x10 and fn == 0x10:
            return Insn(pc, word, "rfe")
        return Insn(pc, word, "invalid")

    if opc == 0x12:  # COP2 / GTE
        if word & (1 << 25):
            name = GTE_OPS.get(fn)
            if name is None:
                return Insn(pc, word, "invalid")
            return Insn(pc, word, name, fmt="gte", extra={"gte": True, "sf": (word >> 19) & 1,
                                                          "lm": (word >> 10) & 1})
        names = {0x00: "mfc2", 0x02: "cfc2", 0x04: "mtc2", 0x06: "ctc2"}
        if rs in names:
            return Insn(pc, word, names[rs], rs, rt, rd, fmt="t,cd")
        return Insn(pc, word, "invalid")

    return Insn(pc, word, "invalid")
