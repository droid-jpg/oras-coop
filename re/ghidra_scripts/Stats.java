// Prints basic stats for the analyzed program.
import ghidra.app.script.GhidraScript;

public class Stats extends GhidraScript {
    @Override
    protected void run() throws Exception {
        println("functions: " + currentProgram.getFunctionManager().getFunctionCount());
        println("memory: " + currentProgram.getMemory().getMinAddress() + " - " + currentProgram.getMemory().getMaxAddress());
        println("language: " + currentProgram.getLanguageID());
    }
}
