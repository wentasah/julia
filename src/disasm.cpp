//===------------- Disassembler for in-memory function --------------------===//
//
// Modified for use in The Julia Language from code in the  llvm-mc project:
//      llvm-mc.cpp and Disassembler.cpp
//
// Original copyright:
//
// University of Illinois/NCSA
// Open Source License
// Copyright (c) 2003-2016 University of Illinois at Urbana-Champaign.
// All rights reserved.
//
//  Developed by:
//
//    LLVM Team
//
//    University of Illinois at Urbana-Champaign
//
//    http://llvm.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal with
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
//
//    * Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimers.
//
//    * Redistributions in binary form must reproduce the above copyright notice,
//      this list of conditions and the following disclaimers in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the names of the LLVM Team, University of Illinois at
//      Urbana-Champaign, nor the names of its contributors may be used to
//      endorse or promote products derived from this Software without specific
//      prior written permission.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE
// SOFTWARE.
//===----------------------------------------------------------------------===//
//
// This class implements a disassembler of a memory block, given a function
// pointer and size.
//
//===----------------------------------------------------------------------===//

#include <map>
#include <set>
#include <string>

#include "llvm-version.h"

// for outputting disassembly
#include <llvm/ADT/Triple.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/BinaryFormat/COFF.h>
#include <llvm/BinaryFormat/MachO.h>
#include <llvm/DebugInfo/DIContext.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/ExecutionEngine/JITEventListener.h>
#include <llvm/IR/AssemblyAnnotationWriter.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/MCAsmBackend.h>
#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCDisassembler/MCDisassembler.h>
#include <llvm/MC/MCDisassembler/MCExternalSymbolizer.h>
#include <llvm/MC/MCExpr.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/MCInstrAnalysis.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCObjectFileInfo.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCStreamer.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/MCSymbol.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/NativeFormatting.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

// for outputting assembly
#include <llvm/CodeGen/AsmPrinter.h>
#include <llvm/CodeGen/AsmPrinterHandler.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/Passes.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/IR/LegacyPassManager.h>

#include "julia.h"
#include "julia_internal.h"
#include "jitlayers.h"
#include "processor.h"

using namespace llvm;
#include "debuginfo.h"
#include "julia_assert.h"

// helper class for tracking inlining context while printing debug info
class DILineInfoPrinter {
    // internal state:
    std::vector<DILineInfo> context;
    uint32_t inline_depth = 0;
    // configuration options:
    const char* LineStart = "; ";
    bool bracket_outer = false;
    bool collapse_recursive = true;

    enum {
        output_none = 0,
        output_source = 1,
    } verbosity = output_source;
public:
    DILineInfoPrinter(const char *LineStart, bool bracket_outer)
        : LineStart(LineStart),
          bracket_outer(bracket_outer) {};
    void SetVerbosity(const char *c)
    {
        if (StringRef("default") == c) {
            verbosity = output_source;
        }
        else if (StringRef("source") == c) {
            verbosity = output_source;
        }
        else if (StringRef("none") == c) {
            verbosity = output_none;
        }
    }

    void emit_finish(raw_ostream &Out);
    void emit_lineinfo(raw_ostream &Out, std::vector<DILineInfo> &DI);

    struct repeat {
        size_t times;
        const char *c;
    };
    struct repeat inlining_indent(const char *c)
    {
        return repeat{
            std::max(inline_depth + bracket_outer, (uint32_t)1) - 1,
            c };
    }

    template<class T>
    void emit_lineinfo(std::string &Out, T &DI)
    {
        raw_string_ostream OS(Out);
        emit_lineinfo(OS, DI);
    }

    void emit_lineinfo(raw_ostream &Out, DILineInfo &DI)
    {
        std::vector<DILineInfo> DIvec(1);
        DIvec[0] = DI;
        emit_lineinfo(Out, DIvec);
    }

    void emit_lineinfo(raw_ostream &Out, DIInliningInfo &DI)
    {
        uint32_t nframes = DI.getNumberOfFrames();
        std::vector<DILineInfo> DIvec(nframes);
        for (uint32_t i = 0; i < DI.getNumberOfFrames(); i++) {
            DIvec[i] = DI.getFrame(i);
        }
        emit_lineinfo(Out, DIvec);
    }

    void emit_finish(std::string &Out)
    {
        raw_string_ostream OS(Out);
        emit_finish(OS);
    }
};

static raw_ostream &operator<<(raw_ostream &Out, struct DILineInfoPrinter::repeat i)
{
    while (i.times-- > 0)
        Out << i.c;
    return Out;
}

void DILineInfoPrinter::emit_finish(raw_ostream &Out)
{
    auto pops = inlining_indent("└");
    if (pops.times > 0)
        Out << LineStart << pops << '\n';
    context.clear();
    this->inline_depth = 0;
}

