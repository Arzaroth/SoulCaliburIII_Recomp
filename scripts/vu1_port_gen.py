#!/usr/bin/env python3
"""SC3's VU1 programs A and B and its VU0 microprograms as C++, from the microcode ps2_vu_aot compiled
(generated/vu1): each instruction as the operation it does on registers held in locals, with what VU1Interpreter's
pipeline makes timing-dependent (stalls and cycles, when Q, MAC and clip results become visible, the VI a branch
reads, XGKICK waits) worked out here for every path from each start pc, so the C++ runs no pipeline.
scripts/recomp.sh runs it.

  scripts/vu1_port_gen.py <ps2_vu_aot output dir> <out dir>     writes sc3_vu1_a.cpp, sc3_vu1_b.cpp and sc3_vu0_program.cpp
  scripts/vu1_port_gen.py --code <image.bin> <A|B|VU0> <out.cpp>   from a code image instead
"""
import argparse
import glob
import os
import re
import struct
import sys


BC = 'xyzw'
FMAC_LATENCY = 4
ACC_FORWARD = 1


def lane_for_component(c):
    return 1 << (3 - c)


def sext(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


class Usage:
    def __init__(self):
        self.vf_read = []  # (reg, lanes)
        self.vf_write = None  # (reg, lanes)
        self.vi_read = 0
        self.vi_write = 0
        self.acc_read = 0
        self.acc_write = 0
        self.latency = 0
        self.vf_latency = 0
        self.vi_latency = 0
        self.pipeline = None
        self.wait_q = False
        self.reads_clip = False
        self.writes_clip = False
        self.delays_branch = False
        self.reserved = False

    def add_vf_read(self, reg, lanes):
        if lanes == 0:
            return
        for i, (r, l) in enumerate(self.vf_read):
            if r == reg:
                self.vf_read[i] = (r, l | lanes)
                return
        if len(self.vf_read) < 2:
            self.vf_read.append((reg, lanes))

    def add_vf_write(self, reg, lanes):
        if reg == 0 or lanes == 0:
            return
        if self.vf_write is None:
            self.vf_write = (reg, lanes)
        elif self.vf_write[0] == reg:
            self.vf_write = (reg, self.vf_write[1] | lanes)

    def vf_read_lanes(self, reg):
        for r, l in self.vf_read:
            if r == reg:
                return l
        return 0


class Upper:
    """kind: nop, fmac (to fd), fmaca (to acc), max, mini, ftoi, itof, abs, clip."""

    def __init__(self, word):
        self.word = word
        self.dest = (word >> 21) & 0xF
        self.ft = (word >> 16) & 31
        self.fs = (word >> 11) & 31
        self.fd = (word >> 6) & 31
        op = word & 0x3F
        self.op = op
        self.kind = None
        self.fmac = None  # (operation, right, bc): operation add sub mul madd msub opmsub opmula; right vec bc q i
        self.scale = 0
        self.right = None
        self.bc = 0
        low11 = word & 0x7FF
        if low11 in (0x2FF, 0x33C):
            self.kind = 'nop'
            return
        if op <= 0x2F:
            if op <= 0x1B and not (0x10 <= op <= 0x17):
                kinds = {0: 'add', 1: 'sub', 2: 'madd', 3: 'msub', 6: 'mul'}
                self.kind = 'fmac'
                self.fmac = kinds[op >> 2]
                self.right = 'bc'
                self.bc = op & 3
            elif 0x10 <= op <= 0x13:
                self.kind, self.right, self.bc = 'max', 'bc', op & 3
            elif 0x14 <= op <= 0x17:
                self.kind, self.right, self.bc = 'mini', 'bc', op & 3
            elif op == 0x1D:
                self.kind, self.right = 'max', 'i'
            elif op == 0x1F:
                self.kind, self.right = 'mini', 'i'
            elif op == 0x2B:
                self.kind, self.right = 'max', 'vec'
            elif op == 0x2F:
                self.kind, self.right = 'mini', 'vec'
            else:
                table = {0x1C: ('mul', 'q'), 0x1E: ('mul', 'i'), 0x20: ('add', 'q'), 0x21: ('madd', 'q'),
                         0x22: ('add', 'i'), 0x23: ('madd', 'i'), 0x24: ('sub', 'q'), 0x25: ('msub', 'q'),
                         0x26: ('sub', 'i'), 0x27: ('msub', 'i'), 0x28: ('add', 'vec'), 0x29: ('madd', 'vec'),
                         0x2A: ('mul', 'vec'), 0x2C: ('sub', 'vec'), 0x2D: ('msub', 'vec'), 0x2E: ('opmsub', 'vec')}
                self.kind = 'fmac'
                self.fmac, self.right = table[op]
            return
        if op >= 0x3C:
            sp = (word & 3) | ((word >> 4) & 0x7C)
            self.special = sp
            if sp <= 0x0F or 0x18 <= sp <= 0x1B:
                kinds = {0: 'add', 1: 'sub', 2: 'madd', 3: 'msub', 6: 'mul'}
                self.kind, self.fmac, self.right, self.bc = 'fmaca', kinds[sp >> 2], 'bc', sp & 3
            elif 0x10 <= sp <= 0x13:
                self.kind, self.scale = 'itof', (0, 4, 12, 15)[sp & 3]
            elif 0x14 <= sp <= 0x17:
                self.kind, self.scale = 'ftoi', (0, 4, 12, 15)[sp & 3]
            elif sp == 0x1C:
                self.kind, self.fmac, self.right = 'fmaca', 'mul', 'q'
            elif sp == 0x1D:
                self.kind = 'abs'
            elif sp == 0x1E:
                self.kind, self.fmac, self.right = 'fmaca', 'mul', 'i'
            elif sp == 0x1F:
                self.kind = 'clip'
            elif sp in (0x2F, 0x30):
                self.kind = 'nop'
            else:
                table = {0x20: ('add', 'q'), 0x21: ('madd', 'q'), 0x22: ('add', 'i'), 0x23: ('madd', 'i'),
                         0x24: ('sub', 'q'), 0x25: ('msub', 'q'), 0x26: ('sub', 'i'), 0x27: ('msub', 'i'),
                         0x28: ('add', 'vec'), 0x29: ('madd', 'vec'), 0x2A: ('mul', 'vec'), 0x2C: ('sub', 'vec'),
                         0x2D: ('msub', 'vec'), 0x2E: ('opmula', 'vec')}
                if sp not in table:
                    raise ValueError(f'reserved upper {word:#010x}')
                self.kind = 'fmaca'
                self.fmac, self.right = table[sp]
            return
        raise ValueError(f'reserved upper {word:#010x}')

    def usage(self):
        u = Usage()
        u.pipeline = 'fmac'
        u.latency = FMAC_LATENCY
        word, op, dest, fs, ft, fd = self.word, self.op, self.dest, self.fs, self.ft, self.fd
        if self.kind == 'nop':
            return u
        if op <= 0x2F:
            u.add_vf_read(fs, dest)
            u.add_vf_write(fd, dest)
            if op <= 0x1B:
                u.add_vf_read(ft, lane_for_component(op & 3))
            elif op >= 0x28:
                u.add_vf_read(ft, 0xE if op == 0x2E else dest)
            if op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x21, 0x23, 0x25, 0x27, 0x29, 0x2D, 0x2E):
                u.acc_read = dest
            return u
        sp = self.special
        writes_acc = sp <= 0x0F or 0x18 <= sp <= 0x1C or sp == 0x1E or 0x20 <= sp <= 0x2A or 0x2C <= sp <= 0x2E
        if writes_acc:
            u.add_vf_read(fs, dest)
            if sp <= 0x1B:
                u.add_vf_read(ft, lane_for_component(sp & 3))
            elif 0x28 <= sp <= 0x2E:
                u.add_vf_read(ft, 0xE if sp == 0x2E else dest)
            u.acc_write = dest
            if 0x08 <= sp <= 0x0F or sp in (0x21, 0x23, 0x25, 0x27, 0x29, 0x2D):
                u.acc_read = dest
        elif 0x10 <= sp <= 0x17 or sp == 0x1D:
            u.add_vf_read(fs, dest)
            u.add_vf_write(ft, dest)
        elif sp == 0x1F:
            u.add_vf_read(fs, 0xE)
            u.add_vf_read(ft, 0x1)
            u.writes_clip = True
        return u

    def writes_flags(self):
        return self.kind in ('fmac', 'fmaca') and self.dest != 0


