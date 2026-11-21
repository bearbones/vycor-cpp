// Copyright (c) 2026 The vycor-cpp Authors
// Original author: Alex Mason
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "vycor/check/IsaLeak.h"

#include "vycor/check/GlobalIndex.h"
#include "vycor/check/Indexer.h"
#include "vycor/compat/ClangVersion.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclFriend.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace vycor {

// ===========================================================================
// Facet list
// ===========================================================================

bool parseIsaLeakFacets(const std::string &spec, unsigned &facets,
                        std::string &error) {
  unsigned out = 0;
  llvm::SmallVector<llvm::StringRef, 4> parts;
  llvm::StringRef(spec).split(parts, ',', -1, /*KeepEmpty=*/false);
  for (llvm::StringRef part : parts) {
    part = part.trim();
    if (part == "isa")
      out |= kIsaLeakIsa;
    else if (part == "fp")
      out |= kIsaLeakFp;
    else if (part == "eh")
      out |= kIsaLeakEh;
    else if (part == "overflow")
      out |= kIsaLeakOverflow;
    else if (part == "default")
      out |= kIsaLeakDefaultFacets;
    else if (part == "all")
      out |= kIsaLeakIsa | kIsaLeakFp | kIsaLeakEh | kIsaLeakOverflow;
    else {
      error = "unknown isa-leak facet '" + part.str() +
              "' (expected isa, fp, eh, overflow, default, or all)";
      return false;
    }
  }
  if (!out) {
    error = "empty isa-leak facet list";
    return false;
  }
  facets = out;
  return true;
}

// ===========================================================================
// IsaLeakFacts
// ===========================================================================

namespace {

std::string profileKey(const CodegenProfile &p) {
  std::string key = p.triple;
  key += '\x1f';
  for (const auto &f : p.features) {
    key += f;
    key += ',';
  }
  key += '\x1f';
  key += p.fp;
  key += '\x1f';
  key += p.exceptions ? '1' : '0';
  key += static_cast<char>('0' + p.signedOverflow);
  return key;
}

} // namespace

uint32_t IsaLeakFacts::addProfile(const CodegenProfile &profile) {
  std::string key = profileKey(profile);
  std::lock_guard<std::mutex> lock(mutex_);
  auto [it, inserted] =
      profileByKey_.try_emplace(std::move(key),
                                static_cast<uint32_t>(profiles_.size()));
  if (inserted)
    profiles_.push_back(profile);
  return it->second;
}

uint32_t IsaLeakFacts::tuIdLocked(const std::string &tu) {
  auto [it, inserted] =
      tuIds_.try_emplace(tu, static_cast<uint32_t>(tus_.size()));
  if (inserted)
    tus_.push_back(tu);
  return it->second;
}

uint32_t IsaLeakFacts::recordLocked(const EmittedFunction &f) {
  auto [it, inserted] = recordByName_.try_emplace(
      f.linkageName, static_cast<uint32_t>(records_.size()));
  if (inserted) {
    records_.emplace_back();
    records_.back().function = f;
    return it->second;
  }
  EmittedFunction &r = records_[it->second].function;
  // One linkage name, one definition in a well-formed program; when two
  // sightings disagree on where it is, keep the smallest so the merged
  // record is the same whatever order TUs arrive in.
  if (std::tie(f.filePath, f.line, f.qualifiedName) <
      std::tie(r.filePath, r.line, r.qualifiedName)) {
    r.filePath = f.filePath;
    r.line = f.line;
    r.qualifiedName = f.qualifiedName;
  }
  r.bodyBits |= f.bodyBits;
  r.ownBits |= f.ownBits;
  return it->second;
}

void IsaLeakFacts::addTu(const std::string &tuPath, uint32_t profile,
                         std::vector<EmittedFunction> &&functions) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (profile >= profiles_.size())
    return;
  const uint32_t tu = tuIdLocked(tuPath);
  for (const EmittedFunction &f : functions)
    records_[recordLocked(f)].sightings.emplace_back(profile, tu);
}

void IsaLeakFacts::absorb(const IsaLeakFacts &other) {
  if (&other == this)
    return;
  std::vector<uint32_t> profileMap;
  profileMap.reserve(other.profiles_.size());
  for (const auto &p : other.profiles_)
    profileMap.push_back(addProfile(p));
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<uint32_t> tuMap;
  tuMap.reserve(other.tus_.size());
  for (const auto &tu : other.tus_)
    tuMap.push_back(tuIdLocked(tu));
  for (const Record &r : other.records_) {
    Record &into = records_[recordLocked(r.function)];
    into.sightings.reserve(into.sightings.size() + r.sightings.size());
    for (const auto &[profile, tu] : r.sightings)
      into.sightings.emplace_back(profileMap[profile], tuMap[tu]);
  }
}

void IsaLeakFacts::capture(IsaLeakPayload &out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  out.profiles = profiles_;
  out.tus = tus_;
  out.functions.clear();
  out.functions.reserve(records_.size());
  out.sightings.clear();
  size_t n = 0;
  for (const Record &r : records_)
    n += r.sightings.size();
  out.sightings.reserve(n);
  for (const Record &r : records_) {
    const auto function = static_cast<uint32_t>(out.functions.size());
    out.functions.push_back(r.function);
    for (const auto &[profile, tu] : r.sightings)
      out.sightings.push_back({function, profile, tu});
  }
}

void IsaLeakFacts::apply(const IsaLeakPayload &payload) {
  if (payload.sightings.empty())
    return;
  std::vector<uint32_t> profileMap;
  profileMap.reserve(payload.profiles.size());
  for (const auto &p : payload.profiles)
    profileMap.push_back(addProfile(p));
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<uint32_t> tuMap;
  tuMap.reserve(payload.tus.size());
  for (const auto &tu : payload.tus)
    tuMap.push_back(tuIdLocked(tu));
  // Records are interned lazily: a function no valid sighting names adds
  // nothing.
  constexpr uint32_t kUnmapped = ~0u;
  std::vector<uint32_t> functionMap(payload.functions.size(), kUnmapped);
  for (const IsaLeakSighting &s : payload.sightings) {
    if (s.function >= functionMap.size() || s.profile >= profileMap.size() ||
        s.tu >= tuMap.size())
      continue; // a corrupt sighting: drop it
    uint32_t &rec = functionMap[s.function];
    if (rec == kUnmapped)
      rec = recordLocked(payload.functions[s.function]);
    records_[rec].sightings.emplace_back(profileMap[s.profile], tuMap[s.tu]);
  }
}

void IsaLeakFacts::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  profiles_.clear();
  profileByKey_.clear();
  tus_.clear();
  tuIds_.clear();
  records_.clear();
  recordByName_.clear();
}

size_t IsaLeakFacts::sightingCount() const {
  size_t n = 0;
  for (const auto &r : records_)
    n += r.sightings.size();
  return n;
}

// ===========================================================================
// Phase 1: profile and emitted functions of one TU
// ===========================================================================

