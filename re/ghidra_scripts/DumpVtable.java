// Prints a vtable's entries with function size, callee count and the first decompiled lines.
// Script args: <vtableAddrHex> <entries> [decompileLines]
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DumpVtable extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        Address vtable = toAddr(Long.parseLong(a[0].replace("0x", ""), 16));
        int entries = Integer.parseInt(a[1]);
        int lines = a.length > 2 ? Integer.parseInt(a[2]) : 0;

        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);

        for (int i = 0; i < entries; i++) {
            Address slot = vtable.add(i * 4L);
            long target = currentProgram.getMemory().getInt(slot) & 0xFFFFFFFFL;
            Address fa = toAddr(target & ~1L);
            Function f = getFunctionAt(fa);
            String info = f == null ? "(no function)"
                : String.format("%s size=%d callees=%d", f.getName(), f.getBody().getNumAddresses(),
                    f.getCalledFunctions(monitor).size());
            println(String.format("[%2d] +0x%02x -> %08x %s", i, i * 4, target, info));
            if (lines > 0 && f != null) {
                DecompileResults r = di.decompileFunction(f, 30, monitor);
                if (r.decompileCompleted()) {
                    String[] src = r.getDecompiledFunction().getC().split("\n");
                    for (int j = 0; j < Math.min(lines, src.length); j++) {
                        if (!src[j].isBlank()) println("      " + src[j]);
                    }
                }
            }
        }
        di.dispose();
    }
}