class Lower:
    def __init__(self, word):
        self.word = word
        self.op = (word >> 25) & 0x7F
        self.ft = (word >> 16) & 31
        self.fs = (word >> 11) & 31
        self.it = (word >> 16) & 0xF
        self.is_ = (word >> 11) & 0xF
        self.id = (word >> 6) & 0xF
        self.dest = (word >> 21) & 0xF
        self.imm11 = sext(word, 11)
        self.imm15 = (word & 0x7FF) | ((word >> 10) & 0x7800)
        self.imm24 = word & 0xFFFFFF
        self.name = None
        if word in (0, 0x8000033C):
            self.name = 'nop'
            return
        names = {0x00: 'lq', 0x01: 'sq', 0x04: 'ilw', 0x05: 'isw', 0x08: 'iaddiu', 0x09: 'isubiu', 0x12: 'fcand',
                 0x1A: 'fmand', 0x1C: 'fcget', 0x20: 'b', 0x24: 'jr', 0x28: 'ibeq', 0x29: 'ibne', 0x2C: 'ibltz', 0x2D: 'ibgtz',
                 0x2E: 'iblez', 0x2F: 'ibgez'}
        if self.op in names:
            self.name = names[self.op]
            return
        if self.op != 0x40:
            raise ValueError(f'unsupported lower {word:#010x}')
        funct = word & 0x3F
        f1 = {0x30: 'iadd', 0x31: 'isub', 0x34: 'iand', 0x35: 'ior'}
        if funct in f1:
            self.name = f1[funct]
            return
        if funct < 0x3C:
            raise ValueError(f'unsupported lower {word:#010x}')
        sp = (word & 3) | ((word >> 4) & 0x7C)
        f2 = {0x30: 'move', 0x31: 'mr32', 0x34: 'lqi', 0x35: 'sqi', 0x38: 'div', 0x39: 'sqrt', 0x3A: 'rsqrt', 0x3B: 'waitq',
              0x3C: 'mtir', 0x68: 'xtop', 0x6C: 'xgkick'}
        if sp not in f2:
            raise ValueError(f'unsupported lower {word:#010x}')
        self.name = f2[sp]
        self.fsf = (word >> 21) & 3
        self.ftf = (word >> 23) & 3

    def usage(self):
        u = Usage()
        n = self.name
        if n == 'nop':
            return u

        def rvi(r):
            if r:
                u.vi_read |= 1 << r

        def wvi(r):
            if r:
                u.vi_write |= 1 << r
        if n == 'lq':
            u.pipeline, u.latency = 'lsu', 4
            rvi(self.is_)
            u.add_vf_write(self.ft, self.dest)
        elif n == 'sq':
            u.pipeline, u.latency = 'lsu', 1
            rvi(self.it)
            u.add_vf_read(self.fs, self.dest)
        elif n == 'ilw':
            u.pipeline, u.latency = 'lsu', 4
            rvi(self.is_)
            wvi(self.it)
        elif n == 'isw':
            u.pipeline, u.latency = 'lsu', 1
            rvi(self.is_)
            rvi(self.it)
        elif n in ('iaddiu', 'isubiu'):
            u.pipeline, u.latency, u.delays_branch = 'ialu', 1, True
            rvi(self.is_)
            wvi(self.it)
        elif n == 'fcand':
            u.pipeline, u.latency, u.reads_clip = 'ialu', 1, True
            wvi(1)
        elif n == 'fmand':
            u.pipeline, u.latency = 'ialu', 1
            rvi(self.is_)
            wvi(self.it)
        elif n == 'b':
            u.pipeline = 'branch'
        elif n == 'jr':
            u.pipeline = 'branch'
            rvi(self.is_)
        elif n in ('ibeq', 'ibne'):
            u.pipeline = 'branch'
            rvi(self.is_)
            rvi(self.it)
        elif n in ('ibltz', 'ibgtz', 'iblez', 'ibgez'):
            u.pipeline = 'branch'
            rvi(self.is_)
        elif n in ('iadd', 'isub', 'iand', 'ior'):
            u.pipeline, u.latency, u.delays_branch = 'ialu', 1, True
            rvi(self.is_)
            rvi(self.it)
            wvi(self.id)
        elif n == 'move':
            u.pipeline, u.latency = 'fmac', 4
            u.add_vf_read(self.fs, self.dest)
            u.add_vf_write(self.ft, self.dest)
        elif n == 'lqi':
            u.pipeline, u.latency, u.vi_latency, u.delays_branch = 'lsu', 4, 1, True
            rvi(self.is_)
            wvi(self.is_)
            u.add_vf_write(self.ft, self.dest)
        elif n == 'sqi':
            u.pipeline, u.latency, u.delays_branch = 'lsu', 1, True
            rvi(self.it)
            wvi(self.it)
            u.add_vf_read(self.fs, self.dest)
        elif n == 'div':
            u.pipeline, u.latency = 'fdiv', 7
            u.add_vf_read(self.fs, lane_for_component(self.fsf))
            u.add_vf_read(self.ft, lane_for_component(self.ftf))
        elif n == 'sqrt':
            u.pipeline, u.latency = 'fdiv', 7
            u.add_vf_read(self.ft, lane_for_component(self.ftf))
        elif n == 'rsqrt':
            u.pipeline, u.latency = 'fdiv', 13
            u.add_vf_read(self.fs, lane_for_component(self.fsf))
            u.add_vf_read(self.ft, lane_for_component(self.ftf))
        elif n == 'waitq':
            u.pipeline, u.wait_q = 'fdiv', True
        elif n == 'mr32':
            u.pipeline, u.latency = 'fmac', 4
            u.add_vf_read(self.fs, 0xF)
            u.add_vf_write(self.ft, self.dest)
        elif n == 'fcget':
            u.pipeline, u.latency, u.reads_clip = 'ialu', 1, True
            wvi(self.it)
        elif n == 'mtir':
            u.pipeline, u.latency, u.delays_branch = 'ialu', 1, True
            u.add_vf_read(self.fs, lane_for_component(self.fsf))
            wvi(self.it)
        elif n == 'xtop':
            u.pipeline, u.latency = 'ialu', 1
            wvi(self.it)
        elif n == 'xgkick':
            u.pipeline, u.latency = 'xgkick', 2
            rvi(self.is_)
        return u

    def is_branch(self):
        return self.name in ('b', 'jr', 'ibeq', 'ibne', 'ibltz', 'ibgtz', 'iblez', 'ibgez')

    def is_store(self):
        return self.name in ('sq', 'isw', 'sqi')


