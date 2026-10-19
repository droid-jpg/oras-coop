// Prints instructions in [start, end). Script args: <startHex> <endHex>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

public class Listing extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        Address start = toAddr(Long.parseLong(a[0].replace("0x", ""), 16));
        Address end = toAddr(Long.parseLong(a[1].replace("0x", ""), 16));
        Instruction ins = getInstructionAt(start);
        if (ins == null) ins = getInstructionAfter(start);
        while (ins != null && ins.getAddress().compareTo(end) < 0) {
            println(ins.getAddress() + "  " + ins);
            ins = ins.getNext();
        }
    }
}