void DILineInfoPrinter::emit_lineinfo(raw_ostream &Out, std::vector<DILineInfo> &DI)
{
    if (verbosity == output_none)
        return;
    uint32_t nframes = DI.size();
    if (nframes == 0)
        return; // just skip over lines with no debug info at all
    // compute the size of the matching prefix in the inlining information stack
    uint32_t nctx;
    for (nctx = 0; nctx < context.size() && nctx < nframes; nctx++) {
        const DILineInfo &CtxLine = context.at(nctx);
        const DILineInfo &FrameLine = DI.at(nframes - 1 - nctx);
        if (CtxLine != FrameLine) {
            break;
        }
    }
    bool update_line_only = false;
    if (collapse_recursive) {
        if (nctx > 0) {
            // check if we're adding more frames with the same method name,
            // if so, drop all existing calls to it from the top of the context
            // AND check if instead the context was previously printed that way
            // but now has removed the recursive frames
            StringRef method = StringRef(context.at(nctx - 1).FunctionName).rtrim(';'); // last matching frame
            if ((nctx < nframes && StringRef(DI.at(nframes - nctx - 1).FunctionName).rtrim(';') == method) ||
                (nctx < context.size() && StringRef(context.at(nctx).FunctionName).rtrim(';') == method)) {
                update_line_only = true;
                // transform nctx to exclude the combined frames
                while (nctx > 0 && StringRef(context.at(nctx - 1).FunctionName).rtrim(';') == method)
                    nctx -= 1;
            }
        }
        if (!update_line_only && nctx < context.size() && nctx < nframes) {
            // look at the first non-matching element to see if we are only changing the line number
            const DILineInfo &CtxLine = context.at(nctx);
            const DILineInfo &FrameLine = DI.at(nframes - 1 - nctx);
            if (StringRef(CtxLine.FunctionName).rtrim(';') == StringRef(FrameLine.FunctionName).rtrim(';'))
                update_line_only = true;
        }
    }
    else if (nctx < context.size() && nctx < nframes) {
        // look at the first non-matching element to see if we are only changing the line number
        const DILineInfo &CtxLine = context.at(nctx);
        const DILineInfo &FrameLine = DI.at(nframes - 1 - nctx);
        if (CtxLine.FileName == FrameLine.FileName &&
                StringRef(CtxLine.FunctionName).rtrim(';') == StringRef(FrameLine.FunctionName).rtrim(';')) {
            update_line_only = true;
        }
    }
    // examine how many frames we're returning from
    if (nctx < context.size()) {
        // compute the new inlining depth
        uint32_t npops;
        if (collapse_recursive) {
            npops = 1;
            StringRef Prev = StringRef(context.at(nctx).FunctionName).rtrim(';');
            for (uint32_t i = nctx + 1; i < context.size(); i++) {
                StringRef Next = StringRef(context.at(i).FunctionName).rtrim(';');
                if (Prev != Next)
                    npops += 1;
                Prev = Next;
            }
        }
        else {
            npops = context.size() - nctx;
        }
        context.resize(nctx);
        update_line_only && (npops -= 1);
        if (npops > 0) {
            this->inline_depth -= npops;
            Out << LineStart << inlining_indent("│") << repeat{npops, "└"} << '\n';
        }
    }
    // print the new frames
    while (nctx < nframes) {
        const DILineInfo &frame = DI.at(nframes - 1 - nctx);
        Out << LineStart << inlining_indent("│");
        nctx += 1;
        context.push_back(frame);
        if (update_line_only) {
            update_line_only = false;
        }
        else {
            this->inline_depth += 1;
            if (bracket_outer || nctx != 1)
                Out << "┌";
        }
        Out << " @ " << frame.FileName;
        if (frame.Line != UINT_MAX && frame.Line != 0)
            Out << ":" << frame.Line;
        StringRef method = StringRef(frame.FunctionName).rtrim(';');
        Out << " within `" << method << "`";
        if (collapse_recursive) {
            while (nctx < nframes) {
                const DILineInfo &frame = DI.at(nframes - 1 - nctx);
                if (StringRef(frame.FunctionName).rtrim(';') != method)
                    break;
                nctx += 1;
                context.push_back(frame);
                Out << " @ " << frame.FileName
                    << ":" << frame.Line;
            }
        }
        Out << "\n";
    }
#ifndef JL_NDEBUG
    StringRef Prev = StringRef(context.at(0).FunctionName).rtrim(';');
    uint32_t depth2 = 1;
    for (uint32_t i = 1; i < nctx; i++) {
        StringRef Next = StringRef(context.at(i).FunctionName).rtrim(';');
        if (!collapse_recursive || Prev != Next)
            depth2 += 1;
        Prev = Next;
    }
    assert(this->inline_depth == depth2);
#endif
}


// adaptor class for printing line numbers before llvm IR lines
class LineNumberAnnotatedWriter : public AssemblyAnnotationWriter {
    const DILocation *InstrLoc = nullptr;
    DILineInfoPrinter LinePrinter;
    DenseMap<const Instruction *, DILocation *> DebugLoc;
    DenseMap<const Function *, DISubprogram *> Subprogram;
public:
    LineNumberAnnotatedWriter(const char *LineStart, bool bracket_outer, const char *debuginfo)
      : LinePrinter(LineStart, bracket_outer) {
        LinePrinter.SetVerbosity(debuginfo);
    }
    virtual void emitFunctionAnnot(const Function *, formatted_raw_ostream &);
    virtual void emitInstructionAnnot(const Instruction *, formatted_raw_ostream &);
    virtual void emitInstructionAnnot(const DILocation *, formatted_raw_ostream &);
    virtual void emitBasicBlockEndAnnot(const BasicBlock *, formatted_raw_ostream &);
    // virtual void printInfoComment(const Value &, formatted_raw_ostream &) {}

    void emitEnd(formatted_raw_ostream &Out) {
        LinePrinter.emit_finish(Out);
        InstrLoc = nullptr;
    }

    void addSubprogram(const Function *F, DISubprogram *SP)
    {
        Subprogram[F] = SP;
    }

    void addDebugLoc(const Instruction *I, DILocation *Loc)
    {
        DebugLoc[I] = Loc;
    }
};

void LineNumberAnnotatedWriter::emitFunctionAnnot(
      const Function *F, formatted_raw_ostream &Out)
{
    InstrLoc = nullptr;
    DISubprogram *FuncLoc = F->getSubprogram();
    if (!FuncLoc) {
        auto SP = Subprogram.find(F);
        if (SP != Subprogram.end())
            FuncLoc = SP->second;
    }
    if (FuncLoc) {
        std::vector<DILineInfo> DIvec(1);
        DILineInfo &DI = DIvec.back();
        DI.FunctionName = FuncLoc->getName().str();
        DI.FileName = FuncLoc->getFilename().str();
        DI.Line = FuncLoc->getLine();
        LinePrinter.emit_lineinfo(Out, DIvec);
    }
}

