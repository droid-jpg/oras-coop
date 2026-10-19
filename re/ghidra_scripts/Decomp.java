// Decompiles the function containing each given address and writes it to <outDir>/<addr>.c,
// also printing the first lines. Script args: <outDir> <addrHex>...
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.File;
import java.io.FileWriter;

public class Decomp extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        File outDir = new File(a[0]);
        outDir.mkdirs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (int i = 1; i < a.length; i++) {
            Address addr = toAddr(Long.parseLong(a[i].replace("0x", ""), 16));
            Function f = getFunctionContaining(addr);
            if (f == null) {
                disassemble(addr);
                f = getFunctionContaining(addr);
            }
            if (f == null) { println(a[i] + ": no function"); continue; }
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage();
            StringBuilder callers = new StringBuilder();
            for (Function caller : f.getCallingFunctions(monitor)) callers.append(caller.getEntryPoint()).append(' ');
            String header = String.format("// %s in %s @ %s (size %d)\n// callers: %s\n",
                a[i], f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses(), callers);
            try (FileWriter w = new FileWriter(new File(outDir, a[i].replace("0x", "") + ".c"))) {
                w.write(header + c);
            }
            println(header.trim() + "  -> " + c.split("\n").length + " lines");
        }
        di.dispose();
    }
}
