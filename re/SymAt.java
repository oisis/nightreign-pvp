// Prints the symbols at the given addresses (for example the class a vtable belongs to).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.Symbol;

public class SymAt extends GhidraScript {
    public void run() throws Exception {
        for (String arg : getScriptArgs())
            for (Symbol s : currentProgram.getSymbolTable().getSymbols(toAddr(arg)))
                println("SYM " + arg + " " + s.getName(true));
    }
}
