// Lists instructions in [start,end) whose text contains the given substring, plus the next N.
// Script args: <startHex> <endHex> <substring> [N=3]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

public class FindImm extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        Address start = toAddr(Long.parseLong(a[0].replace("0x", ""), 16));
        Address end = toAddr(Long.parseLong(a[1].replace("0x", ""), 16));
        String sub = a[2];
        int n = a.length > 3 ? Integer.parseInt(a[3]) : 3;
        Instruction ins = getInstructionAt(start);
        if (ins == null) ins = getInstructionAfter(start);
        while (ins != null && ins.getAddress().compareTo(end) < 0) {
            if (ins.toString().contains(sub)) {
                Function f = getFunctionContaining(ins.getAddress());
                StringBuilder sb = new StringBuilder(ins.getAddress() + " [" + (f == null ? "?" : f.getName()) + "] " + ins);
                Instruction nx = ins.getNext();
                for (int i = 0; i < n && nx != null; i++, nx = nx.getNext()) sb.append(" ; ").append(nx);
                println(sb.toString());
            }
            ins = ins.getNext();
        }
    }
}
