// Decompiles every caller of <addr> (or sym:<name>) and prints those whose C matches <regex> (DOTALL). Arg 3 "-c" prints the code.
import java.util.*;
import java.util.regex.Pattern;
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class GrepCallers extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Pattern pat = Pattern.compile(a[1], Pattern.DOTALL);
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        TreeSet<Address> callers = new TreeSet<>();
        List<Address> targets = new ArrayList<>();
        if (a[0].startsWith("sym:")) {  // every symbol with this name (imports, thunks)
            for (ghidra.program.model.symbol.Symbol s : currentProgram.getSymbolTable().getSymbols(a[0].substring(4))) targets.add(s.getAddress());
        } else targets.add(toAddr(a[0]));
        for (Address t : targets) for (Reference r : getReferencesTo(t)) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null) callers.add(f.getEntryPoint());
        }
        println("GREP callers=" + callers.size());
        for (Address e : callers) {
            DecompileResults res = dec.decompileFunction(getFunctionAt(e), 60, monitor);
            if (!res.decompileCompleted()) continue;
            String c = res.getDecompiledFunction().getC();
            if (pat.matcher(c).find()) println("GREP match " + e + (a.length > 2 ? "\n" + c : ""));
        }
    }
}
