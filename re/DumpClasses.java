// Dumps every RTTI class whose name matches a regex: vtable address, virtual functions,
// functions referencing the vtable (constructors/destructors), and decompiled C for all of them.
// Args: <class-name regex> <output dir>
import java.io.*;
import java.util.*;
import java.util.regex.Pattern;
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;

public class DumpClasses extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Pattern pat = Pattern.compile(args[0]);
        File out = new File(args[1]);
        out.mkdirs();
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        PrintWriter index = new PrintWriter(new File(out, "_index.tsv"));
        SymbolIterator it = currentProgram.getSymbolTable().getSymbols("vftable");
        while (it.hasNext()) {
            Symbol s = it.next();
            String cls = s.getParentNamespace().getName(true);
            if (!pat.matcher(cls).find()) continue;
            Address vt = s.getAddress();
            LinkedHashMap<Address, String> funcs = new LinkedHashMap<>();
            for (int i = 0; i < 64; i++) {  // walk the vtable until the first non-function slot
                Address slot = vt.add(i * 8L);
                if (i > 0 && currentProgram.getSymbolTable().getPrimarySymbol(slot) != null) break;
                Address target = toAddr(getLong(slot));
                Function f = getFunctionAt(target);
                if (f == null) break;
                funcs.put(target, "vfunc" + i);
            }
            for (Reference r : getReferencesTo(vt)) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null) funcs.putIfAbsent(f.getEntryPoint(), "refs_vtable");
            }
            String file = cls.replaceAll("[^A-Za-z0-9_]+", "_") + "_" + vt + ".c";
            PrintWriter w = new PrintWriter(new File(out, file));
            w.println("// class " + cls + " vtable " + vt);
            for (Map.Entry<Address, String> e : funcs.entrySet()) {
                Function f = getFunctionAt(e.getKey());
                index.println(cls + "\t" + vt + "\t" + e.getValue() + "\t" + e.getKey() + "\t" + f.getName());
                w.println("\n// ---- " + e.getValue() + " " + e.getKey() + " " + f.getName());
                DecompileResults res = dec.decompileFunction(f, 60, monitor);
                w.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// decompile failed: " + res.getErrorMessage());
            }
            w.close();
            println("DUMP " + cls + " " + vt + " funcs=" + funcs.size());
        }
        index.close();
    }
}
