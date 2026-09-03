// SPDX-License-Identifier: Apache-2.0
#include "lshaz/ir/opt_remark.h"

#include <llvm/Demangle/Demangle.h>
#include <llvm/Remarks/Remark.h>
#include <llvm/Remarks/RemarkFormat.h>
#include <llvm/Remarks/RemarkParser.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

namespace lshaz {

namespace {

// The oracle keys on getQualifiedNameAsString(), so the signature has to go.
// Bracket depth rather than find('<'), which misreads operator<<.
std::string qualifiedFrom(llvm::StringRef mangled) {
    std::string d = llvm::demangle(mangled.str());
    if (auto paren = d.find('('); paren != std::string::npos)
        d.resize(paren);

    int depth = 0;
    size_t lastSpace = std::string::npos;
    for (size_t i = 0; i < d.size(); ++i) {
        if (d[i] == '<')      ++depth;
        else if (d[i] == '>') --depth;
        else if (d[i] == ' ' && depth == 0) lastSpace = i;
    }
    if (lastSpace != std::string::npos)
        d = d.substr(lastSpace + 1);
    return d;
}

} // anonymous namespace

bool remarkIsReportable(llvm::StringRef pass, llvm::StringRef name) {
    // regalloc/LoopSpillReloadCopies is a backend pass and -S -emit-llvm
    // stops before codegen, so listing it would ship a class that cannot
    // fire. gvn/LoadClobbered is 4822 of one file's 8264 missed remarks and
    // reports imprecise alias analysis, not lost work.
    return pass == "licm" && name == "LoadWithLoopInvariantAddressInvalidated";
}

bool parseOptRemarks(const std::string &path, std::vector<OptRemark> &out) {
    auto buf = llvm::MemoryBuffer::getFile(path);
    if (!buf)
        return buf.getError() == std::errc::no_such_file_or_directory;

    // Bitstream is unreadable under -S -emit-llvm: codegen is skipped, so
    // the string table never reaches the object's .llvm_remarks section.
    // A TU that declined nothing writes a zero-byte file, which the parser
    // rejects as malformed.
    llvm::StringRef body = (*buf)->getBuffer();
    if (body.trim().empty())
        return true;

    auto parser = llvm::remarks::createRemarkParser(
        llvm::remarks::Format::YAML, body);
    if (!parser) {
        llvm::errs() << "lshaz: cannot read remarks from " << path << ": "
                     << llvm::toString(parser.takeError()) << "\n";
        return false;
    }

    while (true) {
        auto maybe = (*parser)->next();
        if (!maybe) {
            llvm::Error e = maybe.takeError();
            if (e.isA<llvm::remarks::EndOfFileError>()) {
                llvm::consumeError(std::move(e));
                break;
            }
            // A truncated stream must not read as a clean parse.
            llvm::errs() << "lshaz: remark stream for " << path
                         << " ended badly: " << llvm::toString(std::move(e))
                         << "\n";
            return false;
        }

        const llvm::remarks::Remark &r = **maybe;
        if (r.RemarkType != llvm::remarks::Type::Missed)
            continue;
        if (!remarkIsReportable(r.PassName, r.RemarkName))
            continue;
        // Remark::Loc is the site; an Argument's own Loc points at the
        // callee or the clobbering store.
        if (!r.Loc || !r.Loc->SourceLine || r.FunctionName.empty())
            continue;

        OptRemark o;
        o.pass = r.PassName.str();
        o.name = r.RemarkName.str();
        o.function = qualifiedFrom(r.FunctionName);
        o.file = r.Loc->SourceFilePath.str();
        o.line = r.Loc->SourceLine;
        o.column = r.Loc->SourceColumn;
        for (const auto &a : r.Args) {
            if (a.Key == "String") {
                if (o.detail.empty() && a.Val.size() > 8)
                    o.detail = a.Val.str();
            } else if (!o.count) {
                unsigned v = 0;
                if (!a.Val.getAsInteger(10, v))
                    o.count = v;
            }
        }
        out.push_back(std::move(o));
    }
    return true;
}

} // namespace lshaz
