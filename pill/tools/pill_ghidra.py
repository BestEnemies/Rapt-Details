"""PyGhidra analysis of the RAPT Pill 2.0.0 app image.

Loads the ESP32 app image, maps segments, auto-analyzes, then decompiles the
Direct-command parser (around 0x400d1dcc), its callers (the GATT write handler
= the command framing), and the functions that log the GATT sub-characteristic
messages. Writes C to cmd_decomp.txt.
"""
import struct, array
import pyghidra
pyghidra.start()

import jpype
from java.io import ByteArrayInputStream
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.app.plugin.core.analysis import AutoAnalysisManager
from ghidra.app.decompiler import DecompInterface

BIN = r"C:\Users\rober\Documents\Claude\Projects\RAPT Pill\app_2.0.0.bin"
PROJ_LOC = r"C:\Users\rober\AppData\Local\Temp\claude\C--Users-rober-Documents-Claude-Projects-BarOS\31fe9203-16f9-40e5-a9f5-be12ba058805\scratchpad\gproj"
PROJ = "pill200"
OUT = r"C:\Users\rober\Documents\Claude\Projects\RAPT Pill\cmd_decomp.txt"
monitor = ConsoleTaskMonitor()

def region(addr):
    if 0x3f400000 <= addr < 0x3f800000: return "DROM_rodata", False
    if 0x3ff80000 <= addr < 0x40000000: return "DRAM", False
    if 0x40070000 <= addr < 0x40080000: return "IRAM_cache", True
    if 0x40080000 <= addr < 0x400a0000: return "IRAM", True
    if 0x400d0000 <= addr < 0x40400000: return "IROM_text", True
    return "SEG", False

def jbytes(b):
    a = array.array('b'); a.frombytes(b)
    return jpype.JArray(jpype.JByte)(a)

with pyghidra.open_program(BIN, project_location=PROJ_LOC, project_name=PROJ,
                           analyze=False, language="Xtensa:LE:32:default",
                           loader="ghidra.app.util.opinion.BinaryLoader") as api:
    program = api.getCurrentProgram()
    fm = program.getFunctionManager()
    mem = program.getMemory()
    sp = program.getAddressFactory().getDefaultAddressSpace()
    refmgr = program.getReferenceManager()
    data = open(BIN, "rb").read()
    seg_count = data[1]; entry = struct.unpack("<I", data[4:8])[0]

    tx = program.startTransaction("load-segments")
    try:
        for b in list(mem.getBlocks()):
            mem.removeBlock(b, monitor)
        off = 8 + 16
        for i in range(seg_count):
            load_addr, seg_len = struct.unpack("<II", data[off:off+8]); off += 8
            seg = data[off:off+seg_len]; off += seg_len
            nm, is_code = region(load_addr)
            blk = mem.createInitializedBlock("%s_%d" % (nm, i), sp.getAddress(load_addr),
                ByteArrayInputStream(jbytes(seg)), seg_len, monitor, False)
            blk.setRead(True); blk.setWrite(not is_code); blk.setExecute(is_code)
        ep = sp.getAddress(entry); api.addEntryPoint(ep); api.disassemble(ep)
        api.createFunction(ep, "entry_start")
    finally:
        program.endTransaction(tx, True)

    print("=== auto-analysis ===")
    tx2 = program.startTransaction("analyze")
    try:
        mgr = AutoAnalysisManager.getAnalysisManager(program)
        mgr.initializeOptions(); mgr.reAnalyzeAll(None); mgr.startAnalysis(monitor)
    finally:
        program.endTransaction(tx2, True)
    print("functions: %d" % fm.getFunctionCount())

    dec = DecompInterface(); dec.openProgram(program)
    def A(x): return sp.getAddress(x)
    def decompile(f):
        try:
            r = dec.decompileFunction(f, 120, monitor)
            if r and r.decompileCompleted():
                return r.getDecompiledFunction().getC()
        except Exception as e:
            return "// decompile error: %s" % e
        return "// decompile failed"
    seen = set(); out = []
    def emit(f, why):
        if f is None:
            out.append("// (no function) %s" % why); return
        ep = f.getEntryPoint()
        if ep in seen: return
        seen.add(ep)
        out.append("// ===== %s @ %s  (%s) =====" % (f.getName(), ep, why))
        out.append(decompile(f))

    # Wi-Fi service (svc0100) handler + the functions it calls, to learn the
    # SSID/password/connect chars and the connected-status/notify format.
    for addr, nm in ((0x400ee580, "svc0100_wifi_cb"), (0x400eeccc, "wifi_fn_ccc"),
                     (0x400eeac0, "wifi_fn_ac0"), (0x400ea0c4, "wifi_fn_a0c4"),
                     (0x400e983c, "wifi_fn_983c"), (0x400ea0d4, "wifi_fn_a0d4"),
                     (0x400fb2a0, "wifi_fn_fb2a0")):
        a = A(addr)
        if fm.getFunctionContaining(a) is None:
            tx3 = program.startTransaction("mk")
            try:
                api.disassemble(a); api.createFunction(a, nm)
            except Exception as e:
                print("mkfunc %#x: %s" % (addr, e))
            finally:
                program.endTransaction(tx3, True)
        emit(fm.getFunctionContaining(a), nm)

    f = open(OUT, "w"); f.write("\n\n".join(out)); f.close()
    print("WROTE %s  (%d functions)" % (OUT, len(seen)))
