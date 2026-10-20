// Like DecompAt, but first clears "no return" on every function (DllField's import thunks into
// code.bin get marked no-return, which cuts decompiles short), re-disassembles past the calls and
// rebuilds the function body. Not saved: run with -readOnly. Script args: <outDir> <addrHex>...
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.File;
import java.io.FileWriter;

public class DecompRet extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        File outDir = new File(a[0]);
        outDir.mkdirs();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.hasNoReturn()) f.setNoReturn(false);
        }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (int i = 1; i < a.length; i++) {
            Address addr = toAddr(Long.parseLong(a[i].replace("0x", ""), 16));
            Function f = getFunctionContaining(addr);
            if (f != null && f.getEntryPoint().equals(addr)) removeFunction(f);
            if (f == null || f.getEntryPoint().equals(addr)) {
                clearListing(addr, addr.add(0x3000));
                disassemble(addr);
                f = createFunction(addr, null);
            }
            if (f == null) { println(a[i] + ": no function"); continue; }
            DecompileResults r = di.decompileFunction(f, 120, monitor);
            String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage();
            String header = String.format("// %s in %s @ %s (size %d)\n", a[i], f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses());
            try (FileWriter w = new FileWriter(new File(outDir, a[i].replace("0x", "") + ".c"))) { w.write(header + c); }
            println(header.trim() + "  -> " + c.split("\n").length + " lines");
        }
        di.dispose();
    }
}
