// Lists every reference to the given addresses: where it comes from, the instruction there and its bytes.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

public class Xrefs extends GhidraScript {
    public void run() throws Exception {
        for (String arg : getScriptArgs()) {
            for (Reference r : getReferencesTo(toAddr(arg))) {
                Instruction i = getInstructionAt(r.getFromAddress());
                StringBuilder hex = new StringBuilder();
                if (i != null) for (byte b : i.getBytes()) hex.append(String.format("%02x", b));
                println("XREF " + arg + " <- " + r.getFromAddress() + " " + r.getReferenceType() + " " + (i == null ? "(data)" : i + " [" + hex + "]"));
            }
        }
    }
}
