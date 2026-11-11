// FD4 step tables pair a function pointer with a step-name string. For every string matching <regex>,
// prints the qwords around each data reference to it, marking those that are function entries.
// With "-c" also decompiles those functions.
import java.util.*;
import java.util.regex.Pattern;
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;

public class StepTable extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Pattern pat = Pattern.compile(a[0]);
        boolean code = a.length > 1 && a[1].equals("-c");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext()) {
            Data d = it.next();
            if (!d.hasStringValue()) continue;
            String s = StringDataInstance.getStringDataInstance(d).getStringValue();
            if (s == null || !pat.matcher(s).find()) continue;
            for (Reference r : getReferencesTo(d.getAddress())) {
                Address ref = r.getFromAddress();
                if (getFunctionContaining(ref) != null) continue;  // only table entries
                for (int off = -0x18; off <= 0x10; off += 8) {
                    Function f = null;
                    try { f = getFunctionAt(toAddr(getLong(ref.add(off)))); } catch (Exception e) { }
                    if (f == null) continue;
                    println("STEP " + s + " -> " + f.getEntryPoint() + " (slot " + off + ")");
                    if (code) {
                        DecompileResults res = dec.decompileFunction(f, 60, monitor);
                        println("STEP code " + s + "\n" + (res.decompileCompleted() ? res.getDecompiledFunction().getC() : "failed"));
                    }
                }
            }
        }
    }
}
