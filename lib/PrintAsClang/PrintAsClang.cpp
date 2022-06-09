//===--- PrintAsClang.cpp - Emit a header file for a Swift AST ------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/PrintAsClang/PrintAsClang.h"

#include "ModuleContentsWriter.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/Module.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/Basic/Version.h"
#include "swift/ClangImporter/ClangImporter.h"

#include "clang/Basic/Module.h"

#include "llvm/Support/raw_ostream.h"

using namespace swift;

static void emitCxxConditional(raw_ostream &out,
                               llvm::function_ref<void()> cxxCase,
                               llvm::function_ref<void()> cCase = {}) {
  out << "#if defined(__cplusplus)\n";
  cxxCase();
  if (cCase) {
    out << "#else\n";
    cCase();
  }
  out << "#endif\n";
}

static void emitObjCConditional(raw_ostream &out,
                                llvm::function_ref<void()> objcCase,
                                llvm::function_ref<void()> nonObjCCase = {}) {
  out << "#if defined(__OBJC__)\n";
  objcCase();
  if (nonObjCCase) {
    out << "#else\n";
    nonObjCCase();
  }
  out << "#endif\n";
}

static void writePrologue(raw_ostream &out, ASTContext &ctx,
                          StringRef macroGuard) {

  out << "// Generated by "
      << version::getSwiftFullVersion(ctx.LangOpts.EffectiveLanguageVersion)
      << "\n"
      // Guard against recursive definition.
      << "#ifndef " << macroGuard << "\n"
      << "#define " << macroGuard
      << "\n"
         "#pragma clang diagnostic push\n"
         "#pragma clang diagnostic ignored \"-Wgcc-compat\"\n"
         "\n"
         "#if !defined(__has_include)\n"
         "# define __has_include(x) 0\n"
         "#endif\n"
         "#if !defined(__has_attribute)\n"
         "# define __has_attribute(x) 0\n"
         "#endif\n"
         "#if !defined(__has_feature)\n"
         "# define __has_feature(x) 0\n"
         "#endif\n"
         "#if !defined(__has_warning)\n"
         "# define __has_warning(x) 0\n"
         "#endif\n"
         "\n"
         "#if __has_include(<swift/objc-prologue.h>)\n"
         "# include <swift/objc-prologue.h>\n"
         "#endif\n"
         "\n"
         "#pragma clang diagnostic ignored \"-Wauto-import\"\n";
  emitObjCConditional(out,
                      [&] { out << "#include <Foundation/Foundation.h>\n"; });
  emitCxxConditional(
      out,
      [&] {
        out << "#include <cstdint>\n"
               "#include <cstddef>\n"
               "#include <cstdbool>\n";
      },
      [&] {
        out << "#include <stdint.h>\n"
               "#include <stddef.h>\n"
               "#include <stdbool.h>\n";
      });
  out << "\n"
         "#if !defined(SWIFT_TYPEDEFS)\n"
         "# define SWIFT_TYPEDEFS 1\n"
         "# if __has_include(<uchar.h>)\n"
         "#  include <uchar.h>\n"
         "# elif !defined(__cplusplus)\n"
         "typedef uint_least16_t char16_t;\n"
         "typedef uint_least32_t char32_t;\n"
         "# endif\n"
#define MAP_SIMD_TYPE(C_TYPE, SCALAR_TYPE, _) \
         "typedef " #SCALAR_TYPE " swift_" #C_TYPE "2"       \
         "  __attribute__((__ext_vector_type__(2)));\n" \
         "typedef " #SCALAR_TYPE " swift_" #C_TYPE "3"       \
         "  __attribute__((__ext_vector_type__(3)));\n" \
         "typedef " #SCALAR_TYPE " swift_" #C_TYPE "4"       \
         "  __attribute__((__ext_vector_type__(4)));\n"
#include "swift/ClangImporter/SIMDMappedTypes.def"
         "#endif\n"
         "\n";

#define CLANG_MACRO_BODY(NAME, BODY) \
  out << "#if !defined(" #NAME ")\n" \
         BODY "\n" \
         "#endif\n";

#define CLANG_MACRO(NAME, ARGS, VALUE) CLANG_MACRO_BODY(NAME, "# define " #NAME #ARGS " " #VALUE)

#define CLANG_MACRO_ALTERNATIVE(NAME, ARGS, CONDITION, VALUE, ALTERNATIVE) CLANG_MACRO_BODY(NAME, \
  "# if " #CONDITION "\n" \
  "#  define " #NAME #ARGS " " #VALUE "\n" \
  "# else\n" \
  "#  define " #NAME #ARGS " " #ALTERNATIVE "\n" \
  "# endif")