namespace {

std::string fpProfile(const clang::LangOptions &lo,
                      const clang::TargetOptions &to) {
  std::string s;
  auto put = [&](const char *key, int value) {
    s += key;
    s += '=';
    s += std::to_string(value);
    s += ';';
  };
  put("contract", static_cast<int>(lo.getDefaultFPContractMode()));
  put("reassoc", lo.AllowFPReassoc);
  put("nnan", lo.NoHonorNaNs);
  put("ninf", lo.NoHonorInfs);
  put("nsz", lo.NoSignedZero);
  put("arcp", lo.AllowRecip);
  put("afn", lo.ApproxFunc);
  put("errno", lo.MathErrno);
  put("round", static_cast<int>(lo.getDefaultRoundingMode()));
  put("except", static_cast<int>(lo.getDefaultExceptionMode()));
  put("eval", static_cast<int>(lo.getFPEvalMethod()));
  s += "fpmath=";
  s += to.FPMath;
  s += ';';
  return s;
}

CodegenProfile profileOf(const clang::ASTContext &ctx) {
  CodegenProfile p;
  const clang::TargetInfo &ti = ctx.getTargetInfo();
  const clang::TargetOptions &to = ti.getTargetOpts();
  p.triple = ti.getTriple().str();
  // Features is the effective list after the CPU's implied features were
  // applied, so -march=haswell and its -m<feature> spelling agree.
  for (const std::string &f : to.Features)
    if (f.size() > 1 && f[0] == '+')
      p.features.push_back(f.substr(1));
  if (p.features.empty())
    for (const auto &kv : to.FeatureMap)
      if (kv.getValue())
        p.features.push_back(kv.getKey().str());
  std::sort(p.features.begin(), p.features.end());
  p.features.erase(std::unique(p.features.begin(), p.features.end()),
                   p.features.end());
  const clang::LangOptions &lo = ctx.getLangOpts();
  p.fp = fpProfile(lo, to);
  p.exceptions = lo.CXXExceptions;
  switch (lo.getSignedOverflowBehavior()) {
  case clang::LangOptions::SOB_Undefined:
    p.signedOverflow = 0;
    break;
  case clang::LangOptions::SOB_Defined:
    p.signedOverflow = 1;
    break;
  case clang::LangOptions::SOB_Trapping:
    p.signedOverflow = 2;
    break;
  }
  return p;
}

bool hasTargetAttr(const clang::FunctionDecl *fd) {
  return fd->hasAttr<clang::TargetAttr>() ||
         fd->hasAttr<clang::TargetClonesAttr>() ||
         fd->hasAttr<clang::TargetVersionAttr>() ||
         fd->hasAttr<clang::CPUSpecificAttr>() ||
         fd->hasAttr<clang::CPUDispatchAttr>();
}

// The TU's reference graph over function definitions, walked the way code
// generation decides what to emit: from what the TU must emit (strong
// definitions, global initializers, vtables it homes) through every
// function a walked body references. Node 0 is the TU itself.
class Collector {
public:
  explicit Collector(clang::ASTContext &ctx)
      : ctx_(ctx), sm_(ctx.getSourceManager()),
        mangle_(ctx.createMangleContext()) {}

  void run(GlobalIndex &index) {
    if (!ctx_.getLangOpts().CPlusPlus)
      return;
    // Node 0: the TU. Its edges are the roots.
    decl_.push_back(nullptr);
    vars_.push_back(nullptr);
    bits_.push_back(0);
    edgeBegin_.push_back(0);
    cur_ = 0;
    scan(ctx_.getTranslationUnitDecl());
    drain();
    // Walking a body appends the nodes it references, so this loop also
    // drains them. Bodies are walked in node order, which leaves the edge
    // list grouped by source: edgeBegin_ is its CSR offset table.
    for (uint32_t i = 1; i < decl_.size(); ++i) {
      edgeBegin_.push_back(static_cast<uint32_t>(edgeDst_.size()));
      cur_ = i;
      if (vars_[i])
        walkVariable(vars_[i]);
      else
        walkFunction(decl_[i]);
      drain();
    }
    edgeBegin_.push_back(static_cast<uint32_t>(edgeDst_.size()));
    own_ = bits_;
    foldInlinableBits();
    exportFacts(index);
  }

private:
  enum EdgeBit : uint8_t {
    kFold = 1, // the source can inline the destination's body
  };

  // ---- node table ------------------------------------------------------

  uint32_t nodeFor(const clang::FunctionDecl *fd) {
    const clang::FunctionDecl *canon = fd->getCanonicalDecl();
    auto id = static_cast<uint32_t>(decl_.size());
    auto [it, inserted] = nodeOf_.try_emplace(canon, id);
    if (!inserted)
      return it->second;
    const clang::FunctionDecl *def = nullptr;
    if (!(fd->hasBody(def) && def && def->getBody() &&
          !def->hasSkippedBody() && !def->isLateTemplateParsed() &&
          !def->isInvalidDecl() && !def->isDependentContext()))
      def = nullptr;
    decl_.push_back(def);
    vars_.push_back(nullptr);
    bits_.push_back(def && hasTargetAttr(def) ? uint32_t(kBodyTargetAttr) : 0u);
    return id;
  }

  // A global variable's dynamic initialization and destruction run in the
  // TU's global initializer, not in the body that references it: the
  // variable is a node of its own (no body, never folded) whose walk adds
  // the functions they reference.
  void reachVariable(const clang::VarDecl *vd) {
    if (!vd->hasGlobalStorage() || vd->isStaticLocal() || vd->isTemplated())
      return;
    const clang::VarDecl *def = vd->getDefinition();
    if (!def || def->isInvalidDecl())
      return;
    auto id = static_cast<uint32_t>(decl_.size());
    auto [it, inserted] = varNode_.try_emplace(def->getCanonicalDecl(), id);
    if (inserted) {
      decl_.push_back(nullptr);
      vars_.push_back(def);
      bits_.push_back(0);
    }
    if (edgeDst_.size() > edgeBegin_.back() && edgeDst_.back() == it->second)
      return;
    edgeDst_.push_back(it->second);
    edgeBits_.push_back(0);
  }

  void walkVariable(const clang::VarDecl *vd) {
    // A constant initializer is emitted as data: nothing runs, nothing it
    // names (a constexpr constructor, say) is emitted for it.
    const clang::Expr *init = vd->getInit();
    if (init && (init->isValueDependent() || !vd->evaluateValue()))
      push(init);
    destroy(vd->getType());
  }

  void addEdge(const clang::FunctionDecl *fd, uint8_t bits = kFold) {
    if (!fd)
      return;
    uint32_t dst = nodeFor(fd);
    if (edgeDst_.size() > edgeBegin_.back() && edgeDst_.back() == dst &&
        edgeBits_.back() == bits)
      return;
    edgeDst_.push_back(dst);
    edgeBits_.push_back(bits);
  }

  // ---- roots -------------------------------------------------------------

  static bool emittedUnconditionally(clang::GVALinkage gva) {
    return gva == clang::GVA_StrongExternal || gva == clang::GVA_StrongODR;
  }

  void rootFunction(const clang::FunctionDecl *fd) {
    if (!fd->doesThisDeclarationHaveABody() || fd->isLateTemplateParsed() ||
        fd->isDependentContext() || fd->isInvalidDecl())
      return;
    if (emittedUnconditionally(ctx_.GetGVALinkageForFunction(fd)) ||
        fd->hasAttr<clang::UsedAttr>() ||
        fd->hasAttr<clang::ConstructorAttr>() ||
        fd->hasAttr<clang::DestructorAttr>())
      addEdge(fd, 0);
  }

  // ASTContext::DeclMustBeEmitted for variables: a variable another TU
  // can need, or one whose initialization or destruction has side
  // effects. Any other is emitted only where emitted code references it
  // (reachVariable from the body walk).
  void rootVariable(const clang::VarDecl *vd) {
    if (!vd->hasGlobalStorage() || vd->isStaticLocal() ||
        vd->isTemplated() || vd->isInvalidDecl() ||
        vd->isThisDeclarationADefinition() != clang::VarDecl::Definition)
      return;
    const clang::GVALinkage gva = ctx_.GetGVALinkageForVariable(vd);
    if (gva == clang::GVA_AvailableExternally)
      return;
    const clang::Expr *init = vd->getInit();
    if (!clang::isDiscardableGVALinkage(gva) || vd->needsDestruction(ctx_) ||
        (init && init->HasSideEffects(ctx_) &&
         (init->isValueDependent() || !vd->evaluateValue())))
      reachVariable(vd);
  }