void LineNumberAnnotatedWriter::emitInstructionAnnot(
      const Instruction *I, formatted_raw_ostream &Out)
{
    const DILocation *NewInstrLoc = I->getDebugLoc();
    if (!NewInstrLoc) {
        auto Loc = DebugLoc.find(I);
        if (Loc != DebugLoc.end())
            NewInstrLoc = Loc->second;
    }
    emitInstructionAnnot(NewInstrLoc, Out);
    Out << LinePrinter.inlining_indent(" ");
}

void LineNumberAnnotatedWriter::emitInstructionAnnot(
      const DILocation *NewInstrLoc, formatted_raw_ostream &Out)
{
    if (NewInstrLoc && NewInstrLoc != InstrLoc) {
        InstrLoc = NewInstrLoc;
        std::vector<DILineInfo> DIvec;
        do {
            DIvec.emplace_back();
            DILineInfo &DI = DIvec.back();
            DIScope *scope = NewInstrLoc->getScope();
            if (scope)
                DI.FunctionName = scope->getName().str();
            DI.FileName = NewInstrLoc->getFilename().str();
            DI.Line = NewInstrLoc->getLine();
            NewInstrLoc = NewInstrLoc->getInlinedAt();
        } while (NewInstrLoc);
        LinePrinter.emit_lineinfo(Out, DIvec);
    }
}

void LineNumberAnnotatedWriter::emitBasicBlockEndAnnot(
        const BasicBlock *BB, formatted_raw_ostream &Out)
{
    if (BB == &BB->getParent()->back())
        emitEnd(Out);
}

static void jl_strip_llvm_debug(Module *m, bool all_meta, LineNumberAnnotatedWriter *AAW)
{
    // strip metadata from all instructions in all functions in the module
    Instruction *deletelast = nullptr; // can't actually delete until the iterator advances
    for (Function &f : m->functions()) {
        if (AAW)
            AAW->addSubprogram(&f, f.getSubprogram());
        for (BasicBlock &f_bb : f) {
            for (Instruction &inst : f_bb) {
                if (deletelast) {
                    deletelast->eraseFromParent();
                    deletelast = nullptr;
                }
                // remove dbg.declare and dbg.value calls
                if (isa<DbgDeclareInst>(inst) || isa<DbgValueInst>(inst)) {
                    deletelast = &inst;
                    continue;
                }

                // iterate over all metadata kinds and set to NULL to remove
                if (all_meta) {
                    SmallVector<std::pair<unsigned, MDNode*>, 4> MDForInst;
                    inst.getAllMetadataOtherThanDebugLoc(MDForInst);
                    for (const auto &md_iter : MDForInst) {
                        inst.setMetadata(md_iter.first, NULL);
                    }
                }
                // record debug location before erasing it
                if (AAW)
                    AAW->addDebugLoc(&inst, inst.getDebugLoc());
                inst.setDebugLoc(DebugLoc());
            }
            if (deletelast) {
                deletelast->eraseFromParent();
                deletelast = nullptr;
            }
        }
        f.setSubprogram(NULL);
    }
    if (all_meta) {
        for (GlobalObject &g : m->global_objects()) {
            g.clearMetadata();
        }
    }
    // now that the subprogram is not referenced, we can delete it too
    if (NamedMDNode *md = m->getNamedMetadata("llvm.dbg.cu"))
        m->eraseNamedMetadata(md);
    //if (NamedMDNode *md = m->getNamedMetadata("llvm.module.flags"))
    //    m->eraseNamedMetadata(md);
}

void jl_strip_llvm_debug(Module *m)
{
    jl_strip_llvm_debug(m, false, NULL);
}

void jl_strip_llvm_addrspaces(Module *m)
{
    legacy::PassManager PM;
    PM.add(createRemoveJuliaAddrspacesPass());
    PM.run(*m);
}

// print an llvm IR acquired from jl_get_llvmf
// warning: this takes ownership of, and destroys, f->getParent()
extern "C" JL_DLLEXPORT
jl_value_t *jl_dump_function_ir_impl(void *f, char strip_ir_metadata, char dump_module, const char *debuginfo)
{
    std::string code;
    raw_string_ostream stream(code);

    {
        Function *llvmf = dyn_cast_or_null<Function>((Function*)f);
        if (!llvmf || (!llvmf->isDeclaration() && !llvmf->getParent()))
            jl_error("jl_dump_function_ir: Expected Function* in a temporary Module");

        JL_LOCK(&jl_codegen_lock); // Might GC
        LineNumberAnnotatedWriter AAW{"; ", false, debuginfo};
        if (!llvmf->getParent()) {
            // print the function declaration as-is
            llvmf->print(stream, &AAW);
            delete llvmf;
        }
        else {
            Module *m = llvmf->getParent();
            if (strip_ir_metadata) {
                std::string llvmfn(llvmf->getName());
                jl_strip_llvm_addrspaces(m);
                jl_strip_llvm_debug(m, true, &AAW);
                // rewriting the function type creates a new function, so look it up again
                llvmf = m->getFunction(llvmfn);
            }
            if (dump_module) {
                m->print(stream, &AAW);
            }
            else {
                llvmf->print(stream, &AAW);
            }
            delete m;
        }
        JL_UNLOCK(&jl_codegen_lock); // Might GC
    }

    return jl_pchar_to_string(stream.str().data(), stream.str().size());
}

static void jl_dump_asm_internal(
        uintptr_t Fptr, size_t Fsize, int64_t slide,
        object::SectionRef Section,
        DIContext *di_ctx,
        raw_ostream &rstream,
        const char* asm_variant,
        const char* debuginfo,
        bool binary);