#define CLANG_MACRO_OBJC(NAME, ARGS, VALUE) \
  out << "#if defined(__OBJC__)\n" \
         "#if !defined(" #NAME ")\n" \
         "# define " #NAME #ARGS " " #VALUE "\n" \
         "#endif\n" \
         "#endif\n";

#define CLANG_MACRO_CXX(NAME, ARGS, VALUE, ALTERNATIVE) \
  out << "#if defined(__cplusplus)\n" \
         "# define " #NAME #ARGS " " #VALUE "\n" \
         "#else\n" \
         "# define " #NAME #ARGS " " #ALTERNATIVE "\n" \
         "#endif\n";

#define CLANG_MACRO_CXX_BODY(NAME, BODY) \
  out << "#if defined(__cplusplus)\n" \
         BODY "\n" \
         "#endif\n";

#include "swift/PrintAsClang/ClangMacros.def"

  static_assert(SWIFT_MAX_IMPORTED_SIMD_ELEMENTS == 4,
              "need to add SIMD typedefs here if max elements is increased");
}

static int compareImportModulesByName(const ImportModuleTy *left,
                                      const ImportModuleTy *right) {
  auto *leftSwiftModule = left->dyn_cast<ModuleDecl *>();
  auto *rightSwiftModule = right->dyn_cast<ModuleDecl *>();

  if (leftSwiftModule && !rightSwiftModule)
    return -compareImportModulesByName(right, left);

  if (leftSwiftModule && rightSwiftModule)
    return leftSwiftModule->getName().compare(rightSwiftModule->getName());

  auto *leftClangModule = left->get<const clang::Module *>();
  assert(leftClangModule->isSubModule() &&
         "top-level modules should use a normal swift::ModuleDecl");
  if (rightSwiftModule) {
    // Because the Clang module is a submodule, its full name will never be
    // equal to a Swift module's name, even if the top-level name is the same;
    // it will always come before or after.
    if (leftClangModule->getTopLevelModuleName() <
        rightSwiftModule->getName().str()) {
      return -1;
    }
    return 1;
  }

  auto *rightClangModule = right->get<const clang::Module *>();
  assert(rightClangModule->isSubModule() &&
         "top-level modules should use a normal swift::ModuleDecl");

  SmallVector<StringRef, 8> leftReversePath(
      ModuleDecl::ReverseFullNameIterator(leftClangModule), {});
  SmallVector<StringRef, 8> rightReversePath(
      ModuleDecl::ReverseFullNameIterator(rightClangModule), {});

  assert(leftReversePath != rightReversePath &&
         "distinct Clang modules should not have the same full name");
  if (std::lexicographical_compare(leftReversePath.rbegin(),
                                   leftReversePath.rend(),
                                   rightReversePath.rbegin(),
                                   rightReversePath.rend())) {
    return -1;
  }
  return 1;
}