  // A dynamic class whose key function this TU defines emits its vtable
  // here, and with it every virtual function.
  void rootVtable(const clang::CXXRecordDecl *rd) {
    if (!rd->isDynamicClass())
      return;
    const clang::CXXMethodDecl *key = ctx_.getCurrentKeyFunction(rd);
    const clang::FunctionDecl *keyDef = nullptr;
    if (key && key->hasBody(keyDef) && keyDef && !keyDef->isInlined())
      vtableMembers(rd);
  }

  // Every slot of the class's vtables, its bases' included: the final
  // overrider of each virtual function. Reached through the vtable, so
  // never inlined into the referencing body.
  void vtableMembers(const clang::CXXRecordDecl *rd) {
    if (!vtablesDone_.insert(rd->getCanonicalDecl()).second)
      return;
    clang::CXXFinalOverriderMap overriders;
    rd->getFinalOverriders(overriders);
    for (const auto &entry : overriders)
      for (const auto &subobject : entry.second)
        for (const clang::UniqueVirtualMethod &m : subobject.second)
          if (m.Method && !m.Method->isPureVirtual())
            addEdge(m.Method, 0);
  }

  void scan(const clang::DeclContext *dc) {
    for (const clang::Decl *d : dc->decls()) {
      if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(d)) {
        rootFunction(fd);
        continue;
      }
      switch (d->getKind()) {
      case clang::Decl::Namespace:
        scan(llvm::cast<clang::NamespaceDecl>(d));
        break;
      case clang::Decl::LinkageSpec:
        scan(llvm::cast<clang::LinkageSpecDecl>(d));
        break;
      case clang::Decl::Export:
        scan(llvm::cast<clang::ExportDecl>(d));
        break;
      case clang::Decl::Var:
        rootVariable(llvm::cast<clang::VarDecl>(d));
        break;
      case clang::Decl::CXXRecord:
      case clang::Decl::ClassTemplateSpecialization:
        scanRecord(llvm::cast<clang::CXXRecordDecl>(d));
        break;
      case clang::Decl::ClassTemplate: {
        // Explicit instantiation definitions emit their members here.
        const auto *ctd = llvm::cast<clang::ClassTemplateDecl>(d);
        if (!ctd->isFirstDecl())
          break;
        for (const auto *spec :
             const_cast<clang::ClassTemplateDecl *>(ctd)->specializations())
          if (spec->getSpecializationKind() ==
              clang::TSK_ExplicitInstantiationDefinition)
            scanRecord(spec);
        break;
      }
      case clang::Decl::FunctionTemplate: {
        const auto *ftd = llvm::cast<clang::FunctionTemplateDecl>(d);
        if (!ftd->isFirstDecl())
          break;
        for (const auto *spec :
             const_cast<clang::FunctionTemplateDecl *>(ftd)->specializations())
          if (spec->getTemplateSpecializationKind() ==
              clang::TSK_ExplicitInstantiationDefinition)
            if (const clang::FunctionDecl *def = spec->getDefinition())
              rootFunction(def);
        break;
      }
      case clang::Decl::Friend:
        if (const auto *nd = llvm::cast<clang::FriendDecl>(d)->getFriendDecl())
          if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(nd))
            rootFunction(fd);
        break;
      default:
        break;
      }
    }
  }

  void scanRecord(const clang::CXXRecordDecl *rd) {
    if (!rd->isThisDeclarationADefinition() || rd->isDependentContext() ||
        rd->isLambda() || rd->isInvalidDecl())
      return;
    rootVtable(rd);
    scan(rd);
  }

  // ---- body walk -------------------------------------------------------

  void push(const clang::Stmt *s) {
    if (s)
      stack_.push_back(s);
  }

  void destroy(clang::QualType type) {
    if (type.isNull())
      return;
    const clang::CXXRecordDecl *rd =
        ctx_.getBaseElementType(type)->getAsCXXRecordDecl();
    if (!rd || !rd->hasDefinition() || rd->hasTrivialDestructor())
      return;
    if (const clang::CXXDestructorDecl *dtor = rd->getDestructor())
      if (!dtor->isDeleted()) {
        addEdge(dtor);
        bits_[cur_] |= kBodyCleanup;
      }
  }

  // Constructing a dynamic class stores its vtable pointer: a vtable with
  // no key function is emitted, with its virtual functions, by every TU
  // that uses it.
  void construct(const clang::CXXConstructorDecl *ctor) {
    addEdge(ctor);
    if (!ctor->isTrivial() && !nothrow(ctor))
      bits_[cur_] |= kBodyMayThrowCall;
    const clang::CXXRecordDecl *rd = ctor->getParent();
    if (rd->isDynamicClass() && !ctx_.getCurrentKeyFunction(rd))
      vtableMembers(rd);
  }

  static bool nothrow(const clang::FunctionDecl *fd) {
    const auto *fpt = fd->getType()->getAs<clang::FunctionProtoType>();
    return fpt && fpt->isNothrow();
  }

  void callee(const clang::CallExpr *call) {
    const clang::FunctionDecl *fd = call->getDirectCallee();
    if (!fd) {
      bits_[cur_] |= kBodyMayThrowCall; // indirect: anything can happen
      return;
    }
    if (unsigned id = fd->getBuiltinID()) {
      builtin(id, call);
      return;
    }
    if (!nothrow(fd))
      bits_[cur_] |= kBodyMayThrowCall;
  }

  void builtin(unsigned id, const clang::CallExpr *call) {
    if (ctx_.BuiltinInfo.isTSBuiltin(id)) {
      bits_[cur_] |= kBodyTargetBuiltin;
      return;
    }
    switch (id) {
    case clang::Builtin::BI__builtin_popcount:
    case clang::Builtin::BI__builtin_popcountl:
    case clang::Builtin::BI__builtin_popcountll:
    case clang::Builtin::BI__builtin_clz:
    case clang::Builtin::BI__builtin_clzl:
    case clang::Builtin::BI__builtin_clzll:
    case clang::Builtin::BI__builtin_ctz:
    case clang::Builtin::BI__builtin_ctzl:
    case clang::Builtin::BI__builtin_ctzll:
    case clang::Builtin::BI__builtin_bswap16:
    case clang::Builtin::BI__builtin_bswap32:
    case clang::Builtin::BI__builtin_bswap64:
    case clang::Builtin::BI__builtin_rotateleft32:
    case clang::Builtin::BI__builtin_rotateleft64:
    case clang::Builtin::BI__builtin_rotateright32:
    case clang::Builtin::BI__builtin_rotateright64:
    case clang::Builtin::BI__builtin_rotateleft8:
    case clang::Builtin::BI__builtin_rotateleft16:
    case clang::Builtin::BI__builtin_rotateright8:
    case clang::Builtin::BI__builtin_rotateright16:
    case clang::Builtin::BI__builtin_ffs:
    case clang::Builtin::BI__builtin_ffsl:
    case clang::Builtin::BI__builtin_ffsll:
    case clang::Builtin::BI__builtin_parity:
    case clang::Builtin::BI__builtin_parityl:
    case clang::Builtin::BI__builtin_parityll:
    case clang::Builtin::BI__builtin_clrsb:
    case clang::Builtin::BI__builtin_clrsbl:
    case clang::Builtin::BI__builtin_clrsbll:
#if VYCOR_LLVM_AT_LEAST(19)
    case clang::Builtin::BI__builtin_popcountg:
    case clang::Builtin::BI__builtin_clzg:
    case clang::Builtin::BI__builtin_ctzg:
#endif
      bits_[cur_] |= kBodyBitBuiltin;
      return;
    case clang::Builtin::BImemcpy:
    case clang::Builtin::BImemmove:
    case clang::Builtin::BImemset:
    case clang::Builtin::BI__builtin_memcpy:
    case clang::Builtin::BI__builtin_memmove:
    case clang::Builtin::BI__builtin_memset:
      bits_[cur_] |= kBodyBulkMemory;
      return;
    default:
      break;
    }
    // A math builtin (sqrt, exp, floor, ...) on floating-point values:
    // -fno-math-errno and -fapprox-func change how it is lowered.
    if (call->getType()->isFloatingType())
      bits_[cur_] |= kBodyFloat | kBodyMathCall;
  }

  static bool floatCast(clang::CastKind kind) {
    switch (kind) {
    case clang::CK_IntegralToFloating:
    case clang::CK_FloatingToIntegral:
    case clang::CK_FloatingCast:
    case clang::CK_FloatingToBoolean:
    case clang::CK_FloatingRealToComplex:
    case clang::CK_FloatingComplexCast:
    case clang::CK_FloatingComplexToReal:
    case clang::CK_IntegralComplexToFloatingComplex:
    case clang::CK_FloatingComplexToIntegralComplex:
      return true;
    default:
      return false;
    }
  }

  static bool signedArithmetic(clang::BinaryOperatorKind op) {
    switch (op) {
    case clang::BO_Add:
    case clang::BO_Sub:
    case clang::BO_Mul:
    case clang::BO_Shl:
    case clang::BO_AddAssign:
    case clang::BO_SubAssign:
    case clang::BO_MulAssign:
    case clang::BO_ShlAssign:
      return true;
    default:
      return false;
    }
  }

  void walkFunction(const clang::FunctionDecl *def) {
    if (!def)
      return;
    if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(def))
      for (const clang::CXXCtorInitializer *init : ctor->inits())
        push(init->getInit());
    if (const auto *dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(def)) {
      // Members and bases are destroyed after the body.
      const clang::CXXRecordDecl *rd = dtor->getParent();
      for (const clang::FieldDecl *field : rd->fields())
        destroy(field->getType());
      for (const clang::CXXBaseSpecifier &base : rd->bases())
        destroy(base.getType());
    }
    // By-value parameters are destroyed by the callee only in ABIs that
    // say so (Microsoft); in Itanium the caller destroys them, which the
    // caller's walk sees as a bound temporary.
    const bool calleeDestroys =
        ctx_.getTargetInfo().getCXXABI().areArgsDestroyedLeftToRightInCallee();
    for (const clang::ParmVarDecl *param : def->parameters()) {
      if (calleeDestroys)
        destroy(param->getType());
      if (param->getType()->isFloatingType())
        bits_[cur_] |= kBodyFloatMove;
    }
    if (def->getReturnType()->isFloatingType())
      bits_[cur_] |= kBodyFloatMove;
    push(def->getBody());
    drain();
  }

  void drain() {
    while (!stack_.empty()) {
      const clang::Stmt *s = stack_.pop_back_val();
      visit(s);
    }
  }

  // A call on an object whose dynamic type is its static type (a variable
  // of class type, or a temporary): code generation calls the function
  // directly, as CodeGen's CanDevirtualizeMemberFunctionCall does.
  static bool completeObject(const clang::MemberExpr *member) {
    if (member->isArrow())
      return false;
    const clang::Expr *base = member->getBase()->IgnoreParenImpCasts();
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(base))
      if (const auto *vd = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
        return !vd->getType()->isReferenceType();
    return base->isPRValue() ||
           llvm::isa<clang::MaterializeTemporaryExpr>(base);
  }

  bool constantFalse(const clang::Expr *cond) const {
    bool value = true;
    return cond && !cond->isValueDependent() &&
           cond->EvaluateAsBooleanCondition(value, ctx_) && !value;
  }

  void visit(const clang::Stmt *s) {
    switch (s->getStmtClass()) {
    case clang::Stmt::ForStmtClass:
      if (!constantFalse(llvm::cast<clang::ForStmt>(s)->getCond()))
        bits_[cur_] |= kBodyLoop;
      break;
    case clang::Stmt::WhileStmtClass:
      if (!constantFalse(llvm::cast<clang::WhileStmt>(s)->getCond()))
        bits_[cur_] |= kBodyLoop;
      break;
    case clang::Stmt::DoStmtClass:
      // do { ... } while (0): the statement-macro idiom, not a loop.
      if (!constantFalse(llvm::cast<clang::DoStmt>(s)->getCond()))
        bits_[cur_] |= kBodyLoop;
      break;
    case clang::Stmt::CXXForRangeStmtClass:
      bits_[cur_] |= kBodyLoop;
      break;
    case clang::Stmt::CXXTryStmtClass:
      bits_[cur_] |= kBodyTry;
      break;
    case clang::Stmt::DeclStmtClass:
      for (const clang::Decl *d : llvm::cast<clang::DeclStmt>(s)->decls())
        if (const auto *v = llvm::dyn_cast<clang::VarDecl>(d)) {
          if (v->hasLocalStorage())
            destroy(v->getType());
          // A constexpr local's initializer is evaluated by the compiler.
          if (!v->isConstexpr())
            push(v->getInit());
        }
      return;
    case clang::Stmt::IfStmtClass: {
      const auto *ifs = llvm::cast<clang::IfStmt>(s);
      if (ifs->isConsteval()) {
        // if consteval: only the runtime branch is code.
        push(ifs->isNegatedConsteval() ? ifs->getThen() : ifs->getElse());
        return;
      }
      if (auto taken = ifs->getNondiscardedCase(ctx_)) {
        // if constexpr: the discarded branch is never emitted.
        push(ifs->getInit());
        push(*taken);
        return;
      }
      break;
    }
    // Unevaluated operands and values the compiler computed: no code.
    case clang::Stmt::UnaryExprOrTypeTraitExprClass:
      if (llvm::cast<clang::UnaryExprOrTypeTraitExpr>(s)
              ->getTypeOfArgument()
              ->isVariablyModifiedType())
        break; // sizeof a VLA is evaluated
      return;
    case clang::Stmt::CXXNoexceptExprClass:
    case clang::Stmt::RequiresExprClass:
      return;
    case clang::Stmt::CXXTypeidExprClass:
      if (!llvm::cast<clang::CXXTypeidExpr>(s)->isPotentiallyEvaluated())
        return;
      break;
    case clang::Stmt::ConstantExprClass:
      if (llvm::cast<clang::ConstantExpr>(s)->hasAPValueResult())
        return;
      break;
    case clang::Stmt::GenericSelectionExprClass: {
      const auto *gse = llvm::cast<clang::GenericSelectionExpr>(s);
      if (!gse->isResultDependent())
        push(gse->getResultExpr());
      return;
    }
    case clang::Stmt::LambdaExprClass: {
      // The closure's call operator is its own function, inlined where it
      // is called; only the captures are evaluated here.
      const auto *lambda = llvm::cast<clang::LambdaExpr>(s);
      addEdge(lambda->getCallOperator());
      for (const clang::Expr *init : lambda->capture_inits())
        push(init);
      return;
    }
    case clang::Stmt::CXXDefaultArgExprClass:
      push(llvm::cast<clang::CXXDefaultArgExpr>(s)->getExpr());
      return;
    case clang::Stmt::CXXDefaultInitExprClass:
      push(llvm::cast<clang::CXXDefaultInitExpr>(s)->getExpr());
      return;
    default:
      break;
    }

    if (const auto *e = llvm::dyn_cast<clang::Expr>(s)) {
      clang::QualType type = e->getType();
      if (!type.isNull() &&
          (type->isVectorType() || type->isSizelessBuiltinType()))
        bits_[cur_] |= kBodyVector;
      // Any floating-point value, even one only loaded, passed or returned:
      // with -mavx every scalar FP move is VEX-encoded. FP flags change
      // only arithmetic, comparisons and conversions (kBodyFloat).
      if (!type.isNull() && type->isFloatingType())
        bits_[cur_] |= kBodyFloatMove;
      if (const auto *bo = llvm::dyn_cast<clang::BinaryOperator>(e)) {
        clang::QualType operand = bo->getLHS()->getType();
        if (operand->isFloatingType() ||
            bo->getRHS()->getType()->isFloatingType()) {
          if (bo->getOpcode() != clang::BO_Assign &&
              bo->getOpcode() != clang::BO_Comma)
            bits_[cur_] |= kBodyFloat;
        }
        else if (signedArithmetic(bo->getOpcode()) &&
                 type->isSignedIntegerType())
          bits_[cur_] |= kBodySignedArith;
      } else if (const auto *uo = llvm::dyn_cast<clang::UnaryOperator>(e)) {
        clang::QualType operand = uo->getSubExpr()->getType();
        if (uo->isArithmeticOp() || uo->isIncrementDecrementOp()) {
          if (operand->isFloatingType())
            bits_[cur_] |= kBodyFloat;
          else if (operand->isSignedIntegerType() &&
                   uo->getOpcode() != clang::UO_Plus &&
                   uo->getOpcode() != clang::UO_Not &&
                   uo->getOpcode() != clang::UO_LNot)
            bits_[cur_] |= kBodySignedArith;
        }
      } else if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
        if (floatCast(cast->getCastKind()))
          bits_[cur_] |= kBodyFloat;
      } else if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
        callee(call);
        // A virtual call goes through the vtable: it neither emits the
        // callee here nor lets this body inline it. Walk the object and
        // the arguments, not the member reference.
        if (const auto *mc = llvm::dyn_cast<clang::CXXMemberCallExpr>(call))
          if (const auto *member =
                  llvm::dyn_cast<clang::MemberExpr>(mc->getCallee()->IgnoreParens()))
            if (const auto *md =
                    llvm::dyn_cast<clang::CXXMethodDecl>(member->getMemberDecl()))
              if (md->isVirtual() && !member->hasQualifier() &&
                  !md->hasAttr<clang::FinalAttr>() &&
                  !md->getParent()->hasAttr<clang::FinalAttr>() &&
                  !completeObject(member)) {
                push(member->getBase());
                for (const clang::Expr *arg : mc->arguments())
                  push(arg);
                return;
              }
      } else if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
        if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl())) {
          if (!fd->getBuiltinID())
            addEdge(fd);
        } else if (const auto *vd = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
          reachVariable(vd);
        }
      } else if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e)) {
        if (const auto *md =
                llvm::dyn_cast<clang::CXXMethodDecl>(member->getMemberDecl()))
          addEdge(md);
        else if (const auto *vd =
                     llvm::dyn_cast<clang::VarDecl>(member->getMemberDecl()))
          reachVariable(vd); // a static data member named through an object
      } else if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructExpr>(e)) {
        construct(ctor->getConstructor());
      } else if (const auto *bind =
                     llvm::dyn_cast<clang::CXXBindTemporaryExpr>(e)) {
        if (const clang::CXXDestructorDecl *dtor =
                bind->getTemporary()->getDestructor()) {
          addEdge(dtor);
          bits_[cur_] |= kBodyCleanup;
        }
      } else if (const auto *newExpr = llvm::dyn_cast<clang::CXXNewExpr>(e)) {
        addEdge(newExpr->getOperatorNew());
        if (!newExpr->shouldNullCheckAllocation())
          bits_[cur_] |= kBodyMayThrowCall;
      } else if (const auto *del = llvm::dyn_cast<clang::CXXDeleteExpr>(e)) {
        addEdge(del->getOperatorDelete());
        destroy(del->getDestroyedType());
      } else if (llvm::isa<clang::CXXThrowExpr>(e)) {
        bits_[cur_] |= kBodyMayThrowCall;
      }
    }
    for (const clang::Stmt *child : s->children())
      push(child);
  }

  // What a body can inline is part of what its flags change: OR each
  // node's bits with those of every body it can inline, to a fixed point.
  // A callee with its own target attribute is never inlined into a caller
  // built with fewer features, and its attribute is its own.
  void foldInlinableBits() {
    const uint32_t n = static_cast<uint32_t>(decl_.size());
    constexpr uint32_t kOwn = kBodyTargetAttr;
    // Reverse fold edges (callee -> callers) in CSR form, then a worklist:
    // a node's bits changing re-queues its callers. Bits only grow and are
    // few, so each node re-enters at most once per bit.
    std::vector<uint32_t> revBegin(n + 1, 0);
    auto folds = [&](uint32_t e) {
      uint32_t to = edgeDst_[e];
      return (edgeBits_[e] & kFold) && decl_[to] && !(bits_[to] & kOwn);
    };
    for (uint32_t i = 1; i < n; ++i)
      for (uint32_t e = edgeBegin_[i]; e < edgeBegin_[i + 1]; ++e)
        if (folds(e))
          ++revBegin[edgeDst_[e] + 1];
    for (uint32_t i = 0; i < n; ++i)
      revBegin[i + 1] += revBegin[i];
    std::vector<uint32_t> revSrc(revBegin[n]);
    {
      std::vector<uint32_t> fill(revBegin.begin(), revBegin.end() - 1);
      for (uint32_t i = 1; i < n; ++i)
        for (uint32_t e = edgeBegin_[i]; e < edgeBegin_[i + 1]; ++e)
          if (folds(e))
            revSrc[fill[edgeDst_[e]]++] = i;
    }
    std::vector<uint32_t> work;
    std::vector<uint8_t> queued(n, 1);
    work.reserve(n);
    for (uint32_t i = n; i-- > 1;)
      work.push_back(i);
    queued[0] = 0;
    while (!work.empty()) {
      const uint32_t callee = work.back();
      work.pop_back();
      queued[callee] = 0;
      const uint32_t give = bits_[callee] & ~kOwn;
      for (uint32_t r = revBegin[callee]; r < revBegin[callee + 1]; ++r) {
        const uint32_t caller = revSrc[r];
        if ((bits_[caller] | give) == bits_[caller])
          continue;
        bits_[caller] |= give;
        if (!queued[caller]) {
          queued[caller] = 1;
          work.push_back(caller);
        }
      }
    }
  }

  // ---- export ----------------------------------------------------------

  bool reportable(const clang::FunctionDecl *def) const {
    if (def->isImplicit() || def->getCanonicalDecl()->isDefaulted() ||
        def->isConsteval() || def->hasAttr<clang::AlwaysInlineAttr>() ||
        def->getParentFunctionOrMethod() || hasTargetAttr(def))
      return false;
    if (const auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(def))
      if (md->getParent()->isLambda())
        return false;
    if (ctx_.GetGVALinkageForFunction(def) != clang::GVA_DiscardableODR)
      return false;
    clang::SourceLocation loc = sm_.getExpansionLoc(def->getLocation());
    return loc.isValid() && !sm_.isInSystemHeader(loc) &&
           sm_.getFileEntryRefForID(sm_.getFileID(loc));
  }

  bool linkageName(const clang::FunctionDecl *def, std::string &out) const {
    llvm::raw_string_ostream os(out);
    if (!mangle_->shouldMangleDeclName(def)) {
      if (!def->getIdentifier())
        return false;
      os << def->getName();
      return true;
    }
    if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(def))
      mangle_->mangleName(clang::GlobalDecl(ctor, clang::Ctor_Complete), os);
    else if (const auto *dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(def))
      mangle_->mangleName(clang::GlobalDecl(dtor, clang::Dtor_Complete), os);
    else
      mangle_->mangleName(clang::GlobalDecl(def), os);
    return !out.empty();
  }

  const std::string &fileOf(clang::FileID fid) {
    auto [it, inserted] = files_.try_emplace(fid);
    if (inserted)
      if (auto entry = sm_.getFileEntryRefForID(fid))
        it->second = absoluteFileName(sm_, entry->getName());
    return it->second;
  }

  void exportFacts(GlobalIndex &index) {
    std::vector<EmittedFunction> out;
    std::string tu;
    if (auto mainFile = sm_.getFileEntryRefForID(sm_.getMainFileID()))
      tu = absoluteFileName(sm_, mainFile->getName());
    for (uint32_t i = 1; i < decl_.size(); ++i) {
      const clang::FunctionDecl *def = decl_[i];
      if (!def || !reportable(def))
        continue;
      EmittedFunction e;
      if (!linkageName(def, e.linkageName))
        continue;
      {
        llvm::raw_string_ostream os(e.qualifiedName);
        def->getNameForDiagnostic(os, ctx_.getPrintingPolicy(),
                                  /*Qualified=*/true);
        os << '(';
        bool first = true;
        for (const clang::ParmVarDecl *param : def->parameters()) {
          if (!first)
            os << ", ";
          first = false;
          os << param->getType().getAsString(ctx_.getPrintingPolicy());
        }
        os << ')';
        if (const auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(def)) {
          if (md->isConst())
            os << " const";
          if (md->getRefQualifier() == clang::RQ_LValue)
            os << " &";
          else if (md->getRefQualifier() == clang::RQ_RValue)
            os << " &&";
        }
      }
      clang::SourceLocation loc = sm_.getExpansionLoc(def->getLocation());
      e.filePath = fileOf(sm_.getFileID(loc));
      e.line = sm_.getExpansionLineNumber(loc);
      e.bodyBits = bits_[i];
      e.ownBits = own_[i];
      out.push_back(std::move(e));
    }
    if (out.empty())
      return;
    IsaLeakFacts &facts = index.mutableIsaLeakFacts();
    facts.addTu(tu, facts.addProfile(profileOf(ctx_)), std::move(out));
  }

  clang::ASTContext &ctx_;
  const clang::SourceManager &sm_;
  std::unique_ptr<clang::MangleContext> mangle_;

  // Node table (struct of arrays), one slot per function this TU reaches.
  llvm::DenseMap<const clang::FunctionDecl *, uint32_t> nodeOf_;
  std::vector<const clang::FunctionDecl *> decl_; // the definition, if any
  std::vector<const clang::VarDecl *> vars_; // set for a variable's node
  llvm::DenseMap<const clang::VarDecl *, uint32_t> varNode_;
  llvm::DenseSet<const clang::CXXRecordDecl *> vtablesDone_;
  std::vector<uint32_t> bits_;
  std::vector<uint32_t> own_; // bits_ before the fold
  // Edges in CSR form: node i's are [edgeBegin_[i], edgeBegin_[i + 1]).
  std::vector<uint32_t> edgeBegin_;
  std::vector<uint32_t> edgeDst_;
  std::vector<uint8_t> edgeBits_;

  llvm::SmallVector<const clang::Stmt *, 128> stack_;
  uint32_t cur_ = 0;
  llvm::DenseMap<clang::FileID, std::string> files_;
};

} // namespace

