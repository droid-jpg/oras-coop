// Lists functions in [start,end) containing stores with one of the given immediate offsets.
// Script args: <startHex> <endHex> <offHex>[,<offHex>...]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import java.util.*;

public class FindLoads extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        Address start = toAddr(Long.parseLong(a[0].replace("0x", ""), 16));
        Address end = toAddr(Long.parseLong(a[1].replace("0x", ""), 16));
        Set<String> offs = new HashSet<>();
        for (String o : a[2].split(",")) offs.add("#0x" + Long.toHexString(Long.parseLong(o.replace("0x", ""), 16)) + "]");
        Map<Function, List<String>> hits = new LinkedHashMap<>();
        Instruction ins = getInstructionAt(start);
        if (ins == null) ins = getInstructionAfter(start);
        while (ins != null && ins.getAddress().compareTo(end) < 0) {
            String m = ins.getMnemonicString().toLowerCase();
            if (m.startsWith(System.getProperty("pfx","ldrb"))) {
                String s = ins.toString();
                for (String o : offs) {
                    if (s.endsWith(o) || s.contains(o + "!")) {
                        Function f = getFunctionContaining(ins.getAddress());
                        if (f != null) hits.computeIfAbsent(f, k -> new ArrayList<>()).add(ins.getAddress() + " " + s);
                    }
                }
            }
            ins = ins.getNext();
        }
        for (Map.Entry<Function, List<String>> e : hits.entrySet()) {
            Function f = e.getKey();
            println(String.format("%s @ %s size=%d callers=%d", f.getName(), f.getEntryPoint(),
                f.getBody().getNumAddresses(), f.getCallingFunctions(monitor).size()));
            for (String s : e.getValue()) println("    " + s);
        }
    }
}
