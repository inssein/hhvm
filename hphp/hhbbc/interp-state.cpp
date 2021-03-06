/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#include "hphp/hhbbc/interp-state.h"

#include <string>

#include "folly/Format.h"
#include "folly/Conv.h"

#include "hphp/hhbbc/analyze.h"

namespace HPHP { namespace HHBBC {

//////////////////////////////////////////////////////////////////////

bool operator==(const ActRec& a, const ActRec& b) {
  auto const fsame =
    a.func.hasValue() != b.func.hasValue() ? false :
    a.func.hasValue() ? a.func->same(*b.func) :
    true;
  return a.kind == b.kind && fsame;
}

bool operator==(const State& a, const State& b) {
  return a.initialized == b.initialized &&
    a.thisAvailable == b.thisAvailable &&
    a.locals == b.locals &&
    a.stack == b.stack &&
    a.fpiStack == b.fpiStack;
}

bool operator!=(const ActRec& a, const ActRec& b) { return !(a == b); }
bool operator!=(const State& a, const State& b)   { return !(a == b); }

State without_stacks(const State& src) {
  auto ret          = State{};
  ret.initialized   = src.initialized;
  ret.thisAvailable = src.thisAvailable;
  ret.locals        = src.locals;
  return ret;
}

//////////////////////////////////////////////////////////////////////

PropertiesInfo::PropertiesInfo(const Index& index,
                               Context ctx,
                               ClassAnalysis* cls)
  : m_cls(cls)
{
  if (m_cls == nullptr && ctx.cls != nullptr) {
    m_privateProperties = index.lookup_private_props(ctx.cls);
    m_privateStatics    = index.lookup_private_statics(ctx.cls);
  }
}

PropState& PropertiesInfo::privateProperties() {
  if (m_cls != nullptr) {
    return m_cls->privateProperties;
  }
  return m_privateProperties;
}

PropState& PropertiesInfo::privateStatics() {
  if (m_cls != nullptr) {
    return m_cls->privateStatics;
  }
  return m_privateStatics;
}

const PropState& PropertiesInfo::privateProperties() const {
  return const_cast<PropertiesInfo*>(this)->privateProperties();
}

const PropState& PropertiesInfo::privateStatics() const {
  return const_cast<PropertiesInfo*>(this)->privateStatics();
}

//////////////////////////////////////////////////////////////////////

bool merge_into(PropState& dst, const PropState& src) {
  assert(dst.size() == src.size());

  auto changed = false;

  auto dstIt = begin(dst);
  auto srcIt = begin(src);
  for (; dstIt != end(dst); ++dstIt, ++srcIt) {
    assert(srcIt != end(src));
    assert(srcIt->first == dstIt->first);
    auto const newT = union_of(dstIt->second, srcIt->second);
    if (newT != dstIt->second) {
      changed = true;
      dstIt->second = newT;
    }
  }

  return changed;
}

bool merge_into(ActRec& dst, const ActRec& src) {
  if (dst.kind != src.kind) {
    dst = ActRec { FPIKind::Unknown };
    return true;
  }
  if (dst != src) {
    dst = ActRec { src.kind };
    return true;
  }
  return false;
}

bool merge_into(State& dst, const State& src) {
  if (!dst.initialized) {
    dst = src;
    return true;
  }

  assert(src.initialized);
  assert(dst.locals.size() == src.locals.size());
  assert(dst.stack.size() == src.stack.size());
  assert(dst.fpiStack.size() == src.fpiStack.size());

  auto changed = false;

  auto const available = dst.thisAvailable && src.thisAvailable;
  if (available != dst.thisAvailable) {
    changed = true;
    dst.thisAvailable = available;
  }

  for (auto i = size_t{0}; i < dst.stack.size(); ++i) {
    auto const newT = union_of(dst.stack[i], src.stack[i]);
    if (dst.stack[i] != newT) {
      changed = true;
      dst.stack[i] = newT;
    }
  }

  for (auto i = size_t{0}; i < dst.locals.size(); ++i) {
    auto const newT = union_of(dst.locals[i], src.locals[i]);
    if (dst.locals[i] != newT) {
      changed = true;
      dst.locals[i] = newT;
    }
  }

  for (auto i = size_t{0}; i < dst.fpiStack.size(); ++i) {
    if (merge_into(dst.fpiStack[i], src.fpiStack[i])) {
      changed = true;
    }
  }

  return changed;
}

//////////////////////////////////////////////////////////////////////

static std::string fpiKindStr(FPIKind k) {
  switch (k) {
  case FPIKind::Unknown:     return "unk";
  case FPIKind::CallableArr: return "arr";
  case FPIKind::Func:        return "func";
  case FPIKind::Ctor:        return "ctor";
  case FPIKind::ObjMeth:     return "objm";
  case FPIKind::ClsMeth:     return "clsm";
  case FPIKind::ObjInvoke:   return "invoke";
  }
  not_reached();
}

std::string show(const ActRec& a) {
  return folly::to<std::string>(
    "ActRec { ",
    fpiKindStr(a.kind),
    a.func ? (": " + show(*a.func)) : std::string{},
    " }"
  );
}

std::string state_string(const php::Func& f, const State& st) {
  std::string ret;

  if (!st.initialized) {
    ret = "state: uninitialized\n";
    return ret;
  }

  ret = "state:\n";
  if (f.cls) {
    ret += folly::format("thisAvailable({})\n", st.thisAvailable).str();
  }
  for (auto i = size_t{0}; i < st.locals.size(); ++i) {
    ret += folly::format("${: <8} :: {}\n",
      f.locals[i]->name
        ? std::string(f.locals[i]->name->data())
        : folly::format("<unnamed{}>", i).str(),
      show(st.locals[i])
    ).str();
  }

  for (auto i = size_t{0}; i < st.stack.size(); ++i) {
    ret += folly::format("stk[{:02}] :: {}\n",
      i,
      show(st.stack[i])
    ).str();
  }

  return ret;
}

std::string property_state_string(const PropertiesInfo& props) {
  std::string ret;

  ret += "properties:\n";
  for (auto& kv : props.privateProperties()) {
    ret += folly::format("$this->{: <14} :: {}\n",
      kv.first->data(),
      show(kv.second)
    ).str();
  }
  for (auto& kv : props.privateStatics()) {
    ret += folly::format("self::${: <14} :: {}\n",
      kv.first->data(),
      show(kv.second)
    ).str();
  }

  return ret;
}

//////////////////////////////////////////////////////////////////////

}}