// This isn't particularly fast, but neither is printing assembly, and they're only used for interactive mode
static uint64_t compute_obj_symsize(object::SectionRef Section, uint64_t offset)
{
    // Scan the object file for the closest symbols above and below offset in the given section
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool setlo = false;
    uint64_t SAddr = Section.getAddress();
    uint64_t SSize = Section.getSize();
    if (offset < SAddr || offset >= SAddr + SSize)
        return 0;
    // test for lower and upper symbol bounds relative to other symbols
    hi = SAddr + SSize;
    for (const object::SymbolRef &Sym : Section.getObject()->symbols()) {
        if (!Section.containsSymbol(Sym))
            continue;
        uint64_t Addr = cantFail(Sym.getAddress());
        if (Addr <= offset && Addr >= lo) {
            // test for lower bound on symbol
            lo = Addr;
            setlo = true;
        }
        if (Addr > offset && Addr < hi) {
            // test for upper bound on symbol
            hi = Addr;
        }
    }
    if (setlo)
        return hi - lo;
    return 0;
}

// print a native disassembly for the function starting at fptr
extern "C" JL_DLLEXPORT
jl_value_t *jl_dump_fptr_asm_impl(uint64_t fptr, char raw_mc, const char* asm_variant, const char *debuginfo, char binary)
{
    assert(fptr != 0);
    std::string code;
    raw_string_ostream stream(code);

    // Find debug info (line numbers) to print alongside
    object::SectionRef Section;
    int64_t slide = 0;
    uint64_t symsize = 0;
    llvm::DIContext *context = NULL;
    if (!jl_DI_for_fptr(fptr, &symsize, &slide, &Section, &context)) {
        if (!jl_dylib_DI_for_fptr(fptr, &Section, &slide, &context,
                    false, NULL, NULL, NULL, NULL)) {
            jl_printf(JL_STDERR, "WARNING: Unable to find function pointer\n");
            return jl_pchar_to_string("", 0);
        }
    }
    if (symsize == 0 && Section.getObject())
        symsize = compute_obj_symsize(Section, fptr + slide);
    if (symsize == 0) {
        jl_printf(JL_STDERR, "WARNING: Could not determine size of symbol\n");
        return jl_pchar_to_string("", 0);
    }

    if (raw_mc) {
        return (jl_value_t*)jl_pchar_to_array((char*)fptr, symsize);
    }

    // Dump assembly code
    jl_ptls_t ptls = jl_current_task->ptls;
    int8_t gc_state = jl_gc_safe_enter(ptls);
    jl_dump_asm_internal(
            fptr, symsize, slide,
            Section, context,
            stream,
            asm_variant,
            debuginfo,
            binary);
    jl_gc_safe_leave(ptls, gc_state);

    return jl_pchar_to_string(stream.str().data(), stream.str().size());
}


namespace {
#define FuncMCView ArrayRef<uint8_t>

// Look up a symbol, and return a const char* to its name when the
// address matches. We currently just use "L<address>" as name for the
// symbol. We could easily get more fancy, e.g. numbering symbols
// sequentially or encoding the line number, but that doesn't seem
// necessary.
class SymbolTable {
    typedef std::map<uint64_t, std::string> TableType;
    TableType Table;
    MCContext& Ctx;
    const FuncMCView &MemObj;
    int Pass;
    const object::ObjectFile *object;
    uint64_t ip; // virtual instruction pointer of the current instruction
    int64_t slide;
public:
    SymbolTable(MCContext &Ctx, const object::ObjectFile *object, int64_t slide, const FuncMCView &MemObj):
        Ctx(Ctx), MemObj(MemObj), object(object), ip(0), slide(slide) {}
    const FuncMCView &getMemoryObject() const { return MemObj; }
    void setPass(int Pass) { this->Pass = Pass; }
    int getPass() const { return Pass; }
    void insertAddress(uint64_t addr);
    // void createSymbol(const char *name, uint64_t addr);
    void createSymbols();
    const char *lookupSymbolName(uint64_t addr);
    MCSymbol *lookupSymbol(uint64_t addr);
    StringRef getSymbolNameAt(uint64_t offset) const;
    const char *lookupLocalPC(size_t addr);
    void setIP(uint64_t addr);
    uint64_t getIP() const;
};

void SymbolTable::setIP(uint64_t addr)
{
    ip = addr;
}
uint64_t SymbolTable::getIP() const
{
    return ip;
}

const char *SymbolTable::lookupLocalPC(size_t addr) {
    jl_frame_t *frame = NULL;
    jl_getFunctionInfo(&frame,
            addr,
            /*skipC*/0,
            /*noInline*/1/* the entry pointer shouldn't have inlining */);
    char *name = frame->func_name; // TODO: free me
    free(frame->file_name);
    free(frame);
    return name;
}

StringRef SymbolTable::getSymbolNameAt(uint64_t offset) const
{
    if (object == NULL)
        return StringRef();
    object::section_iterator ESection = object->section_end();
    for (const object::SymbolRef &Sym : object->symbols()) {
        auto Sect = cantFail(Sym.getSection());
        if (Sect == ESection)
            continue;
        if (Sect->getAddress() == 0)
            continue;
        uint64_t Addr = cantFail(Sym.getAddress());
        if (Addr == offset) {
            auto sNameOrError = Sym.getName();
            if (sNameOrError)
                return sNameOrError.get();
        }
    }
    return StringRef();
}

// Insert an address
void SymbolTable::insertAddress(uint64_t addr)
{
    Table[addr] = "";
}
// Create symbols for all addresses
void SymbolTable::createSymbols()
{
    uintptr_t Fptr = (uintptr_t)MemObj.data();
    uintptr_t Fsize = MemObj.size();
    for (TableType::iterator isymb = Table.begin(), esymb = Table.end();
         isymb != esymb; ++isymb) {
        uintptr_t rel = isymb->first - ip;
        uintptr_t addr = isymb->first;
        if (Fptr <= addr && addr < Fptr + Fsize) {
            std::string name;
            raw_string_ostream(name) << "L" << rel;
            isymb->second = name;
        }
        else {
            const char *global = lookupLocalPC(addr);
            if (global && global[0])
                isymb->second = global;
            // TODO: free(global)?
        }
    }
}

const char *SymbolTable::lookupSymbolName(uint64_t addr)
{
    TableType::iterator Sym;
    bool insertion;
    std::tie(Sym, insertion) = Table.insert(std::make_pair(addr, std::string()));
    if (insertion) {
        // First time we've seen addr: try to look it up
        StringRef local_name = getSymbolNameAt(addr + slide);
        if (local_name.empty()) {
            const char *global = lookupLocalPC(addr);
            if (global) {
                //std::string name;
                //raw_string_ostream(name) << global << "@0x" << std::hex
                //     << std::setfill('0') << std::setw(2 * sizeof(void*))
                //     << addr;
                //Sym->second = name.str();
                Sym->second = global;
            }
        }
        else {
            Sym->second = local_name.str();
        }
    }
    return Sym->second.empty() ? NULL : Sym->second.c_str();
}

MCSymbol *SymbolTable::lookupSymbol(uint64_t addr)
{
    TableType::iterator Sym = Table.find(addr);
    if (Sym == Table.end() || Sym->second.empty())
        return NULL;
    MCSymbol *symb = Ctx.getOrCreateSymbol(Sym->second);
    assert(symb->isUndefined());
    return symb;
}

static const char *SymbolLookup(void *DisInfo, uint64_t ReferenceValue, uint64_t *ReferenceType,
                                uint64_t ReferencePC, const char **ReferenceName)
{
    uint64_t RTypeIn = *ReferenceType;
    SymbolTable *SymTab = (SymbolTable*)DisInfo;
    *ReferenceType = LLVMDisassembler_ReferenceType_InOut_None;
    *ReferenceName = NULL;
    if (SymTab->getPass() != 0) {
        if (RTypeIn == LLVMDisassembler_ReferenceType_In_Branch) {
            uint64_t addr = ReferenceValue + SymTab->getIP(); // probably pc-rel
            const char *symbolName = SymTab->lookupSymbolName(addr);
            return symbolName;
        }
        else if (RTypeIn == LLVMDisassembler_ReferenceType_In_PCrel_Load) {
            uint64_t addr = ReferenceValue + SymTab->getIP();
            const char *symbolName = SymTab->lookupSymbolName(addr);
            if (symbolName) {
                *ReferenceType = LLVMDisassembler_ReferenceType_Out_LitPool_SymAddr;
                *ReferenceName = symbolName;
            }
        }
        else if (RTypeIn == LLVMDisassembler_ReferenceType_InOut_None) {
            uint64_t addr = ReferenceValue; // probably not pc-rel
            const char *symbolName = SymTab->lookupSymbolName(addr);
            return symbolName;
        }
    }
    return NULL;
}

static int OpInfoLookup(void *DisInfo, uint64_t PC, uint64_t Offset, uint64_t Size,
                        int TagType, void *TagBuf)
{
    SymbolTable *SymTab = (SymbolTable*)DisInfo;
    LLVMOpInfo1 *info = (LLVMOpInfo1*)TagBuf;
    memset(info, 0, sizeof(*info));
    if (TagType != 1)
        return 0;               // Unknown data format
    PC += SymTab->getIP() - (uint64_t)(uintptr_t)SymTab->getMemoryObject().data(); // add offset from MemoryObject base
    // TODO: see if we knew of a relocation applied at PC
    // info->AddSymbol.Present = 1;
    // info->AddSymbol.Name = name;
    // info->AddSymbol.Value = pointer; // unused by LLVM
    // info->Value = 0;                 // offset
    // return 1;                        // Success
    return 0;
}
} // namespace

