// For each function address given: list its callers, and for every caller the RTTI class
// whose vtable holds it (if any). Optional last arg "-c" also prints the target's decompiled C.
import java.util.*;
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;

public class Callers extends GhidraScript {
    String owner(Address fn) {  // class owning a vtable slot that points at fn
        for (Reference r : getReferencesTo(fn)) {
            if (!r.getReferenceType().isData()) continue;
            Address a = r.getFromAddress();
            for (int i = 0; i < 80; i++, a = a.subtract(8)) {
                Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(a);
                if (s != null) {
                    if (s.getName().equals("vftable")) return s.getParentNamespace().getName(true) + "::vfunc" + i;
                    break;
                }
            }
        }
        return "";
    }

    public void run() throws Exception {
        List<String> args = new ArrayList<>(Arrays.asList(getScriptArgs()));
        boolean c = args.remove("-c");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        for (String arg : args) {
            Function f = getFunctionAt(toAddr(arg));
            println("CALLERS target " + arg + " " + (f == null ? "<no function>" : f.getName() + " " + owner(f.getEntryPoint())));
            if (f == null) continue;
            TreeMap<Address, String> seen = new TreeMap<>();
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                Function caller = getFunctionContaining(r.getFromAddress());
                if (caller != null && r.getReferenceType().isCall())
                    seen.put(caller.getEntryPoint(), caller.getName() + " " + owner(caller.getEntryPoint()));
            }
            for (Map.Entry<Address, String> e : seen.entrySet()) println("CALLERS   " + e.getKey() + " " + e.getValue());
            if (c) {
                DecompileResults res = dec.decompileFunction(f, 60, monitor);
                println("CALLERS code\n" + (res.decompileCompleted() ? res.getDecompiledFunction().getC() : res.getErrorMessage()));
            }
        }
    }
}
