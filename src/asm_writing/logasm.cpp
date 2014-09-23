// Copyright (c) 2014 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "asm_writing/logasm.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/Mangler.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/MemoryObject.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"

#include "codegen/codegen.h"

namespace pyston {
namespace assembler {

// http://blog.llvm.org/2010/01/x86-disassembler.html
class BufferMemoryObject : public llvm::MemoryObject {
private:
    const uint8_t* Bytes;
    uint64_t Length;

public:
    BufferMemoryObject(const uint8_t* bytes, uint64_t length) : Bytes(bytes), Length(length) {}

    uint64_t getBase() const { return 0; }
    uint64_t getExtent() const { return Length; }

    int readByte(uint64_t addr, uint8_t* byte) const {
        if (addr > getExtent())
            return -1;
        *byte = Bytes[addr];
        return 0;
    }
};

void AssemblyLogger::log_comment(const std::string& comment, size_t offset) {
    comments[offset].push_back(comment);
}

void AssemblyLogger::append_comments(llvm::raw_string_ostream& stream, size_t pos) const {
    if (comments.count(pos) > 0) {
        for (const std::string& comment : comments.at(pos)) {
            stream << "; " << comment << "\n";
        }
    }
}

std::string AssemblyLogger::finalize_log(uint8_t const* start_addr, uint8_t const* end_addr) const {
    BufferMemoryObject bufferMObj(start_addr, end_addr - start_addr);

    std::string result = "";
    llvm::raw_string_ostream stream(result);

    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    llvm::InitializeNativeTargetDisassembler();

    // TODO oh god the memory leak

    const llvm::StringRef triple = g.tm->getTargetTriple();
    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
    assert(target);
    const llvm::MCRegisterInfo* MRI = target->createMCRegInfo(triple);
    assert(MRI);
    const llvm::MCAsmInfo* MAI = target->createMCAsmInfo(*MRI, triple);
    assert(MAI);
    const llvm::MCInstrInfo* MII = target->createMCInstrInfo();
    assert(MII);
    std::string FeaturesStr;
    const llvm::StringRef CPU = "";
    const llvm::MCSubtargetInfo* STI = target->createMCSubtargetInfo(triple, CPU, FeaturesStr);
    assert(STI);
    int AsmPrinterVariant = MAI->getAssemblerDialect(); // 0 is ATT, 1 is Intel
    llvm::MCInstPrinter* IP = target->createMCInstPrinter(AsmPrinterVariant, *MAI, *MII, *MRI, *STI);
    assert(IP);
    llvm::MCObjectFileInfo* MOFI = new llvm::MCObjectFileInfo();
    assert(MOFI);
    llvm::MCContext* Ctx = new llvm::MCContext(MAI, MRI, MOFI);
    assert(Ctx);
    llvm::MCDisassembler* DisAsm = target->createMCDisassembler(*STI, *Ctx);
    assert(DisAsm);

    size_t pos = 0;
    append_comments(stream, pos);

    while (pos < (end_addr - start_addr)) {
        llvm::MCInst inst;
        uint64_t size;
        llvm::MCDisassembler::DecodeStatus s = DisAsm->getInstruction(
            inst /* out */,
            size /* out */,
            bufferMObj, pos, llvm::nulls(), llvm::nulls());
        assert(s == llvm::MCDisassembler::Success);

        IP->printInst(&inst, stream, "");
        stream << "\n";
        
        pos += size;
        append_comments(stream, pos);
    }

    return stream.str();
}

}
}