// Stringify raw bytes as a comment string.
std::string rawCodeComment(const llvm::ArrayRef<uint8_t>& Memory, const llvm::Triple& Triple)
{
    std::string Buffer{"; "};
    llvm::raw_string_ostream Stream{Buffer};
    auto Address = reinterpret_cast<uintptr_t>(Memory.data());
    // write abbreviated address
    llvm::write_hex(Stream, Address & 0xffff, HexPrintStyle::Lower, 4);
    Stream << ":";
    auto Arch = Triple.getArch();
    bool FixedLength = !(Arch == Triple::x86 || Arch == Triple::x86_64);
    if (FixedLength)
        Stream << " ";
    if (FixedLength && Triple.isLittleEndian()) {
        for (auto Iter = Memory.rbegin(); Iter != Memory.rend(); ++Iter)
            llvm::write_hex(Stream, *Iter, HexPrintStyle::Lower, 2);
    }
    else {
        // variable-length or (fixed-length) big-endian format
        for (auto Byte : Memory) {
            if (!FixedLength)
                Stream << " ";
            llvm::write_hex(Stream, Byte, HexPrintStyle::Lower, 2);
        }
    }
    return Stream.str();
}

static void jl_dump_asm_internal(
        uintptr_t Fptr, size_t Fsize, int64_t slide,
        object::SectionRef Section,
        DIContext *di_ctx,
        raw_ostream &rstream,
        const char* asm_variant,
        const char* debuginfo,
        bool binary)
{
    // GC safe
    // Get the host information
    Triple TheTriple(sys::getProcessTriple());

    const auto &target = jl_get_llvm_disasm_target();
    const auto &cpu = target.first;
    const auto &features = target.second;

    std::string err;
    const Target *TheTarget = TargetRegistry::lookupTarget(TheTriple.str(), err);

    // Set up required helpers and streamer
    SourceMgr SrcMgr;

    MCTargetOptions Options;
    std::unique_ptr<MCAsmInfo> MAI(
        TheTarget->createMCAsmInfo(*TheTarget->createMCRegInfo(TheTriple.str()), TheTriple.str(), Options));
    assert(MAI && "Unable to create target asm info!");

    std::unique_ptr<MCRegisterInfo> MRI(TheTarget->createMCRegInfo(TheTriple.str()));
    assert(MRI && "Unable to create target register info!");

    std::unique_ptr<llvm::MCSubtargetInfo> STI(
      TheTarget->createMCSubtargetInfo(TheTriple.str(), cpu, features));
    assert(STI && "Unable to create subtarget info!");

#if JL_LLVM_VERSION >= 130000
    MCContext Ctx(TheTriple, MAI.get(), MRI.get(), STI.get(), &SrcMgr);
    std::unique_ptr<MCObjectFileInfo> MOFI(
      TheTarget->createMCObjectFileInfo(Ctx, /*PIC=*/false, /*LargeCodeModel=*/ false));
    Ctx.setObjectFileInfo(MOFI.get());
#else
    std::unique_ptr<MCObjectFileInfo> MOFI(new MCObjectFileInfo());
    MCContext Ctx(MAI.get(), MRI.get(), MOFI.get(), &SrcMgr);
    MOFI->InitMCObjectFileInfo(TheTriple, /* PIC */ false, Ctx);
#endif

    std::unique_ptr<MCDisassembler> DisAsm(TheTarget->createMCDisassembler(*STI, Ctx));
    if (!DisAsm) {
        rstream << "ERROR: no disassembler for target " << TheTriple.str();
        return;
    }
    unsigned OutputAsmVariant = 0; // ATT or Intel-style assembly

    if (strcmp(asm_variant, "intel") == 0) {
        OutputAsmVariant = 1;
    }
    bool ShowEncoding = false;

    std::unique_ptr<MCInstrInfo> MCII(
            TheTarget->createMCInstrInfo());
    std::unique_ptr<MCInstrAnalysis> MCIA(
            TheTarget->createMCInstrAnalysis(MCII.get()));
    std::unique_ptr<MCInstPrinter> IP(
            TheTarget->createMCInstPrinter(TheTriple, OutputAsmVariant, *MAI, *MCII, *MRI));
    //IP->setPrintImmHex(true); // prefer hex or decimal immediates
    std::unique_ptr<MCCodeEmitter> CE;
    std::unique_ptr<MCAsmBackend> MAB;
    if (ShowEncoding) {
        CE.reset(TheTarget->createMCCodeEmitter(*MCII, *MRI, Ctx));
        MAB.reset(TheTarget->createMCAsmBackend(*STI, *MRI, Options));
    }

    // createAsmStreamer expects a unique_ptr to a formatted stream, which means
    // it will destruct the stream when it is done. We cannot have this, so we
    // start out with a raw stream, and create formatted stream from it here.
    // LLVM will destroy the formatted stream, and we keep the raw stream.
    std::unique_ptr<formatted_raw_ostream> ustream(new formatted_raw_ostream(rstream));
    std::unique_ptr<MCStreamer> Streamer(
            TheTarget->createAsmStreamer(Ctx, std::move(ustream), /*asmverbose*/true,
                                         /*useDwarfDirectory*/ true,
                                         IP.release(),
                                         std::move(CE), std::move(MAB),
                                         /*ShowInst*/ false));
    Streamer->InitSections(true);

    // Make the MemoryObject wrapper
    ArrayRef<uint8_t> memoryObject(const_cast<uint8_t*>((const uint8_t*)Fptr),Fsize);
    SymbolTable DisInfo(Ctx, Section.getObject(), slide, memoryObject);

    DILineInfoTable di_lineinfo;
    if (di_ctx)
        di_lineinfo = di_ctx->getLineInfoForAddressRange(makeAddress(Section, Fptr + slide), Fsize);
    if (!di_lineinfo.empty()) {
        auto cur_addr = di_lineinfo[0].first;
        auto nlineinfo = di_lineinfo.size();
        // filter out line infos that doesn't contain any instructions
        unsigned j = 0;
        for (unsigned i = 1; i < nlineinfo; i++) {
            auto &info = di_lineinfo[i];
            if (info.first != cur_addr)
                j++;
            cur_addr = info.first;
            if (i != j) {
                di_lineinfo[j] = std::move(info);
            }
        }
        if (j + 1 < nlineinfo) {
            di_lineinfo.resize(j + 1);
        }
    }

    if (binary) {
        // Print the complete address and the size at the top (instruction addresses are abbreviated)
        std::string Buffer{"; code origin: "};
        llvm::raw_string_ostream Stream{Buffer};
        auto Address = reinterpret_cast<uintptr_t>(memoryObject.data());
        llvm::write_hex(Stream, Address, HexPrintStyle::Lower, 16);
        Stream << ", code size: " << memoryObject.size();
        Streamer->emitRawText(Stream.str());
    }

    // Take two passes: In the first pass we record all branch labels,
    // in the second we actually perform the output
    for (int pass = 0; pass < 2; ++ pass) {
        DisInfo.setPass(pass);
        if (pass != 0) {
            // Switch to symbolic disassembly. We cannot do this
            // before the first pass, because this changes branch
            // targets from immediate values (constants) to
            // expressions, which are not handled correctly by
            // MCIA->evaluateBranch. (It should be possible to rewrite
            // this routine to handle this case correctly as well.)
            // Could add OpInfoLookup here
            DisAsm->setSymbolizer(std::unique_ptr<MCSymbolizer>(new MCExternalSymbolizer(
                        Ctx,
                        std::unique_ptr<MCRelocationInfo>(new MCRelocationInfo(Ctx)),
                        OpInfoLookup,
                        SymbolLookup,
                        &DisInfo)));
        }

        uint64_t nextLineAddr = -1;
        DILineInfoTable::iterator di_lineIter = di_lineinfo.begin();
        DILineInfoTable::iterator di_lineEnd = di_lineinfo.end();
        DILineInfoPrinter dbgctx{"; ", true};
        dbgctx.SetVerbosity(debuginfo);
        if (pass != 0) {
            if (di_ctx && di_lineIter != di_lineEnd) {
                // Set up the line info
                nextLineAddr = di_lineIter->first;
                if (nextLineAddr != (uint64_t)(Fptr + slide)) {
                    std::string buf;
                    dbgctx.emit_lineinfo(buf, di_lineIter->second);
                    if (!buf.empty()) {
                        Streamer->emitRawText(buf);
                    }
                }
            }
        }

        uint64_t Index = 0;
        uint64_t insSize = 0;

        // Do the disassembly
        for (Index = 0; Index < Fsize; Index += insSize) {

            if (pass != 0 && nextLineAddr != (uint64_t)-1 && Index + Fptr + slide == nextLineAddr) {
                if (di_ctx) {
                    std::string buf;
                    DILineInfoSpecifier infoSpec(
                        DILineInfoSpecifier::FileLineInfoKind::RawValue,
                        DILineInfoSpecifier::FunctionNameKind::ShortName);
                    DIInliningInfo dbg = di_ctx->getInliningInfoForAddress(makeAddress(Section, Index + Fptr + slide), infoSpec);
                    if (dbg.getNumberOfFrames()) {
                        dbgctx.emit_lineinfo(buf, dbg);
                    }
                    else {
                        dbgctx.emit_lineinfo(buf, di_lineIter->second);
                    }
                    if (!buf.empty()) {
                        Streamer->emitRawText(buf);
                    }
                    nextLineAddr = (++di_lineIter)->first;
                }
            }

            DisInfo.setIP(Fptr+Index);
            if (pass != 0) {
                // Uncomment this to output addresses for all instructions
                // stream << Index << ": ";
                MCSymbol *symbol = DisInfo.lookupSymbol(Fptr+Index);
                if (symbol) {
                    Streamer->emitLabel(symbol);
                }
            }

            MCInst Inst;
            MCDisassembler::DecodeStatus S;
            FuncMCView view = memoryObject.slice(Index);
            S = DisAsm->getInstruction(Inst, insSize, view, 0,
                                      /*CStream*/ pass != 0 ? Streamer->GetCommentOS() : nulls());
            if (pass != 0 && Streamer->GetCommentOS().tell() > 0)
                Streamer->GetCommentOS() << '\n';
            switch (S) {
            case MCDisassembler::Fail:
                if (insSize == 0) // skip illegible bytes
#if defined(_CPU_PPC_) || defined(_CPU_PPC64_) || defined(_CPU_ARM_) || defined(_CPU_AARCH64_)
                    insSize = 4; // instructions are always 4 bytes
#else
                    insSize = 1; // attempt to slide 1 byte forward
#endif
                if (pass != 0) {
                    std::string _buf;
                    raw_string_ostream buf(_buf);
                    if (insSize == 4) {
                        buf << "\t.long\t";
                        llvm::write_hex(buf, *(uint32_t*)(Fptr + Index), HexPrintStyle::PrefixLower, 8);
                    }
                    else {
                        for (uint64_t i = 0; i < insSize; ++i) {
                            buf << "\t.byte\t";
                            llvm::write_hex(buf, *(uint8_t*)(Fptr + Index + i), HexPrintStyle::PrefixLower, 2);
                        }
                    }
                    Streamer->emitRawText(StringRef(buf.str()));
                }
                break;

            case MCDisassembler::SoftFail:
                if (pass != 0) {
                    Streamer->emitRawText(StringRef("potentially undefined instruction encoding:"));
                }
                // Fall through

            case MCDisassembler::Success:
                if (pass == 0) {
                    // Pass 0: Record all branch target references
                    if (MCIA) {
                        const MCInstrDesc &opcode = MCII->get(Inst.getOpcode());
                        if (opcode.isBranch() || opcode.isCall()) {
                            uint64_t addr;
                            if (MCIA->evaluateBranch(Inst, Fptr + Index, insSize, addr))
                                DisInfo.insertAddress(addr);
                        }
                    }
                }
                else {
                    // Pass 1: Output instruction
                    if (pass != 0) {
                        // attempt to symbolicate any immediate operands
                        const MCInstrDesc &opinfo = MCII->get(Inst.getOpcode());
                        for (unsigned Op = 0; Op < opinfo.NumOperands; Op++) {
                            const MCOperand &OpI = Inst.getOperand(Op);
                            if (OpI.isImm()) {
                                int64_t imm = OpI.getImm();
                                if (opinfo.OpInfo[Op].OperandType == MCOI::OPERAND_PCREL)
                                    imm += Fptr + Index;
                                const char *name = DisInfo.lookupSymbolName(imm);
                                if (name)
                                    Streamer->AddComment(name);
                            }
                        }
                    }
                    if (binary)
                        Streamer->emitRawText(rawCodeComment(memoryObject.slice(Index, insSize), TheTriple));
                    Streamer->emitInstruction(Inst, *STI);
                }
                break;
            }
        }

        DisInfo.setIP(Fptr);
        if (pass == 0)
            DisInfo.createSymbols();

        if (pass != 0 && di_ctx) {
            std::string buf;
            dbgctx.emit_finish(buf);
            if (!buf.empty()) {
                Streamer->emitRawText(buf);
            }
        }
    }
}

