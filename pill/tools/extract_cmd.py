# Ghidra headless postScript: decompile the Direct-command parser, its callers
# (the GATT write handler = the framing), and functions referencing the GATT
# sub-characteristic log strings. Writes C to cmd_decomp.txt.
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = r"C:\Users\rober\Documents\Claude\Projects\RAPT Pill\cmd_decomp.txt"
fp = currentProgram.getFunctionManager()
rm = currentProgram.getReferenceManager()
space = currentProgram.getAddressFactory().getDefaultAddressSpace()
def A(x): return space.getAddress(x)

dec = DecompInterface(); dec.openProgram(currentProgram)
mon = ConsoleTaskMonitor()

def decompile(func):
    try:
        r = dec.decompileFunction(func, 120, mon)
        if r and r.decompileCompleted():
            return r.getDecompiledFunction().getC()
    except Exception as e:
        return "// decompile error: %s" % e
    return "// decompile failed"

seen = set(); out = []
def emit(func, why):
    if func is None:
        out.append("// (no function) %s" % why); return
    ep = func.getEntryPoint()
    if ep in seen: return
    seen.add(ep)
    out.append("// ===== %s @ %s  (%s) =====" % (func.getName(), ep, why))
    out.append(decompile(func))

# 1) command parser (contains the report_now/keep_awake/... literals)
cmdfn = fp.getFunctionContaining(A(0x400d1dcc))
emit(cmdfn, "command parser")

# 2) callers of the command parser -> the GATT write handler (framing)
if cmdfn:
    for r in rm.getReferencesTo(cmdfn.getEntryPoint()):
        emit(fp.getFunctionContaining(r.getFromAddress()), "caller of command parser @%s" % r.getFromAddress())

# 3) functions referencing the GATT sub-characteristic / direct-command strings
for sa in (0x3f40cd90, 0x3f40d1f7, 0x3f40d236, 0x3f4097a8):
    for r in rm.getReferencesTo(A(sa)):
        emit(fp.getFunctionContaining(r.getFromAddress()), "refs string %#x @%s" % (sa, r.getFromAddress()))

f = open(OUT, "w"); f.write("\n\n".join(out)); f.close()
print("WROTE %s  (%d functions)" % (OUT, len(seen)))