class Pair:
    def __init__(self, pc, lower, upper):
        self.pc = pc
        self.lower_word, self.upper_word = lower, upper
        self.ibit = bool(upper & 0x80000000)
        self.ebit = bool(upper & 0x40000000)
        self.dbit = bool(upper & 0x10000000)
        self.tbit = bool(upper & 0x08000000)
        self.upper = Upper(upper)
        self.uu = self.upper.usage()
        if self.ibit:
            self.lower = Lower(0)
            self.loi = lower
        else:
            self.lower = Lower(lower)
            self.loi = None
        self.lu = self.lower.usage()
        self.shadow = 0
        self.suppressed_lower_vf = 0
        w = self.uu.vf_write
        if w and (self.lu.vf_read_lanes(w[0]) or (self.lu.vf_write and self.lu.vf_write[0] == w[0])):
            self.shadow = w[0]
            if self.lu.vf_write and self.lu.vf_write[0] == w[0]:
                self.suppressed_lower_vf = w[0]


def parse(data):
    pairs = []
    for i in range(len(data) // 8):
        lo, up = struct.unpack_from('<II', data, i * 8)
        try:
            pairs.append(Pair(i * 8, lo, up))
        except ValueError:
            pairs.append(None)
    return pairs


MAX_FLAG_ENTRIES = 8


class GenError(Exception):
    pass


class TState:
    __slots__ = ('vf', 'vi', 'acc', 'wb', 'fdiv', 'flags', 'mac', 'clip', 'last', 'kick', 'branch', 'ebit',
                 'backup', '_key')

    def __init__(self):
        self.vf = [0] * 128
        self.vi = [0] * 16
        self.acc = [0] * 4
        self.wb = 0
        self.fdiv = None
        self.flags = ()
        self.mac = None
        self.clip = None
        self.last = None
        self.kick = False
        self.branch = None
        self.ebit = False
        self.backup = 0
        self._key = None

    def copy(self):
        s = TState()
        s.vf = list(self.vf)
        s.vi = list(self.vi)
        s.acc = list(self.acc)
        s.wb, s.fdiv, s.flags, s.mac, s.clip, s.last = self.wb, self.fdiv, self.flags, self.mac, self.clip, self.last
        s.kick, s.branch, s.ebit, s.backup = self.kick, self.branch, self.ebit, self.backup
        return s

    def key(self):
        if self._key is None:
            self._key = (tuple(self.vf), tuple(self.vi), tuple(self.acc), self.wb, self.fdiv, self.flags, self.mac,
                         self.clip, self.last, self.kick, self.branch, self.ebit, self.backup)
        return self._key

    def horizon(self):
        values = self.vf + self.vi + self.acc + [self.wb] + [f[0] for f in self.flags]
        if self.fdiv is not None:
            values.append(self.fdiv)
        return max(values)


def lanes_of(mask):
    return [c for c in range(4) if mask & (8 >> c)]


class Node:
    def __init__(self, nid, pc, state, extra=None):
        self.id = nid
        self.pc = pc
        self.state = state
        self.extra = extra  # XGKICK nodes: None = the dynamic wait, else the wait it ended with
        self.stall = 0
        self.land_q = False
        self.mac_src = None
        self.clip_src = None
        self.succ = []  # (condition, node) ; condition None, 'taken', 'not', ('jr', target), ('wait', e)
        self.end = None  # END: (static tail cycles after the pair, kick maybe active, fdiv pending at the end)
        self.save_backup = 0
        self.path1_store = False
        self.branch_backup = 0
        self.preds = []


class Program:
    def __init__(self, pairs, name, track_mac, track_clip, unit='VU1'):
        self.pairs = pairs
        self.mask = 0x3FFF if unit == 'VU1' else 0xFFF
        self.data_mask = 0x3FF if unit == 'VU1' else 0xFF
        self.name = name
        self.track_mac = track_mac
        self.track_clip = track_clip
        self.nodes = []
        self.index = {}
        self.entries = {}

    def node(self, pc, state, extra=None):
        k = (pc, state.key(), extra)
        n = self.index.get(k)
        if n is None:
            n = Node(len(self.nodes), pc, state, extra)
            self.nodes.append(n)
            self.index[k] = n
            self.work.append(n)
        return n

    def pair(self, pc):
        p = self.pairs[pc >> 3] if (pc >> 3) < len(self.pairs) else None
        if p is None:
            raise GenError(f'{self.name}: no instruction at {pc:#x}')
        if p.dbit or p.tbit:
            raise GenError(f'{self.name}: D/T bit at {pc:#x}')
        return p

    def build(self, entries, targets):
        self.work = []
        self.targets = targets
        for pc in entries:
            self.entries[pc] = self.node(pc, TState())
        while self.work:
            n = self.work.pop()
            self.step(n)
            if len(self.nodes) > 200000:
                raise GenError('too many nodes')
        for n in self.nodes:
            for _, s in n.succ:
                s.preds.append(n)
        for n in self.nodes:
            if n.branch_backup:
                for p in n.preds:
                    p.save_backup = n.branch_backup

    def static_ready(self, p, s):
        ready = 0
        for u in (p.uu, p.lu):
            for reg, lanes in u.vf_read:
                for c in lanes_of(lanes):
                    ready = max(ready, s.vf[reg * 4 + c])
            for r in range(1, 16):
                if u.vi_read & (1 << r):
                    ready = max(ready, s.vi[r])
            if u.acc_read:
                for c in lanes_of(u.acc_read):
                    ready = max(ready, s.acc[c])
        if p.lu.pipeline == 'fdiv' and s.fdiv is not None:
            ready = max(ready, s.fdiv)
        return ready

    def step(self, n):
        p = self.pair(n.pc)
        s = n.state
        stall = self.static_ready(p, s)
        if p.lower.name == 'xgkick' and s.kick and n.extra is None:
            horizon = max(0, s.horizon() - stall)
            for e in range(horizon + 1):
                n.succ.append((('wait', e), self.node(n.pc, s, e)))
            return
        if n.extra is not None:
            stall += n.extra
        n.stall = stall
        t = stall
        s = s.copy()
        s._key = None
        if s.fdiv is not None and s.fdiv <= t:
            n.land_q = True
            s.fdiv = None
        flags = list(s.flags)
        while flags and flags[0][0] <= t:
            _, kind, pc = flags.pop(0)
            if kind == 'f':
                s.mac = pc
            else:
                s.clip = pc
        if p.lower.name == 'fmand':
            n.mac_src = s.mac
            if s.mac is not None and any(k == 'f' and pc == s.mac for _, k, pc in flags):
                raise GenError(f'{self.name}: MAC of {s.mac:#x} read at {p.pc:#x} while it is written again')
        if p.lower.name in ('fcand', 'fcget'):
            n.clip_src = s.clip
            if s.clip is not None and any(k == 'c' and pc == s.clip for _, k, pc in flags):
                raise GenError(f'{self.name}: clip of {s.clip:#x} read at {p.pc:#x} while it is written again')
        if p.upper.writes_flags():
            flags.append((t + FMAC_LATENCY, 'f', p.pc))
            s.last = p.pc
        if p.upper.kind == 'clip':
            flags.append((t + FMAC_LATENCY, 'c', p.pc))
        if len(flags) > MAX_FLAG_ENTRIES:
            raise GenError(f'{self.name}: flag queue overflow at {p.pc:#x}')
        seen = set()
        for _, kind, pc in flags:
            if (kind, pc) in seen:
                raise GenError(f'{self.name}: two {kind} writes of {pc:#x} in flight')
            seen.add((kind, pc))
        if not self.track_mac:
            s.mac = None
        if not self.track_clip:
            s.clip = None
        lo = p.lower
        if lo.name in ('div', 'sqrt', 'rsqrt'):
            s.fdiv = t + (13 if lo.name == 'rsqrt' else 7)
        if lo.is_store():
            n.path1_store = s.kick
            s.wb = max(s.wb, t + 1)
        if lo.name == 'xgkick':
            s.kick = True

        def ready_lanes(arr, base, lanes, value):
            for c in lanes_of(lanes):
                arr[base + c] = value
        w = p.lu.vf_write
        if w and p.suppressed_lower_vf != w[0]:
            lat = p.lu.vf_latency or p.lu.latency
            ready_lanes(s.vf, w[0] * 4, w[1], t + lat)
            s.wb = max(s.wb, t + lat)
        w = p.uu.vf_write
        if w:
            lat = p.uu.vf_latency or p.uu.latency
            ready_lanes(s.vf, w[0] * 4, w[1], t + lat)
            s.wb = max(s.wb, t + lat)
        vw = p.lu.vi_write & 0xFFFE
        if vw:
            lat = p.lu.vi_latency or p.lu.latency
            for r in range(1, 16):
                if vw & (1 << r):
                    s.vi[r] = t + lat
            s.wb = max(s.wb, t + lat)
        if p.uu.acc_write:
            ready_lanes(s.acc, 0, p.uu.acc_write, t + ACC_FORWARD)
            s.wb = max(s.wb, t + ACC_FORWARD)
        branch_reads = 0
        if lo.is_branch():
            if lo.name in ('ibeq', 'ibne'):
                branch_reads = (1 << lo.is_) | (1 << lo.it)
            elif lo.name != 'b' and lo.name != 'jr':
                branch_reads = 1 << lo.is_
        if s.backup and branch_reads & (1 << s.backup):
            n.branch_backup = s.backup
        backup = 0
        if vw and p.lu.delays_branch:
            backup = min(r for r in range(1, 16) if vw & (1 << r))
        s.backup = backup
        shift = t + 1

        def dec(v):
            return max(0, v - shift)
        s.vf = [dec(v) for v in s.vf]
        s.vi = [dec(v) for v in s.vi]
        s.acc = [dec(v) for v in s.acc]
        s.wb = dec(s.wb)
        if s.fdiv is not None:
            s.fdiv = dec(s.fdiv)
        s.flags = tuple((dec(r), k, pc) for r, k, pc in flags)
        ended = s.ebit
        if p.ebit and not ended:
            s.ebit = True
        outcomes = []
        if lo.is_branch():
            if s.branch is not None:
                raise GenError(f'{self.name}: branch in a delay slot at {p.pc:#x}')
            if lo.name == 'jr':
                outcomes.append((None, ('jr', 1)))
            elif lo.name == 'b':
                outcomes.append((None, ((p.pc + 8 + lo.imm11 * 8) & self.mask, 1)))
            else:
                outcomes.append(('taken', ((p.pc + 8 + lo.imm11 * 8) & self.mask, 1)))
                outcomes.append(('not', None))
        else:
            outcomes.append((None, s.branch))
        for cond, branch in outcomes:
            ns = s.copy()
            ns._key = None
            next_pcs = [(cond, (p.pc + 8) & self.mask)]
            if branch is not None:
                target, delay = branch
                if delay == 0:
                    if target == 'jr':
                        next_pcs = [(('jr', tgt), tgt) for tgt in self.targets]
                    else:
                        next_pcs = [(cond, target)]
                    ns.branch = None
                else:
                    ns.branch = (target, delay - 1)
            else:
                ns.branch = None
            if ended:
                tail = max([0, ns.wb] + [r for r, _, _ in ns.flags] + ([ns.fdiv] if ns.fdiv is not None else []))
                n.end = (tail, ns.kick, ns.fdiv is not None, ns.last, ns.mac)
                return
            for c, pc in next_pcs:
                n.succ.append((c, self.node(pc, ns)))


CPP_OPS = {'add': 'Add', 'sub': 'Sub', 'mul': 'Mul', 'madd': 'Madd', 'msub': 'Msub', 'opmsub': 'CrossMsub',
           'opmula': 'CrossMul'}
class Emitter:
    def __init__(self, prog, fn):
        self.prog = prog
        self.fn = fn
        self.lines = []
        self.need_mac = set()
        self.need_clip = set()
        for n in prog.nodes:
            if n.mac_src is not None:
                self.need_mac.add(n.mac_src)
            if n.clip_src is not None:
                self.need_clip.add(n.clip_src)
            if n.end is not None and n.end[3] is not None:
                self.need_mac.add(n.end[3])

    def vf(self, r):
        return 'kVf0' if r == 0 else f'vf{r}'

    def vi(self, r):
        return '0' if r == 0 else f'vi{r}'

    def out(self, text, indent=2):
        self.lines.append('    ' * (indent + 1) + text)

    def upper(self, p):
        u = p.upper
        k = u.kind
        if k == 'nop':
            return None
        d = u.dest
        if d == 0 and k != 'clip':
            return None
        if k in ('fmac', 'fmaca'):
            dst = 'acc' if k == 'fmaca' else (f'vf{u.fd}' if u.fd else 'discard')
            if u.right == 'vec':
                right, rkind = self.vf(u.ft), 'Vec'
            elif u.right == 'bc':
                right, rkind = self.vf(u.ft), f'Bc{u.bc}'
            elif u.right == 'q':
                right, rkind = 'q', 'Scalar'
            else:
                right, rkind = 'iReg', 'Scalar'
            live = p.pc in self.need_mac and d != 0
            flags = f'mac_{p.pc:04x}, cur_{p.pc:04x}' if live else 'unusedMac, unusedCur'
            expr = (f'fmac<Op::{CPP_OPS[u.fmac]}, R::{rkind}, {d}, kSticky, {"true" if live else "false"}>'
                    f'(f, {dst}, {self.vf(u.fs)}, {right}, acc, {flags})')
            return dst, expr
        if k in ('max', 'mini'):
            dst = f'vf{u.fd}' if u.fd else 'discard'
            if u.right == 'vec':
                right, rk = self.vf(u.ft), 'Vec'
            elif u.right == 'bc':
                right, rk = self.vf(u.ft), f'Bc{u.bc}'
            else:
                right, rk = 'iReg', 'Scalar'
            fn = 'maxv' if k == 'max' else 'miniv'
            return dst, f'{fn}<R::{rk}, {d}>({dst}, {self.vf(u.fs)}, {right})'
        if k == 'itof':
            dst = f'vf{u.ft}' if u.ft else 'discard'
            return dst, f'itof<{u.scale}, {d}>({dst}, {self.vf(u.fs)})'
        if k == 'ftoi':
            dst = f'vf{u.ft}' if u.ft else 'discard'
            return dst, f'ftoi<{u.scale}, {d}>({dst}, {self.vf(u.fs)})'
        if k == 'clip':
            return 'clip', f'clipFlags({self.vf(u.fs)}, {self.vf(u.ft)})'
        raise GenError(f'upper {k} at {p.pc:#x} not written')

    def lower(self, n, p):
        lo = p.lower
        nm = lo.name
        vf, vi = self.vf, self.vi
        if nm == 'nop':
            return []
        if nm == 'lq':
            return [f'{vf(lo.ft)} = put<{lo.dest}>({vf(lo.ft)}, m.lq({vi(lo.is_)} + {lo.imm11}));'] if lo.ft else []
        if nm == 'sq':
            return [f'm.sq<{lo.dest}>({vi(lo.it)} + {lo.imm11}, {vf(lo.fs)});']
        if nm == 'ilw':
            comp = 0 if lo.dest & 8 else 1 if lo.dest & 4 else 2 if lo.dest & 2 else 3
            return [f'{vi(lo.it)} = m.ilw({vi(lo.is_)} + {lo.imm11}, {comp});'] if lo.it else []
        if nm == 'isw':
            return [f'm.isw<{lo.dest}>({vi(lo.is_)} + {lo.imm11}, {vi(lo.it)});']
        if nm == 'iaddiu':
            if not lo.is_:
                return [f'{vi(lo.it)} = {sext(lo.imm15, 16)};'] if lo.it else []
            return [f'{vi(lo.it)} = i16({vi(lo.is_)} + {lo.imm15});'] if lo.it else []
        if nm == 'isubiu':
            return [f'{vi(lo.it)} = i16({vi(lo.is_)} - {lo.imm15});'] if lo.it else []
        if nm == 'iadd' and (lo.is_ == 0 or lo.it == 0) and lo.id:
            src = lo.it if lo.is_ == 0 else lo.is_
            return [f'{vi(lo.id)} = {vi(src)};'] if src else [f'{vi(lo.id)} = 0;']
        if nm in ('iadd', 'isub'):
            sign = '+' if nm == 'iadd' else '-'
            return [f'{vi(lo.id)} = i16({vi(lo.is_)} {sign} {vi(lo.it)});'] if lo.id else []
        if nm in ('iand', 'ior'):
            sign = '&' if nm == 'iand' else '|'
            return [f'{vi(lo.id)} = {vi(lo.is_)} {sign} {vi(lo.it)};'] if lo.id else []
        if nm == 'move':
            return [f'{vf(lo.ft)} = put<{lo.dest}>({vf(lo.ft)}, {vf(lo.fs)});'] if lo.ft else []
        if nm == 'mtir':
            return [f'{vi(lo.it)} = i16(static_cast<int32_t>(laneBits({vf(lo.fs)}, {lo.fsf}) & 0xFFFFu));'] if lo.it else []
        if nm == 'lqi':
            out = [f'{vf(lo.ft)} = put<{lo.dest}>({vf(lo.ft)}, m.lq({vi(lo.is_)}));'] if lo.ft else []
            if lo.is_:
                out.append(f'{vi(lo.is_)} = i16({vi(lo.is_)} + 1);')
            return out
        if nm == 'sqi':
            out = [f'm.sq<{lo.dest}>({vi(lo.it)}, {vf(lo.fs)});']
            if lo.it:
                out.append(f'{vi(lo.it)} = i16({vi(lo.it)} + 1);')
            return out
        if nm == 'div':
            return [f'qPending = divide(laneValue({vf(lo.fs)}, {lo.fsf}), laneValue({vf(lo.ft)}, {lo.ftf}), diPending);']
        if nm == 'fcand':
            src = 'clip0' if n.clip_src is None else f'clip_{n.clip_src:04x}'
            return [f'vi1 = ({src} & {lo.imm24:#x}u) != 0u ? 1 : 0;']
        if nm == 'fcget':
            src = 'clip0' if n.clip_src is None else f'clip_{n.clip_src:04x}'
            return [f'{vi(lo.it)} = static_cast<int32_t>({src} & 0xFFFu);'] if lo.it else []
        if nm == 'mr32':
            return [f'{vf(lo.ft)} = put<{lo.dest}>({vf(lo.ft)}, rotate({vf(lo.fs)}));'] if lo.ft else []
        if nm == 'sqrt':
            return [f'qPending = squareRoot(laneValue({vf(lo.ft)}, {lo.ftf}), diPending);']
        if nm == 'rsqrt':
            return [f'qPending = reciprocalRoot(laneValue({vf(lo.fs)}, {lo.fsf}), laneValue({vf(lo.ft)}, {lo.ftf}), diPending);']
        if nm == 'waitq':
            return []
        if nm == 'fmand':
            src = 'mac0' if n.mac_src is None else f'mac_{n.mac_src:04x}'
            return [f'{vi(lo.it)} = static_cast<int32_t>({src} & static_cast<uint32_t>(static_cast<uint16_t>({vi(lo.is_)})));'] if lo.it else []
        if nm == 'xtop':
            return [f'{vi(lo.it)} = static_cast<int32_t>(top & 0x3FFu);'] if lo.it else []
        if nm == 'xgkick':
            return [f'path.begin({vi(lo.is_)}, t);']
        if nm == 'jr':
            return [f'jrTarget = (static_cast<uint32_t>(static_cast<uint16_t>({vi(lo.is_)})) * 8u) & {self.prog.mask:#x}u;']
        if lo.is_branch():
            return []
        raise GenError(f'lower {nm} at {p.pc:#x} not written')

    def branch_cond(self, n, p):
        lo = p.lower

        def rd(r):
            if r == 0:
                return '0'
            if n.branch_backup == r:
                return 'viBack'
            return f'vi{r}'
        a, b = f'i16({rd(lo.is_)})', f'i16({rd(lo.it)})'
        return {'ibeq': f'{a} == {b}', 'ibne': f'{a} != {b}', 'ibltz': f'{a} < 0', 'ibgtz': f'{a} > 0',
                'iblez': f'{a} <= 0', 'ibgez': f'{a} >= 0'}[lo.name]

    def label(self, n):
        return f'n{n.id}'

    def emit_node(self, n):
        p = self.prog.pair(n.pc)
        self.out(f'{self.label(n)}:', 1)
        if n.extra is None and p.lower.name == 'xgkick' and n.succ and n.succ[0][0] and n.succ[0][0][0] == 'wait':
            top = len(n.succ) - 1
            stall = self.prog.static_ready(p, n.state)
            self.out(f't += {stall + 1};')
            self.out('if (path.pending)')
            self.out('    kickDone = path.finish(data, out, user);')
            self.out('wait = kickDone > t ? kickDone - t : 0;')
            self.out('t += wait;')
            self.out(f'switch (wait < {top} ? wait : {top})')
            self.out('{')
            for (_, e), s in n.succ:
                self.out(f'case {e}: goto {self.label(s)};' if e < top else f'default: goto {self.label(s)};')
            self.out('}')
            return
        if n.extra is None:
            self.out(f't += {n.stall + 1};')
        if n.land_q:
            self.out('q = qPending;')
            self.out('landDi(f, diPending);')
        lower = self.lower(n, p)
        if n.save_backup:
            self.out(f'viBack = vi{n.save_backup};')
        up = self.upper(p)
        if up is not None:
            dst, expr = up
            if dst == 'clip':
                self.out(f'clipWork = ((clipWork << 6) | {expr}) & 0xFFFFFFu;')
                if p.pc in self.need_clip:
                    self.out(f'clip_{p.pc:04x} = clipWork;')
            elif p.shadow:
                self.out('{')
                self.out(f'    const __m128 upper = {expr};')
            else:
                self.out(f'{dst} = {expr};')
        if p.loi is not None:
            self.out(f'iReg = normalizeOperand(std::bit_cast<float>({p.loi:#010x}u));')
        inner = '    ' if up is not None and p.shadow and up[0] != 'clip' else ''
        if n.path1_store:
            self.out(inner + 'path.advance(data, t);')
        for line in lower:
            self.out(inner + line)
        if inner:
            self.out(f'    {up[0]} = upper;')
            self.out('}')
        if n.end is not None:
            tail, kick, fdiv, last, macsrc = n.end
            if fdiv:
                self.out('q = qPending;')
                self.out('landDi(f, diPending);')
            self.out(f'endCycle = t + 1 + {tail};')
            if kick:
                self.out('if (path.pending)')
                self.out('{')
                self.out('    kickDone = path.finish(data, out, user);')
                self.out('    if (kickDone > endCycle)')
                self.out('        endCycle = kickDone;')
                self.out('}')
            if last is not None:
                self.out(f'lastMac = mac_{last:04x};')
                self.out(f'lastCur = cur_{last:04x};')
                self.out('hasFmac = true;')
            self.out(f'endPc = {(n.pc + 8) & self.prog.mask:#x}u;')
            self.out('goto done;')
            return
        if len(n.succ) == 1:
            self.out(f'goto {self.label(n.succ[0][1])};')
            return
        conds = [c for c, _ in n.succ]
        if conds[0] == 'taken':
            self.out(f'if ({self.branch_cond(n, p)})')
            self.out(f'    goto {self.label(n.succ[0][1])};')
            self.out(f'goto {self.label(n.succ[1][1])};')
            return
        if conds[0][0] == 'jr':
            self.out('switch (jrTarget)')
            self.out('{')
            for (_, tgt), x in n.succ:
                self.out(f'case {tgt:#x}u: goto {self.label(x)};')
            self.out('default: return kUnknownTarget;')
            self.out('}')
            return
        raise GenError(f'successors {conds} at {n.pc:#x}')

    def emit(self):
        prog = self.prog
        name = self.fn
        self.out('template <bool kSticky>', 0)
        self.out(f'uint64_t {name}(uint32_t entry, VU1State &state, uint8_t *data, uint32_t top, Vu1Kick out, void *user)', 0)
        self.out('{', 0)
        self.out(f'const Memory<{self.prog.data_mask:#x}u> m{{data}};', 1)
        self.out('Fmac f;', 1)
        self.out('Path1 path;', 1)
        self.out('const __m128 kVf0 = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);', 1)
        self.out('__m128 discard = _mm_setzero_ps();', 1)
        for r in range(1, 32):
            self.out(f'__m128 vf{r} = load(state.vf[{r}]);', 1)
        self.out('__m128 acc = load(state.acc);', 1)
        for r in range(1, 16):
            self.out(f'int32_t vi{r} = state.vi[{r}];', 1)
        self.out('float q = state.q, iReg = state.i, qPending = 0.0f;', 1)
        self.out('uint32_t diPending = 0u, clipWork = state.clip & 0xFFFFFFu, clip0 = clipWork, mac0 = state.mac;', 1)
        self.out('uint32_t unusedMac = 0u, unusedCur = 0u, lastMac = 0u, lastCur = 0u, endPc = 0u;', 1)
        self.out('[[maybe_unused]] int32_t viBack = 0;', 1)
        self.out('[[maybe_unused]] uint32_t jrTarget = 0u;', 1)
        self.out('bool hasFmac = false;', 1)
        self.out('int64_t t = -1;', 1)
        self.out('[[maybe_unused]] int64_t kickDone = 0, wait = 0;', 1)
        self.out('int64_t endCycle = 0;', 1)
        for pc in sorted(self.need_mac):
            self.out(f'uint32_t mac_{pc:04x} = 0u, cur_{pc:04x} = 0u;', 1)
        for pc in sorted(self.need_clip):
            self.out(f'uint32_t clip_{pc:04x} = 0u;', 1)
        self.out('switch (entry)', 1)
        self.out('{', 1)
        for pc, n in sorted(prog.entries.items()):
            self.out(f'case {pc:#x}u: goto {self.label(n)};', 1)
        self.out('default: return 0;', 1)
        self.out('}', 1)
        order = []
        seen = set()
        heads = [n for _, n in sorted(prog.entries.items())]
        while heads:
            x = heads.pop(0)
            while x is not None and x.id not in seen:
                seen.add(x.id)
                order.append(x)
                fall = None
                if len(x.succ) == 1:
                    fall = x.succ[0][1]
                elif x.succ and x.succ[0][0] == 'taken':
                    fall = x.succ[1][1]
                    heads.insert(0, x.succ[0][1])
                else:
                    heads[0:0] = [s for _, s in x.succ]
                x = fall
        for i, n in enumerate(order):
            self.emit_node(n)
            if i + 1 < len(order) and self.lines[-1].strip() == f'goto {self.label(order[i + 1])};':
                self.lines.pop()
        import re
        used = set(re.findall(r'goto (n\d+);', '\n'.join(self.lines)))
        self.lines = [l for l in self.lines if not (re.fullmatch(r'        n\d+:', l) and l.strip()[:-1] not in used)]
        merged = []
        pending = 0
        indent = ''
        for line in self.lines:
            m = re.fullmatch(r'(\s*)t \+= (-?\d+);', line)
            if m:
                pending += int(m.group(2))
                indent = m.group(1)
                continue
            stripped = line.strip()
            if pending and (re.search(r'\bt\b', stripped) or stripped.endswith(':') or stripped.startswith(('goto', 'if', 'switch', 'case', 'default', '}', '{'))):
                merged.append(f'{indent}t += {pending};')
                pending = 0
            merged.append(line)
        self.lines = merged
        self.out('done:', 0)
        for r in range(1, 32):
            self.out(f'store(state.vf[{r}], vf{r});', 1)
        self.out('store(state.acc, acc);', 1)
        for r in range(1, 16):
            self.out(f'state.vi[{r}] = vi{r};', 1)
        self.out('state.q = q;', 1)
        self.out('state.i = iReg;', 1)
        self.out('state.clip = clipWork;', 1)
        self.out('if (hasFmac)', 1)
        self.out('state.mac = lastMac;', 2)
        self.out('state.status = f.status(state.status, hasFmac, lastCur);', 1)
        self.out('state.pc = endPc;', 1)
        self.out('return static_cast<uint64_t>(endCycle);', 1)
        self.out('}', 0)
        return '\n'.join(self.lines)


PROGRAMS = {
    'A': dict(hash=0x95e81f9a833c1b45, pairs=1177, function='programA', file='sc3_vu1_a.cpp',
              entries=[0x10, 0xd8, 0x2b0, 0x450, 0x768, 0xa48, 0xc18, 0xda8, 0xf68, 0x1268, 0x1558, 0x1730, 0x18c0, 0x1ab8,
                       0x1c80, 0x1d28, 0x1f00, 0x2098, 0x2140, 0x2330],
              targets=[0xd8, 0x2b0, 0x450, 0x768, 0xa48, 0xc18, 0xda8, 0xf68, 0x1268, 0x1558, 0x1730, 0x18c0, 0x1ab8, 0x1d28,
                       0x1f00, 0x2140, 0x2330]),
    'B': dict(hash=0xd2fbf7af837a8df4, pairs=1352, function='programB', file='sc3_vu1_b.cpp',
              entries=[0x10, 0xd8, 0x350, 0x5b0, 0x910, 0xb78, 0xdd0, 0x10f0, 0x1388, 0x15e8, 0x1990, 0x1a40, 0x1ca8, 0x1ef0,
                       0x21e0, 0x2290, 0x24f8, 0x2740],
              targets=[0xd8, 0x350, 0x5b0, 0x910, 0xb78, 0xdd0, 0x10f0, 0x1388, 0x15e8, 0x1a40, 0x1ca8, 0x1ef0, 0x2290, 0x24f8,
                       0x2740]),
    'VU0': dict(hash=0x5f8cbf2471656edf, pairs=172, function='programVu0', file='sc3_vu0_program.cpp', unit='VU0',
                entries=[0x0, 0x18, 0x38, 0x60, 0xf0, 0x360, 0x3c8, 0x3e8, 0x438], targets=[]),
}


def code_hash(data):
    h = 0xCBF29CE484222325
    for byte in data:
        h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def code_from_aot(directory, spec):
    pairs = {}
    unit = spec.get('unit', 'VU1')
    for path in glob.glob(os.path.join(directory, f'{unit.lower()}_aot_*_{spec["hash"]:016x}*.cpp')):
        text = open(path).read()
        for m in re.finditer(r'kPair(\d+) = Vu::decodePair\(Vu::Unit::' + unit + r', 0x([0-9a-f]+)u, 0x([0-9a-f]+)u\)', text):
            pairs[int(m[1])] = (int(m[2], 16), int(m[3], 16))
    if len(pairs) != spec['pairs'] or sorted(pairs) != list(range(spec['pairs'])):
        raise GenError(f'{directory}: the compiled microcode {spec["hash"]:016x} is not all there ({len(pairs)} pairs)')
    return b''.join(struct.pack('<II', *pairs[i]) for i in range(spec['pairs']))


def generate(code, spec):
    if code_hash(code[:spec['pairs'] * 8]) != spec['hash']:
        raise GenError(f'the code image does not hash to {spec["hash"]:016x}')
    pairs = parse(code[:spec['pairs'] * 8])
    has_fmand = any(p is not None and p.lower.name == 'fmand' for p in pairs)
    has_fcand = any(p is not None and p.lower.name in ('fcand', 'fcget') for p in pairs)
    prog = Program(pairs, spec['function'], has_fmand, has_fcand, spec.get('unit', 'VU1'))
    prog.build(spec['entries'], spec['targets'])
    name = spec['function']
    lines = ['#include "sc3_vu1_ops.h"', '', 'namespace sc3::vu1', '{', Emitter(prog, name).emit(), '']
    sig = '(uint32_t entry, VU1State &state, uint8_t *data, uint32_t top, Vu1Kick out, void *user);'
    lines.append(f'    template uint64_t {name}<true>{sig}')
    lines.append(f'    template uint64_t {name}<false>{sig}')
    lines.append('}')
    return '\n'.join(lines) + '\n', len(prog.nodes)


def write(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return
    with open(path, 'w') as out:
        out.write(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--code', metavar='IMAGE')
    ap.add_argument('source')
    ap.add_argument('out')
    a = ap.parse_args()
    try:
        if a.code:
            spec = PROGRAMS[a.source]
            text, nodes = generate(open(a.code, 'rb').read(), spec)
            write(a.out, text)
            return
        os.makedirs(a.out, exist_ok=True)
        for key, spec in PROGRAMS.items():
            text, nodes = generate(code_from_aot(a.source, spec), spec)
            write(os.path.join(a.out, spec['file']), text)
            print(f'program {key}: {len(spec["entries"])} start pcs, {nodes} timed pairs -> {spec["file"]}')
    except GenError as error:
        sys.exit(f'vu1_port_gen: {error}')


if __name__ == '__main__':
    main()