/// addPassesToX helper drives creation and initialization of TargetPassConfig.
static MCContext *
addPassesToGenerateCode(LLVMTargetMachine *TM, PassManagerBase &PM) {
    TargetPassConfig *PassConfig = TM->createPassConfig(PM);
    PassConfig->setDisableVerify(false);
    PM.add(PassConfig);
    MachineModuleInfoWrapperPass *MMIWP =
        new MachineModuleInfoWrapperPass(TM);
    PM.add(MMIWP);
    if (PassConfig->addISelPasses())
        return NULL;
    PassConfig->addMachinePasses();
    PassConfig->setInitialized();
    return &MMIWP->getMMI().getContext();
}

class LineNumberPrinterHandler : public AsmPrinterHandler {
    MCStreamer &S;
    LineNumberAnnotatedWriter LinePrinter;
    std::string Buffer;
    llvm::raw_string_ostream RawStream;
    llvm::formatted_raw_ostream Stream;

public:
    LineNumberPrinterHandler(AsmPrinter &Printer, const char *debuginfo)
        : S(*Printer.OutStreamer),
          LinePrinter("; ", true, debuginfo),
          RawStream(Buffer),
          Stream(RawStream) {}

    void emitAndReset() {
        Stream.flush();
        RawStream.flush();
        if (Buffer.empty())
            return;
        S.emitRawText(Buffer);
        Buffer.clear();
    }

