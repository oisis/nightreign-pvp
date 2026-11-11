// Prints function/symbol counts and BreakIn-related symbols: a smoke test that the saved analysis is usable.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.Symbol;
public class Overview extends GhidraScript {
    public void run() throws Exception {
        println("OVERVIEW program=" + currentProgram.getName()
            + " functions=" + currentProgram.getFunctionManager().getFunctionCount()
            + " symbols=" + currentProgram.getSymbolTable().getNumSymbols());
        int n = 0;
        for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(false)) {
            String name = s.getName(true);
            if (name.contains("BreakIn") && n++ < 15) println("OVERVIEW sym " + s.getAddress() + " " + name);
        }
        println("OVERVIEW breakin_symbols=" + n);
    }
}
