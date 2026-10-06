#!/usr/bin/env python3
"""Salvage a damaged WiCAN OBD logger .db into a fresh, valid one (stdlib only).

    python3 tools/logger_repair.py damaged.db repaired.db

Why files come back "malformed": the logger used to run with synchronous=OFF, so
rows were appended to the file without the card's directory entry (the file size)
ever being updated until the file was closed. A reboot or power cut then left a file
whose b-tree (and sometimes the page count in its header) refers to pages that are
not there. Everything that did reach the card is still in intact leaf pages; this
walks the param_data b-tree itself, skips pointers that lead outside the file, and
rewrites the rows into a new database. param_info and settings_log are copied
through SQLite when it can still read them. The damaged file is never modified.

Rows that were only in the pages that never reached the card are gone; the report
says how many pages the tree wanted but the file does not have.
"""
import sqlite3
import struct
import sys

PAGE_HDR_LEAF, PAGE_HDR_INTERIOR = 13, 5


def varint(b, o):
    v = 0
    for k in range(9):
        c = b[o + k]
        if k == 8:
            return (v << 8) | c, 9
        v = (v << 7) | (c & 0x7F)
        if not c & 0x80:
            return v, k + 1


class Db:
    def __init__(self, data):
        self.d = data
        self.ps = struct.unpack(">H", data[16:18])[0] or 65536
        self.n = len(data) // self.ps

    def page(self, i):
        return self.d[(i - 1) * self.ps:i * self.ps]

    def hdr(self, i):
        b = self.page(i)
        o = 100 if i == 1 else 0
        t = b[o]
        nc = struct.unpack(">H", b[o + 3:o + 5])[0]
        rp = struct.unpack(">I", b[o + 8:o + 12])[0] if t in (2, 5) else None
        return b, o, t, nc, rp

    def leaves(self, root, missing):
        """Leaf page numbers under root in key order; pointers outside the file go to missing."""
        out = []
        stack = [root]
        while stack:
            i = stack.pop()
            if i < 1 or i > self.n:
                missing.append(i)
                continue
            b, o, t, nc, rp = self.hdr(i)
            if t == PAGE_HDR_INTERIOR:
                kids = [struct.unpack(">I", b[struct.unpack(">H", b[o + 12 + 2 * k:o + 14 + 2 * k])[0]:][:4])[0]
                        for k in range(nc)] + [rp]
                stack.extend(reversed(kids))
            elif t == PAGE_HDR_LEAF:
                out.append(i)
        return out

    def rows(self, i):
        b, o, t, nc, _ = self.hdr(i)
        for k in range(nc):
            cp = struct.unpack(">H", b[o + 8 + 2 * k:o + 10 + 2 * k])[0]
            _, l1 = varint(b, cp)
            _, l2 = varint(b, cp + l1)
            q = cp + l1 + l2
            hl, l3 = varint(b, q)
            hp, hend, sts = q + l3, q + hl, []
            while hp < hend:
                v, l = varint(b, hp)
                sts.append(v)
                hp += l
            dp, vals = hend, []
            for st in sts:
                if st == 0:
                    vals.append(None)
                elif 1 <= st <= 6:
                    nb = {1: 1, 2: 2, 3: 3, 4: 4, 5: 6, 6: 8}[st]
                    vals.append(int.from_bytes(b[dp:dp + nb], "big", signed=True))
                    dp += nb
                elif st == 7:
                    vals.append(struct.unpack(">d", b[dp:dp + 8])[0])
                    dp += 8
                elif st in (8, 9):
                    vals.append(st - 8)
                else:
                    return  # text/blob: not a param_data row, stop rather than guess
            if len(vals) == 3 and all(v is not None for v in vals):
                yield vals


def table_root(path, name):
    try:
        con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        r = con.execute("SELECT rootpage FROM sqlite_master WHERE name=?", (name,)).fetchone()
        con.close()
        return r[0] if r else None
    except sqlite3.Error:
        return None


def copy_table(src, dst, name, ddl):
    try:
        s = sqlite3.connect(f"file:{src}?mode=ro", uri=True)
        rows = s.execute(f"SELECT * FROM {name}").fetchall()
        cols = [c[1] for c in s.execute(f"PRAGMA table_info({name})")]
        s.close()
    except sqlite3.Error as e:
        print(f"  {name}: not readable ({e}); left empty")
        return 0
    dst.execute(ddl)
    dst.executemany(f"INSERT INTO {name} ({','.join(cols)}) VALUES ({','.join('?' * len(cols))})", rows)
    return len(rows)


def main(src, out):
    data = open(src, "rb").read()
    db = Db(data)
    print(f"{src}: {len(data)} bytes, {db.n} pages, header says {struct.unpack('>I', data[28:32])[0]}")
    root = table_root(src, "param_data") or 2
    missing = []
    leaves = db.leaves(root, missing)
    dst = sqlite3.connect(out)
    dst.execute("CREATE TABLE param_data (timestamp INTEGER, param_id INTEGER, value REAL)")
    n = 0
    for i in leaves:
        batch = [(int(a), int(b), float(c)) for a, b, c in db.rows(i)]
        dst.executemany("INSERT INTO param_data VALUES (?,?,?)", batch)
        n += len(batch)
    print(f"  param_data: {n} rows from {len(leaves)} leaf pages; "
          f"{len(missing)} page(s) the tree points at are not in the file {missing[:6]}")
    p = copy_table(src, dst, "param_info",
                   "CREATE TABLE param_info (Id INTEGER PRIMARY KEY AUTOINCREMENT, Name VARCHAR(50) UNIQUE, Type VARCHAR(50), Data JSON)")
    s = copy_table(src, dst, "settings_log",
                   "CREATE TABLE settings_log (timestamp INTEGER, uptime_ms INTEGER, source TEXT, key TEXT, old_value TEXT, new_value TEXT, event TEXT)")
    print(f"  param_info: {p} rows, settings_log: {s} rows")
    dst.commit()
    ok = dst.execute("PRAGMA integrity_check").fetchone()[0]
    lo, hi = dst.execute("SELECT MIN(timestamp), MAX(timestamp) FROM param_data").fetchone()
    print(f"  integrity_check: {ok}; rows span {lo}..{hi} (epoch ms)")
    dst.close()


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
