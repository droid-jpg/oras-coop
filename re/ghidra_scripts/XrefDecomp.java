// For each given address, lists references to it and decompiles each referencing function
// into <outDir>/xref_<target>_<func>.c. Script args: <outDir> <addrHex>...
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.File;
import java.io.FileWriter;
import java.util.HashSet;

public class XrefDecomp extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        File outDir = new File(a[0]);
        outDir.mkdirs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (int i = 1; i < a.length; i++) {
            Address t = toAddr(Long.parseLong(a[i].replace("0x", ""), 16));
            HashSet<Function> seen = new HashSet<>();
            for (Reference r : getReferencesTo(t)) {
                Function f = getFunctionContaining(r.getFromAddress());
                println(a[i] + " <- " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (f == null ? "?" : f.getName()));
                if (f == null || !seen.add(f)) continue;
                DecompileResults res = di.decompileFunction(f, 60, monitor);
                String c = res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// failed";
                try (FileWriter w = new FileWriter(new File(outDir, "xref_" + a[i].replace("0x", "") + "_" + f.getEntryPoint() + ".c"))) {
                    w.write("// refs " + a[i] + " from " + r.getFromAddress() + "\n" + c);
                }
            }
        }
        di.dispose();
    }
}
