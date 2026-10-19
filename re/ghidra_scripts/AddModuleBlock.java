// Adds a relocated CRO memory dump as a new initialized block, then seeds functions from a vtable.
// Script args: <dumpFile> <baseAddrHex> <blockName> <vtableAddrHex> <vtableEntries>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import java.io.File;
import java.io.FileInputStream;

public class AddModuleBlock extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        File dump = new File(a[0]);
        Address base = toAddr(Long.parseLong(a[1].replace("0x", ""), 16));
        String name = a[2];
        Address vtable = toAddr(Long.parseLong(a[3].replace("0x", ""), 16));
        int entries = Integer.parseInt(a[4]);

        Memory mem = currentProgram.getMemory();
        if (mem.getBlock(name) == null) {
            try (FileInputStream in = new FileInputStream(dump)) {
                MemoryBlock b = mem.createInitializedBlock(name, base, in, dump.length(), monitor, false);
                b.setRead(true);
                b.setWrite(true);
                b.setExecute(true);
            }
            println("added block " + name + " at " + base + " size 0x" + Long.toHexString(dump.length()));
        } else {
            println("block " + name + " already present");
        }

        for (int i = 0; i < entries; i++) {
            Address slot = vtable.add(i * 4L);
            long target = mem.getInt(slot) & 0xFFFFFFFFL;
            Address fn = toAddr(target & ~1L);
            if (!mem.contains(fn)) continue;
            disassemble(fn);
            if (getFunctionAt(fn) == null) createFunction(fn, null);
        }
        println("seeded functions from vtable " + vtable);
    }
}