static void writeImports(raw_ostream &out,
                         llvm::SmallPtrSetImpl<ImportModuleTy> &imports,
                         ModuleDecl &M, StringRef bridgingHeader) {
  out << "#if __has_feature(modules)\n";

  out << "#if __has_warning(\"-Watimport-in-framework-header\")\n"
      << "#pragma clang diagnostic ignored \"-Watimport-in-framework-header\"\n"
      << "#endif\n";

  // Sort alphabetically for determinism and consistency.
  SmallVector<ImportModuleTy, 8> sortedImports{imports.begin(),
                                               imports.end()};
  llvm::array_pod_sort(sortedImports.begin(), sortedImports.end(),
                       &compareImportModulesByName);

  auto isUnderlyingModule = [&M, bridgingHeader](ModuleDecl *import) -> bool {
    if (bridgingHeader.empty())
      return import != &M && import->getName() == M.getName();

    auto importer = static_cast<ClangImporter *>(
        import->getASTContext().getClangModuleLoader());
    return import == importer->getImportedHeaderModule();
  };

  // Track printed names to handle overlay modules.
  llvm::SmallPtrSet<Identifier, 8> seenImports;
  bool includeUnderlying = false;
  for (auto import : sortedImports) {
    if (auto *swiftModule = import.dyn_cast<ModuleDecl *>()) {
      auto Name = swiftModule->getName();
      if (isUnderlyingModule(swiftModule)) {
        includeUnderlying = true;
        continue;
      }
      if (seenImports.insert(Name).second)
        out << "@import " << Name.str() << ";\n";
    } else {
      const auto *clangModule = import.get<const clang::Module *>();
      assert(clangModule->isSubModule() &&
             "top-level modules should use a normal swift::ModuleDecl");
      out << "@import ";
      ModuleDecl::ReverseFullNameIterator(clangModule).printForward(out);
      out << ";\n";
    }
  }

  out << "#endif\n\n";

  if (includeUnderlying) {
    if (bridgingHeader.empty())
      out << "#import <" << M.getName().str() << '/' << M.getName().str()
          << ".h>\n\n";
    else
      out << "#import \"" << bridgingHeader << "\"\n\n";
  }
}

static void writePostImportPrologue(raw_ostream &os, ModuleDecl &M) {
  os << "#pragma clang diagnostic ignored \"-Wproperty-attribute-mismatch\"\n"
        "#pragma clang diagnostic ignored \"-Wduplicate-method-arg\"\n"
        "#if __has_warning(\"-Wpragma-clang-attribute\")\n"
        "# pragma clang diagnostic ignored \"-Wpragma-clang-attribute\"\n"
        "#endif\n"
        "#pragma clang diagnostic ignored \"-Wunknown-pragmas\"\n"
        "#pragma clang diagnostic ignored \"-Wnullability\"\n"
        "#pragma clang diagnostic ignored "
        "\"-Wdollar-in-identifier-extension\"\n"
        "\n"
        "#if __has_attribute(external_source_symbol)\n"
        "# pragma push_macro(\"any\")\n"
        "# undef any\n"
        "# pragma clang attribute push("
        "__attribute__((external_source_symbol(language=\"Swift\", "
        "defined_in=\""
     << M.getNameStr()
     << "\",generated_declaration))), "
        "apply_to=any(function,enum,objc_interface,objc_category,"
        "objc_protocol))\n"
        "# pragma pop_macro(\"any\")\n"
        "#endif\n\n";
}

static void writeEpilogue(raw_ostream &os) {
  os <<
      "#if __has_attribute(external_source_symbol)\n"
      "# pragma clang attribute pop\n"
      "#endif\n"
      "#pragma clang diagnostic pop\n"
      // For the macro guard against recursive definition
      "#endif\n";
}

static std::string computeMacroGuard(const ModuleDecl *M) {
  return (llvm::Twine(M->getNameStr().upper()) + "_SWIFT_H").str();
}

static std::string getModuleContentsCxxString(ModuleDecl &M) {
  SmallPtrSet<ImportModuleTy, 8> imports;
  std::string moduleContentsBuf;
  llvm::raw_string_ostream moduleContents{moduleContentsBuf};
  printModuleContentsAsCxx(moduleContents, imports, M);
  return moduleContents.str();
}

bool swift::printAsClangHeader(raw_ostream &os, ModuleDecl *M,
                               StringRef bridgingHeader,
                               bool ExposePublicDeclsInClangHeader) {
  llvm::PrettyStackTraceString trace("While generating Clang header");

  SmallPtrSet<ImportModuleTy, 8> imports;
  std::string objcModuleContentsBuf;
  llvm::raw_string_ostream objcModuleContents{objcModuleContentsBuf};
  printModuleContentsAsObjC(objcModuleContents, imports, *M);
  writePrologue(os, M->getASTContext(), computeMacroGuard(M));
  emitObjCConditional(os,
                      [&] { writeImports(os, imports, *M, bridgingHeader); });
  writePostImportPrologue(os, *M);
  emitObjCConditional(os, [&] { os << objcModuleContents.str(); });
  emitCxxConditional(os, [&] {
    // FIXME: Expose Swift with @expose by default.
    if (ExposePublicDeclsInClangHeader) {
      os << getModuleContentsCxxString(*M);
    }
  });
  writeEpilogue(os);

  return false;
}
