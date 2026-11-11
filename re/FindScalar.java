// Lists instructions in [start,end) that use any of the given constants as an operand.
// Args: <start> <end> <value>... (decimal or 0x hex)
import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;

public class FindScalar extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Set<Long> want = new HashSet<>();
        for (int i = 2; i < a.length; i++) want.add(Long.decode(a[i]));
        InstructionIterator it = currentProgram.getListing().getInstructions(new AddressSet(toAddr(a[0]), toAddr(a[1])), true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            for (int op = 0; op < ins.getNumOperands(); op++) {
                Scalar s = ins.getScalar(op);
                if (s != null && want.contains(s.getUnsignedValue())) {
                    Function f = getFunctionContaining(ins.getAddress());
                    println("SCALAR " + s.getUnsignedValue() + " at " + ins.getAddress() + " " + ins + "   in " + (f == null ? "?" : f.getEntryPoint().toString()));
                }
            }
        }
    }
}