    virtual void setSymbolSize(const MCSymbol *Sym, uint64_t Size) override {}
    //virtual void beginModule(Module *M) override {}
    virtual void endModule() override {}
    /// note that some AsmPrinter implementations may not call beginFunction at all
    virtual void beginFunction(const MachineFunction *MF) override {
        LinePrinter.emitFunctionAnnot(&MF->getFunction(), Stream);
        emitAndReset();
    }
    //virtual void markFunctionEnd() override {}
    virtual void endFunction(const MachineFunction *MF) override {
        LinePrinter.emitEnd(Stream);
        emitAndReset();
    }
    //virtual void beginFragment(const MachineBasicBlock *MBB,
    //                           ExceptionSymbolProvider ESP) override {}
    //virtual void endFragment() override {}
    //virtual void beginFunclet(const MachineBasicBlock &MBB,
    //                          MCSymbol *Sym = nullptr) override {}
    //virtual void endFunclet() override {}
    virtual void beginInstruction(const MachineInstr *MI) override {
        LinePrinter.emitInstructionAnnot(MI->getDebugLoc(), Stream);
        emitAndReset();
    }
    virtual void endInstruction() override {}
};

// get a native assembly for llvm::Function
extern "C" JL_DLLEXPORT
jl_value_t *jl_dump_function_asm_impl(void *F, char raw_mc, const char* asm_variant, const char *debuginfo, char binary)
{
    // precise printing via IR assembler
    SmallVector<char, 4096> ObjBufferSV;
    { // scope block
        Function *f = (Function*)F;
        llvm::raw_svector_ostream asmfile(ObjBufferSV);
        assert(!f->isDeclaration());
        std::unique_ptr<Module> m(f->getParent());
        for (auto &f2 : m->functions()) {
            if (f != &f2 && !f->isDeclaration())
                f2.deleteBody();
        }
        LLVMTargetMachine *TM = static_cast<LLVMTargetMachine*>(jl_TargetMachine);
        legacy::PassManager PM;
        addTargetPasses(&PM, TM);
        if (raw_mc) {
            raw_svector_ostream obj_OS(ObjBufferSV);
            if (TM->addPassesToEmitFile(PM, obj_OS, nullptr, CGFT_ObjectFile, false, nullptr))
                return jl_an_empty_string;
            PM.run(*m);
        }
        else {
            MCContext *Context = addPassesToGenerateCode(TM, PM);
            if (!Context)
                return jl_an_empty_string;
            Context->setGenDwarfForAssembly(false);
            // Duplicate LLVMTargetMachine::addAsmPrinter here so we can set the asm dialect and add the custom annotation printer
            const MCSubtargetInfo &STI = *TM->getMCSubtargetInfo();
            const MCAsmInfo &MAI = *TM->getMCAsmInfo();
            const MCRegisterInfo &MRI = *TM->getMCRegisterInfo();
            const MCInstrInfo &MII = *TM->getMCInstrInfo();
            unsigned OutputAsmDialect = MAI.getAssemblerDialect();
            if (!strcmp(asm_variant, "att"))
                OutputAsmDialect = 0;
            if (!strcmp(asm_variant, "intel"))
                OutputAsmDialect = 1;
            MCInstPrinter *InstPrinter = TM->getTarget().createMCInstPrinter(
                TM->getTargetTriple(), OutputAsmDialect, MAI, MII, MRI);
             std::unique_ptr<MCAsmBackend> MAB(TM->getTarget().createMCAsmBackend(
                STI, MRI, TM->Options.MCOptions));
            std::unique_ptr<MCCodeEmitter> MCE;
            if (binary) // enable MCAsmStreamer::AddEncodingComment printing
                MCE.reset(TM->getTarget().createMCCodeEmitter(MII, MRI, *Context));
            auto FOut = std::make_unique<formatted_raw_ostream>(asmfile);
            std::unique_ptr<MCStreamer> S(TM->getTarget().createAsmStreamer(
                *Context, std::move(FOut), true,
                true, InstPrinter,
                std::move(MCE), std::move(MAB),
                false));
            std::unique_ptr<AsmPrinter> Printer(
                TM->getTarget().createAsmPrinter(*TM, std::move(S)));
            Printer->addAsmPrinterHandler(AsmPrinter::HandlerInfo(
                        std::unique_ptr<AsmPrinterHandler>(new LineNumberPrinterHandler(*Printer, debuginfo)),
                        "emit", "Debug Info Emission", "Julia", "Julia::LineNumberPrinterHandler Markup"));
            if (!Printer)
                return jl_an_empty_string;
            PM.add(Printer.release());
            PM.add(createFreeMachineFunctionPass());
            PM.run(*m);
        }
    }
    return jl_pchar_to_string(ObjBufferSV.data(), ObjBufferSV.size());
}

extern "C" JL_DLLEXPORT
LLVMDisasmContextRef jl_LLVMCreateDisasm_impl(
        const char *TripleName, void *DisInfo, int TagType,
        LLVMOpInfoCallback GetOpInfo, LLVMSymbolLookupCallback SymbolLookUp)
{
    return LLVMCreateDisasm(TripleName, DisInfo, TagType, GetOpInfo, SymbolLookUp);
}

extern "C" JL_DLLEXPORT
JL_DLLEXPORT size_t jl_LLVMDisasmInstruction_impl(
        LLVMDisasmContextRef DC, uint8_t *Bytes, uint64_t BytesSize,
        uint64_t PC, char *OutString, size_t OutStringSize)
{
    return LLVMDisasmInstruction(DC, Bytes, BytesSize, PC, OutString, OutStringSize);
}
