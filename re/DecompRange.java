// Decompiles every function whose entry lies in [start, end). Args: <start> <end>
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.*;

public class DecompRange extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(new AddressSet(toAddr(a[0]), toAddr(a[1])), true);
        while (it.hasNext()) {
            Function f = it.next();
            DecompileResults res = dec.decompileFunction(f, 60, monitor);
            println("RANGE " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses() + "\n"
                + (res.decompileCompleted() ? res.getDecompiledFunction().getC() : "failed"));
        }
    }
}
