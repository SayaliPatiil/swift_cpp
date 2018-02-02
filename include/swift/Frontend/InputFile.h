//===--- InputFile.h --------------------------------------------*- C++ -*-===//
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

#ifndef SWIFT_FRONTEND_INPUTFILE_H
#define SWIFT_FRONTEND_INPUTFILE_H

#include "llvm/Support/MemoryBuffer.h"
#include <string>
#include <vector>

namespace swift {

enum class InputFileKind {
  IFK_None,
  IFK_Swift,
  IFK_Swift_Library,
  IFK_Swift_REPL,
  IFK_SIL,
  IFK_LLVM_IR
};

// Inputs may include buffers that override contents, and eventually should
// always include a buffer.
class InputFile {
  std::string Filename;
  bool IsPrimary;
  /// Points to a buffer overriding the file's contents, or nullptr if there is
  /// none.
  llvm::MemoryBuffer *Buffer;

  /// Contains the name of the main output file, that is, the .o file for this
  /// input. If there is no such file, contains an empty string. If the output
  /// is to be written to stdout, contains "-".
  std::string OutputFilename;

public:
  /// Does not take ownership of \p buffer. Does take ownership of (copy) a
  /// string.
  InputFile(StringRef name, bool isPrimary,
            llvm::MemoryBuffer *buffer = nullptr,
            StringRef outputFilename = StringRef())
      : Filename(
            convertBufferNameFromLLVM_getFileOrSTDIN_toSwiftConventions(name)),
        IsPrimary(isPrimary), Buffer(buffer), OutputFilename(outputFilename) {
    assert(!name.empty());
  }

  bool isPrimary() const { return IsPrimary; }
  llvm::MemoryBuffer *buffer() const { return Buffer; }
  StringRef file() const {
    assert(!Filename.empty());
    return Filename;
  }

  /// Return Swift-standard file name from a buffer name set by
  /// llvm::MemoryBuffer::getFileOrSTDIN, which uses "<stdin>" instead of "-".
  static StringRef convertBufferNameFromLLVM_getFileOrSTDIN_toSwiftConventions(
      StringRef filename) {
    return filename.equals("<stdin>") ? "-" : filename;
  }

  const std::string &outputFilename() const { return OutputFilename; }

  void setOutputFilename(StringRef outputFilename) {
    OutputFilename = outputFilename;
  }
};

} // namespace swift

#endif /* SWIFT_FRONTEND_INPUTFILE_H */
