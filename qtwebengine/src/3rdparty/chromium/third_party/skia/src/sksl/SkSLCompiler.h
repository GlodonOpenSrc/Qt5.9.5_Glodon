/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
 
#ifndef SKSL_COMPILER
#define SKSL_COMPILER

#include <set>
#include <vector>
#include "ir/SkSLProgram.h"
#include "ir/SkSLSymbolTable.h"
#include "SkSLCFGGenerator.h"
#include "SkSLContext.h"
#include "SkSLErrorReporter.h"
#include "SkSLGLSLCodeGenerator.h"

#define SK_FRAGCOLOR_BUILTIN 10001

namespace SkSL {

class IRGenerator;

/**
 * Main compiler entry point. This is a traditional compiler design which first parses the .sksl
 * file into an abstract syntax tree (a tree of ASTNodes), then performs semantic analysis to 
 * produce a Program (a tree of IRNodes), then feeds the Program into a CodeGenerator to produce
 * compiled output.
 *
 * See the README for information about SkSL.
 */
class Compiler : public ErrorReporter {
public:
    Compiler();

    ~Compiler();

    std::unique_ptr<Program> convertProgram(Program::Kind kind, std::string text);

    bool toSPIRV(Program::Kind kind, const std::string& text, std::ostream& out);
    
    bool toSPIRV(Program::Kind kind, const std::string& text, std::string* out);

    bool toGLSL(Program::Kind kind, const std::string& text, const GrGLSLCaps& caps,
                std::ostream& out);
    
    bool toGLSL(Program::Kind kind, const std::string& text, const GrGLSLCaps& caps,
                std::string* out);

    void error(Position position, std::string msg) override;

    std::string errorText();

    void writeErrorCount();

private:
    void addDefinition(const Expression* lvalue, const Expression* expr,
                       std::unordered_map<const Variable*, const Expression*>* definitions);
 
    void addDefinitions(const BasicBlock::Node& node, 
                        std::unordered_map<const Variable*, const Expression*>* definitions);

    void scanCFG(CFG* cfg, BlockId block, std::set<BlockId>* workList);

    void scanCFG(const FunctionDefinition& f);

    void internalConvertProgram(std::string text,
                                Modifiers::Flag* defaultPrecision,
                                std::vector<std::unique_ptr<ProgramElement>>* result);

    std::shared_ptr<SymbolTable> fTypes;
    IRGenerator* fIRGenerator;
    std::string fSkiaVertText; // FIXME store parsed version instead

    Context fContext;
    int fErrorCount;
    std::string fErrorText;
};

} // namespace

#endif
