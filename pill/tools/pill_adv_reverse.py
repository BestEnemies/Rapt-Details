"""Reverse the BLE advertisement builder in BOTH firmware images and dump C.

For each image: load ESP32 segments, auto-analyze, find the function that logs
the 'ble data' string (that's where the advertisement payload is assembled),
decompile it plus the helpers it calls. Writes adv_<tag>.txt per image.
"""
import struct, array
import pyghidra
pyghidra.start()
import jpype
from java.io import ByteArrayInputStream
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.app.plugin.core.analysis import AutoAnalysisManager
from ghidra.app.decompiler import DecompInterface

PROJ_LOC = r"C:\Users\rober\AppData\Local\Temp\claude\C--Users-rober-Documents-Claude-Projects-BarOS\31fe9203-16f9-40e5-a9f5-be12ba058805\scratchpad\gproj_adv"
IMAGES = [
  ("new2_0_0", r"C:\Users\rober\Documents\Claude\Projects\RAPT Pill\app_2.0.0.bin", 0x3f405998),
  ("old2023",  r"C:\Users\rober\Documents\Claude\Projects\RAPT Pill\server_PillG1_20230420_054501_98ef1a1.bin", 0x3f406d0f),
]
monitor = ConsoleTaskMonitor()

def region(a):
    if 0x3f400000<=a<0x3f800000: return False
    if 0x3ff80000<=a<0x40000000: return False
    if 0x40070000<=a<0x40080000: return True
    if 0x40080000<=a<0x400a0000: return True
    if 0x400d0000<=a<0x40400000: return True
    return False
def jbytes(b):
    arr=array.array('b'); arr.frombytes(b); return jpype.JArray(jpype.JByte)(arr)

for tag, BIN, strva in IMAGES:
    OUT = r"C:\Users\rober\Documents\Claude\Projects\RAPT Pill\adv_%s.txt" % tag
    with pyghidra.open_program(BIN, project_location=PROJ_LOC, project_name="adv_"+tag,
                               analyze=False, language="Xtensa:LE:32:default",
                               loader="ghidra.app.util.opinion.BinaryLoader") as api:
        program=api.getCurrentProgram(); fm=program.getFunctionManager()
        mem=program.getMemory(); sp=program.getAddressFactory().getDefaultAddressSpace()
        refmgr=program.getReferenceManager(); data=open(BIN,"rb").read()
        seg_count=data[1]
        tx=program.startTransaction("load")
        try:
            for b in list(mem.getBlocks()): mem.removeBlock(b,monitor)
            off=24
            for i in range(seg_count):
                la,sl=struct.unpack("<II",data[off:off+8]); off+=8
                seg=data[off:off+sl]; off+=sl
                blk=mem.createInitializedBlock("s%d"%i, sp.getAddress(la),
                    ByteArrayInputStream(jbytes(seg)), sl, monitor, False)
                blk.setRead(True); blk.setExecute(region(la)); blk.setWrite(not region(la))
            ep=sp.getAddress(struct.unpack("<I",data[4:8])[0]); api.addEntryPoint(ep); api.disassemble(ep); api.createFunction(ep,"entry")
        finally: program.endTransaction(tx,True)
        tx2=program.startTransaction("an")
        try:
            mgr=AutoAnalysisManager.getAnalysisManager(program); mgr.initializeOptions(); mgr.reAnalyzeAll(None); mgr.startAnalysis(monitor)
        finally: program.endTransaction(tx2,True)
        dec=DecompInterface(); dec.openProgram(program)
        def dc(f):
            try:
                r=dec.decompileFunction(f,120,monitor)
                if r and r.decompileCompleted(): return r.getDecompiledFunction().getC()
            except Exception as e: return "// err %s"%e
            return "// failed"
        seen=set(); out=["// image: %s  (ble-data str @%#x)"%(BIN,strva)]
        def emit(f,why):
            if f is None: out.append("// (no fn) "+why); return
            ep=f.getEntryPoint()
            if ep in seen: return
            seen.add(ep); out.append("\n// ===== %s @ %s (%s) ====="%(f.getName(),ep,why)); out.append(dc(f))
        A=sp.getAddress
        builder=fm.getFunctionContaining(A(strva)) # unlikely (it's a string)
        refs=list(refmgr.getReferencesTo(A(strva)))
        out.append("// refs to ble-data string: %d"%len(refs))
        callees=set()
        for r in refs:
            f=fm.getFunctionContaining(r.getFromAddress())
            emit(f, "builds/logs 'ble data' @%s"%r.getFromAddress())
            if f:
                # collect called functions to see metric packing
                for ins in program.getListing().getInstructions(f.getBody(), True):
                    for rr in ins.getReferencesFrom():
                        tf=fm.getFunctionAt(rr.getToAddress())
                        if tf: callees.add(tf.getEntryPoint())
        for ep in list(callees)[:10]:
            emit(fm.getFunctionAt(ep), "callee")
        open(OUT,"w").write("\n".join(out))
        print("WROTE %s (%d fns)"%(OUT,len(seen)))
