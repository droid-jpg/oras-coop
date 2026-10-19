// Lists functions whose entry is in [start,end) with size and caller count.
// Script args: <startHex> <endHex>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class ListFuncs extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        Address start = toAddr(Long.parseLong(a[0].replace("0x", ""), 16));
        Address end = toAddr(Long.parseLong(a[1].replace("0x", ""), 16));
        for (Function f : currentProgram.getFunctionManager().getFunctions(start, true)) {
            if (f.getEntryPoint().compareTo(end) >= 0) break;
            println(String.format("%s size=%d callers=%d callees=%d", f.getEntryPoint(),
                f.getBody().getNumAddresses(), f.getCallingFunctions(monitor).size(),
                f.getCalledFunctions(monitor).size()));
        }
    }
}
