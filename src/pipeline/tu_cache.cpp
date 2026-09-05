// SPDX-License-Identifier: Apache-2.0
#include "lshaz/pipeline/tu_cache.h"
#include "lshaz/core/config.h"
#include "lshaz/core/version.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MD5.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

namespace lshaz {

namespace {

std::string hashOf(llvm::StringRef s) {
    llvm::MD5 h;
    h.update(s);
    llvm::MD5::MD5Result r;
    h.final(r);
    return r.digest().str().str();
}

// Absent and unreadable both hash to a sentinel rather than to nothing, so a
// deleted header invalidates instead of matching an entry recorded when it
// existed.
std::string hashFile(const std::string &p) {
    auto buf = llvm::MemoryBuffer::getFile(p);
    if (!buf)
        return "<absent>";
    return hashOf((*buf)->getBuffer());
}

// Format version. Bumping it invalidates every entry, which is the correct
// response to changing what a record contains or how a key is built.
constexpr const char *kCacheFormat = "1";

} // namespace

std::string TUCache::keyFor(const std::string &mainFile,
                            const std::vector<std::string> &command,
                            const std::string &configDigest) {
    std::string material = kCacheFormat;
    material += '\0';
    material += kToolVersion;
    material += '\0';
    material += mainFile;
    material += '\0';
    for (const auto &a : command) {
        material += a;
        material += '\0';
    }
    material += configDigest;
    return hashOf(material);
}

std::string TUCache::path(const std::string &key) const {
    llvm::SmallString<256> p(dir_);
    llvm::sys::path::append(p, key.substr(0, 2));
    llvm::sys::path::append(p, key + ".rec");
    return std::string(p);
}

bool TUCache::lookup(const std::string &key, std::string &record) {
    if (!enabled())
        return false;
    auto buf = llvm::MemoryBuffer::getFile(path(key));
    if (!buf) {
        ++misses_;
        return false;
    }
    // "<path>\t<hash>\n" per dependency, a blank line, then the record.
    llvm::StringRef all = (*buf)->getBuffer();
    while (true) {
        auto [line, rest] = all.split('\n');
        if (line.empty()) {
            all = rest;
            break;
        }
        auto [p, h] = line.split('\t');
        if (h.empty() || hashFile(p.str()) != h) {
            ++misses_;
            return false;
        }
        all = rest;
    }
    record = all.str();
    ++hits_;
    return true;
}

void TUCache::store(const std::string &key,
                    const std::vector<std::string> &deps,
                    const std::string &record) {
    if (!enabled())
        return;
    const std::string p = path(key);
    llvm::SmallString<256> parent(p);
    llvm::sys::path::remove_filename(parent);
    if (llvm::sys::fs::create_directories(parent))
        return;

    // Write then rename: a reader must never see a half-written entry, and
    // shards write concurrently.
    llvm::SmallString<256> tmp(p);
    tmp += "." + std::to_string(::getpid()) + ".tmp";
    std::error_code ec;
    {
        llvm::raw_fd_ostream out(std::string(tmp), ec);
        if (ec)
            return;
        for (const auto &d : deps)
            out << d << '\t' << hashFile(d) << '\n';
        out << '\n' << record;
        out.close();
        if (out.has_error())
            return;
    }
    if (llvm::sys::fs::rename(tmp, p))
        llvm::sys::fs::remove(tmp);
}

std::string configDigest(const Config &cfg) {
    std::string m;
    auto add = [&](llvm::StringRef s) { m += s; m += '\0'; };
    auto addN = [&](long long n) { m += std::to_string(n); m += '\0'; };
    auto addV = [&](const std::vector<std::string> &v) {
        for (const auto &s : v) add(s);
        m += '\1';
    };

    addN(static_cast<long long>(cfg.cacheLineBytes));
    addN(static_cast<long long>(cfg.stackFrameWarnBytes));
    addN(static_cast<long long>(cfg.allocSizeEscalation));
    addN(cfg.inferHotPaths ? 1 : 0);
    addN(static_cast<long long>(cfg.targetArch));
    add(cfg.linkedAllocator);
    addV(cfg.atomicTypeNames);
    addV(cfg.allocatorFunctionPatterns);
    addV(cfg.lockFunctionPatterns);
    addV(cfg.unlockFunctionPatterns);
    addV(cfg.mappingFunctionPatterns);
    addV(cfg.hotFunctionPatterns);
    addV(cfg.hotFilePatterns);
    addV(cfg.mainFunctionPatterns);
    addV(cfg.threadEntryPatterns);
    addV(cfg.disabledRules);
    // Derived names are an input to pass two exactly like a config list, and
    // they change when any file in the project changes.
    for (const auto &s : cfg.derivedAllocatorNames) add(s);
    m += '\1';
    for (const auto &s : cfg.derivedFreeNames) add(s);
    m += '\1';
    for (const auto &s : cfg.derivedMappingNames) add(s);
    return hashOf(m);
}

} // namespace lshaz