void collectIsaLeakFacts(clang::ASTContext &context, GlobalIndex &index) {
  Collector(context).run(index);
}

// ===========================================================================
// Index-only pass
// ===========================================================================

namespace {

// Which bodies a target feature can change.
enum FeatureClass : uint8_t {
  kFeatureVector = 1, // SIMD / floating-point units: vectorized loops, FP
  kFeatureBit = 2,    // bit manipulation: popcount, lzcnt, BMI, ...
  kFeatureOther = 4,  // reachable only through intrinsics (AES, CRC32, ...)
};

FeatureClass classifyFeature(llvm::StringRef f) {
  static const char *const kVectorPrefixes[] = {
      "sse",   "ssse3", "avx",    "fma",  "f16c",    "xop",     "amx",
      "x87",   "mmx",   "3dnow",  "evex512",
      "neon",  "sve",   "sme",    "fp-armv8", "fullfp16", "fp16", "bf16",
      "i8mm",  "dotprod", "mve",  "vfp",  "d32",     "fp64",    "altivec",
      "vsx",   "power8-vector", "power9-vector", "power10-vector", "mma",
      "simd128", "relaxed-simd", "msa", "lsx", "lasx", "vector", "zv",
      "zfh",   "zfa"};
  static const char *const kVectorExact[] = {"v", "f", "d", "q", "fp",
                                             "simd", "rdm", "complxnum",
                                             "jsconv"};
  static const char *const kBitExact[] = {"popcnt", "lzcnt", "bmi",
                                          "bmi2",   "tbm",   "abm",
                                          "movbe",  "adx",   "popcntd",
                                          "zba",    "zbb",   "zbc",
                                          "zbs",    "zbkb",  "cssc"};
  for (const char *p : kVectorExact)
    if (f == p)
      return kFeatureVector;
  for (const char *p : kBitExact)
    if (f == p)
      return kFeatureBit;
  for (const char *p : kVectorPrefixes)
    if (f.starts_with(p))
      return kFeatureVector;
  return kFeatureOther;
}

// What a body is sensitive to, as a mask over the facets and feature
// classes (the projection key below is built from it).
enum Relevance : unsigned {
  kRelFeatureVector = kFeatureVector,
  kRelFeatureBit = kFeatureBit,
  kRelFeatureOther = kFeatureOther,
  kRelFp = 1u << 3,
  kRelEh = 1u << 4,
  kRelOverflow = 1u << 5,
  kRelFpMath = 1u << 6, // the math-library keys of the fp facet
  kRelAnyFeature = kRelFeatureVector | kRelFeatureBit | kRelFeatureOther,
};

unsigned relevanceOf(uint32_t bits, unsigned facets) {
  unsigned r = 0;
  if (facets & kIsaLeakIsa) {
    if (bits & (kBodyLoop | kBodyFloat | kBodyFloatMove | kBodyVector |
                kBodyBulkMemory))
      r |= kRelFeatureVector;
    if (bits & (kBodyBitBuiltin | kBodyLoop))
      r |= kRelFeatureBit;
    if (bits & kBodyTargetBuiltin)
      r |= kRelAnyFeature;
  }
  if ((facets & kIsaLeakFp) && (bits & (kBodyFloat | kBodyVector)))
    r |= kRelFp; // moving an FP value is the same under any FP flags
  if ((facets & kIsaLeakFp) && (bits & kBodyMathCall))
    r |= kRelFpMath;
  if ((facets & kIsaLeakEh) &&
      ((bits & kBodyTry) ||
       ((bits & kBodyCleanup) && (bits & kBodyMayThrowCall))))
    r |= kRelEh;
  if ((facets & kIsaLeakOverflow) && (bits & kBodySignedArith))
    r |= kRelOverflow;
  return r;
}

// One profile seen through one relevance mask.
struct Projection {
  std::vector<std::string> features; // the classes the body is sensitive to
  std::string fp;                    // "" when not relevant
  int eh = -1;                       // -1 when not relevant
  int overflow = -1;
};

Projection project(const CodegenProfile &p, unsigned rel) {
  Projection out;
  if (rel & kRelAnyFeature)
    for (const auto &f : p.features)
      if (rel & classifyFeature(f))
        out.features.push_back(f);
  // errno and approximate functions change only math-library calls; the
  // other keys change floating-point arithmetic itself.
  if (rel & (kRelFp | kRelFpMath)) {
    llvm::SmallVector<llvm::StringRef, 16> parts;
    llvm::StringRef(p.fp).split(parts, ';', -1, /*KeepEmpty=*/false);
    for (llvm::StringRef part : parts) {
      llvm::StringRef key = part.split('=').first;
      bool mathKey = key == "errno" || key == "afn";
      if (rel & (mathKey ? kRelFpMath : kRelFp)) {
        out.fp += part;
        out.fp += ';';
      }
    }
  }
  if (rel & kRelEh)
    out.eh = p.exceptions ? 1 : 0;
  if (rel & kRelOverflow)
    out.overflow = p.signedOverflow;
  return out;
}

std::string projectionKey(const Projection &p) {
  std::string key;
  for (const auto &f : p.features) {
    key += f;
    key += ',';
  }
  key += '\x1f';
  key += p.fp;
  key += '\x1f';
  key += std::to_string(p.eh);
  key += '\x1f';
  key += std::to_string(p.overflow);
  return key;
}

// "k=v;" list -> ordered pairs.
std::vector<std::pair<std::string, std::string>> fpFields(llvm::StringRef fp) {
  std::vector<std::pair<std::string, std::string>> out;
  llvm::SmallVector<llvm::StringRef, 16> parts;
  fp.split(parts, ';', -1, /*KeepEmpty=*/false);
  for (llvm::StringRef part : parts) {
    auto [k, v] = part.split('=');
    out.emplace_back(k.str(), v.str());
  }
  return out;
}

// The command-line spelling of one floating-point setting.
std::string fpSpelling(const std::string &key, const std::string &value) {
  auto flag = [&](const char *on, const char *off) {
    return std::string(value == "1" ? on : off);
  };
  if (key == "contract") {
    static const char *const names[] = {"off", "on", "fast",
                                        "fast-honor-pragmas"};
    unsigned v = static_cast<unsigned>(std::atoi(value.c_str()));
    return std::string("-ffp-contract=") + (v < 4 ? names[v] : value.c_str());
  }
  if (key == "reassoc")
    return flag("-fassociative-math", "-fno-associative-math");
  if (key == "nnan")
    return flag("-fno-honor-nans", "-fhonor-nans");
  if (key == "ninf")
    return flag("-fno-honor-infinities", "-fhonor-infinities");
  if (key == "nsz")
    return flag("-fno-signed-zeros", "-fsigned-zeros");
  if (key == "arcp")
    return flag("-freciprocal-math", "-fno-reciprocal-math");
  if (key == "afn")
    return flag("-fapprox-func", "-fno-approx-func");
  if (key == "errno")
    return flag("-fmath-errno", "-fno-math-errno");
  if (key == "except") {
    static const char *const names[] = {"ignore", "maytrap", "strict"};
    unsigned v = static_cast<unsigned>(std::atoi(value.c_str()));
    return std::string("-ffp-exception-behavior=") +
           (v < 3 ? names[v] : value.c_str());
  }
  if (key == "eval") {
    static const char *const names[] = {"source", "double", "extended"};
    unsigned v = static_cast<unsigned>(std::atoi(value.c_str()));
    return std::string("-ffp-eval-method=") +
           (v < 3 ? names[v] : value.c_str());
  }
  if (key == "fpmath")
    return "-mfpmath=" + (value.empty() ? std::string("default") : value);
  if (key == "round")
    return value == "1" ? "rounding to nearest"
                        : "rounding mode " + value + " (-frounding-math)";
  return key + "=" + value;
}

// What in the body the differing classes `rel` change, in words.
std::string bodyPhrase(uint32_t bits, unsigned rel) {
  std::vector<const char *> parts;
  const bool vectorUnits = rel & kRelFeatureVector;
  if ((vectorUnits || (rel & kRelFeatureBit)) && (bits & kBodyLoop))
    parts.push_back("a loop");
  if ((vectorUnits || (rel & kRelFp)) && (bits & kBodyFloat))
    parts.push_back("floating-point arithmetic");
  else if (vectorUnits && (bits & kBodyFloatMove))
    parts.push_back("floating-point values");
  if ((vectorUnits || (rel & kRelFp)) && (bits & kBodyVector))
    parts.push_back("vector types");
  if (vectorUnits && (bits & kBodyBulkMemory))
    parts.push_back("memcpy/memset");
  if ((rel & kRelFeatureBit) && (bits & kBodyBitBuiltin))
    parts.push_back("bit-manipulation builtins");
  if ((rel & kRelAnyFeature) && (bits & kBodyTargetBuiltin))
    parts.push_back("target-specific builtins");
  if ((rel & kRelFpMath) && (bits & kBodyMathCall))
    parts.push_back("math-library calls");
  if (rel & kRelEh) {
    if (bits & kBodyTry)
      parts.push_back("a try block");
    else
      parts.push_back("destructors around calls that may throw");
  }
  if (rel & kRelOverflow)
    parts.push_back("signed integer arithmetic");
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i)
      out += i + 1 == parts.size() ? " and " : ", ";
    out += parts[i];
  }
  return out;
}

