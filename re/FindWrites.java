// Lists instructions in [start,end) that write to [reg + <offset>]. Args: <start> <end> <offset hex, e.g. 0x170>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.*;

public class FindWrites extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        String needle = "+ " + a[2] + "]";
        InstructionIterator it = currentProgram.getListing().getInstructions(new AddressSet(toAddr(a[0]), toAddr(a[1])), true);
        while (it.hasNext()) {
            Instruction i = it.next();
            String op0 = i.getNumOperands() > 0 ? i.getDefaultOperandRepresentation(0) : "";
            if (op0.contains(needle) && !i.getMnemonicString().startsWith("CMP") && !i.getMnemonicString().startsWith("TEST")) {
                Function f = getFunctionContaining(i.getAddress());
                println("WRITE " + i.getAddress() + " " + i + "   in " + (f == null ? "?" : f.getEntryPoint().toString()));
            }
        }
    }
}