struct Group {
  uint32_t projection = 0;
  std::string witness; // smallest TU path
  size_t tuCount = 0;
};

} // namespace

void analyzeIsaLeaks(const GlobalIndex &index, unsigned facets,
                     std::vector<Diagnostic> &diagnostics) {
  const IsaLeakFacts &facts = index.isaLeakFacts();
  const auto &profiles = facts.profiles();
  const auto &records = facts.records();
  if (profiles.size() < 2 || records.empty())
    return;

  // Triples partition the comparison: two targets are two link units.
  // Ids follow the triple's spelling, so findings come out in the same
  // order whatever order the profiles were interned in.
  std::vector<uint32_t> tripleOf(profiles.size());
  {
    std::map<std::string, uint32_t> ids;
    for (const auto &p : profiles)
      ids.emplace(p.triple, 0);
    uint32_t next = 0;
    for (auto &kv : ids)
      kv.second = next++;
    for (size_t i = 0; i < profiles.size(); ++i)
      tripleOf[i] = ids[profiles[i].triple];
  }

  // (profile, relevance) -> projection id, computed once per pair.
  std::vector<Projection> projections;
  std::unordered_map<std::string, uint32_t> projectionIds;
  std::unordered_map<uint64_t, uint32_t> projectionOf;
  auto projectionFor = [&](uint32_t profile, unsigned rel) {
    uint64_t key = (static_cast<uint64_t>(profile) << 8) | rel;
    auto it = projectionOf.find(key);
    if (it != projectionOf.end())
      return it->second;
    Projection p = project(profiles[profile], rel);
    auto [pit, inserted] = projectionIds.try_emplace(
        projectionKey(p), static_cast<uint32_t>(projections.size()));
    if (inserted)
      projections.push_back(std::move(p));
    projectionOf.emplace(key, pit->second);
    return pit->second;
  };

  // Records in a canonical order, whatever order TUs were absorbed in.
  std::vector<uint32_t> order(records.size());
  for (uint32_t i = 0; i < order.size(); ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    const EmittedFunction &fa = records[a].function;
    const EmittedFunction &fb = records[b].function;
    return std::tie(fa.filePath, fa.line, fa.linkageName) <
           std::tie(fb.filePath, fb.line, fb.linkageName);
  });

  // (triple, projection, TU) per sighting; reused across records.
  std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> seen;
  std::vector<Group> groups;
  for (uint32_t ri : order) {
    const IsaLeakFacts::Record &entry = records[ri];
    const EmittedFunction &rec = entry.function;
    const unsigned rel = relevanceOf(rec.bodyBits, facets);
    if (!rel || entry.sightings.size() < 2)
      continue;
    seen.clear();
    for (const auto &[profile, tu] : entry.sightings)
      seen.emplace_back(tripleOf[profile], projectionFor(profile, rel), tu);
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());

    for (size_t begin = 0; begin < seen.size();) {
      const uint32_t triple = std::get<0>(seen[begin]);
      size_t end = begin;
      groups.clear();
      while (end < seen.size() && std::get<0>(seen[end]) == triple) {
        const auto &[t, proj, tu] = seen[end];
        (void)t;
        if (groups.empty() || groups.back().projection != proj)
          groups.push_back({proj, facts.tuPath(tu), 0});
        Group &g = groups.back();
        ++g.tuCount;
        if (facts.tuPath(tu) < g.witness)
          g.witness = facts.tuPath(tu);
        ++end;
      }
      begin = end;
      if (groups.size() < 2)
        continue;
      std::sort(groups.begin(), groups.end(),
                [](const Group &a, const Group &b) {
                  return std::tie(a.witness, a.projection) <
                         std::tie(b.witness, b.projection);
                });

      // Which facets actually differ between the groups.
      const Projection &first = projections[groups.front().projection];
      std::vector<std::string> common = first.features;
      bool fpDiffers = false, ehDiffers = false, ovfDiffers = false;
      for (const Group &g : groups) {
        const Projection &p = projections[g.projection];
        std::vector<std::string> both;
        std::set_intersection(common.begin(), common.end(),
                              p.features.begin(), p.features.end(),
                              std::back_inserter(both));
        common.swap(both);
        fpDiffers |= p.fp != first.fp;
        ehDiffers |= p.eh != first.eh;
        ovfDiffers |= p.overflow != first.overflow;
      }
      bool isaDiffers = false;
      for (const Group &g : groups)
        isaDiffers |= projections[g.projection].features.size() !=
                      common.size();

      // Per-group description: what it has that the others do not.
      std::vector<std::pair<std::string, std::string>> firstFp =
          fpFields(first.fp);
      std::vector<std::string> fpDifferingKeys;
      if (fpDiffers)
        for (const auto &[k, v] : firstFp)
          for (const Group &g : groups)
            if (g.projection != groups.front().projection) {
              bool differs = true;
              for (const auto &[k2, v2] :
                   fpFields(projections[g.projection].fp))
                if (k2 == k) {
                  differs = v2 != v;
                  break;
                }
              if (differs) {
                fpDifferingKeys.push_back(k);
                break;
              }
            }

      std::string groupsText;
      unsigned featureDiff = 0; // the feature classes that differ
      for (size_t gi = 0; gi < groups.size(); ++gi) {
        const Group &g = groups[gi];
        const Projection &p = projections[g.projection];
        std::vector<std::string> desc;
        if (isaDiffers) {
          std::vector<std::string> extra;
          std::set_difference(p.features.begin(), p.features.end(),
                              common.begin(), common.end(),
                              std::back_inserter(extra));
          for (const std::string &f : extra)
            featureDiff |= classifyFeature(f);
          if (extra.empty()) {
            desc.push_back("no extra target features");
          } else {
            std::string s = "target features ";
            const size_t shown = std::min<size_t>(extra.size(), 6);
            for (size_t i = 0; i < shown; ++i) {
              if (i)
                s += ',';
              s += '+';
              s += extra[i];
            }
            if (extra.size() > shown)
              s += " and " + std::to_string(extra.size() - shown) + " more";
            desc.push_back(std::move(s));
          }
        }
        if (fpDiffers) {
          std::string s;
          for (const auto &[k, v] : fpFields(p.fp))
            if (std::find(fpDifferingKeys.begin(), fpDifferingKeys.end(), k) !=
                fpDifferingKeys.end()) {
              if (!s.empty())
                s += ' ';
              s += fpSpelling(k, v);
            }
          desc.push_back(std::move(s));
        }
        if (ehDiffers)
          desc.push_back(p.eh == 1 ? "-fexceptions" : "-fno-exceptions");
        if (ovfDiffers)
          desc.push_back(p.overflow == 1   ? "-fwrapv"
                         : p.overflow == 2 ? "-ftrapv"
                                           : "signed overflow undefined");
        groupsText += gi ? "; " : "";
        groupsText += "[" + std::to_string(gi + 1) + "] " + g.witness;
        if (g.tuCount > 1)
          groupsText += " and " + std::to_string(g.tuCount - 1) + " more TU" +
                        (g.tuCount > 2 ? "s" : "");
        groupsText += " (";
        for (size_t i = 0; i < desc.size(); ++i)
          groupsText += (i ? ", " : "") + desc[i];
        groupsText += ")";
      }

      unsigned differing = 0;
      differing |= featureDiff & rel;
      for (const std::string &k : fpDifferingKeys)
        differing |= (k == "errno" || k == "afn") ? kRelFpMath : kRelFp;
      differing &= rel | ~(kRelFp | kRelFpMath);
      if (ehDiffers)
        differing |= kRelEh;
      if (ovfDiffers)
        differing |= kRelOverflow;

      std::vector<std::string> consequences;
      if (isaDiffers)
        consequences.push_back(
            "a copy built with extra target features can run on a CPU that "
            "lacks them (an illegal-instruction crash far from any SIMD "
            "code)");
      if (fpDiffers)
        consequences.push_back(
            "callers that asked for one floating-point model can get the "
            "copy compiled for another");
      if (ehDiffers)
        consequences.push_back(
            "if a -fno-exceptions copy is kept, an exception unwinding "
            "through it skips its destructors");
      if (ovfDiffers)
        consequences.push_back(
            "signed overflow wraps in one copy and is undefined or traps in "
            "another");

      std::string facetNames;
      auto addName = [&](const char *name) {
        if (!facetNames.empty())
          facetNames += ", ";
        facetNames += name;
      };
      if (isaDiffers)
        addName("isa");
      if (fpDiffers)
        addName("fp");
      if (ehDiffers)
        addName("eh");
      if (ovfDiffers)
        addName("overflow");

      // Sensitive in its own body, or only through what it inlines: the
      // second needs an optimizing build that leaves this function out of
      // line yet inlines into it, so it is a lead, not a finding.
      const bool ownBody = (relevanceOf(rec.ownBits, facets) & differing) != 0;
      Diagnostic diag;
      diag.kind = ownBody ? Diagnostic::IsaLeak_Divergent
                          : Diagnostic::IsaLeak_Inlined;
      diag.checkName = "isa-leak";
      diag.file = rec.filePath;
      diag.callLocation = rec.filePath + ":" + std::to_string(rec.line);
      diag.resolvedDecl = rec.qualifiedName;
      // The triple of this partition, from any of its sightings.
      diag.entities = {rec.linkageName, std::string()};
      for (const auto &[profile, tu] : entry.sightings)
        if (tripleOf[profile] == triple) {
          diag.entities[1] = profiles[profile].triple;
          break;
        }
      diag.message =
          "'" + rec.qualifiedName +
          "' has vague linkage and is emitted by TUs built with different "
          "code generation (" +
          facetNames + "): " + groupsText +
          ". The linker keeps one copy of '" + rec.linkageName +
          "' for every caller, picked by link order; its body has " +
          bodyPhrase(rec.bodyBits, differing) +
          (ownBody ? " (including what it can inline)"
               : " only through functions it can inline (a copy built "
                 "without inlining may not differ)") +
          ", which those settings compile differently, so ";
      for (size_t i = 0; i < consequences.size(); ++i) {
        if (i)
          diag.message += i + 1 == consequences.size() ? ", and " : ", ";
        diag.message += consequences[i];
      }
      diag.message +=
          ". Harmless only where every call is inlined. Build it the same "
          "way in every TU or give it internal linkage (a static or "
          "anonymous-namespace copy) in the TUs with special flags";
      diag.message += isaDiffers ? "; for target features, a target "
                                   "attribute also pins it."
                                 : ".";
      diagnostics.push_back(std::move(diag));
    }
  }
}

} // namespace vycor
