#pragma once

//! @file contract.hpp
//! @brief OMNICPP_CONTRACT — engine-wide body-level invariant macro.
//!
//! C++26 contracts (P2900) attach to function signatures (pre/post), not to
//! statements, so they cannot express body checks at this macro's call sites.
//! This facility is therefore a two-mode macro:
//!
//!   - Default (check): a violation prints the expression, file, and line to
//!     stderr and aborts. Deliberately NOT tied to NDEBUG — these guard
//!     memory-safety invariants (mapped-pointer validity, slot bounds,
//!     lifetime rules) that must stay load-bearing in optimized builds.
//!   - OMNICPP_CONTRACT_MODE_ASSUME: compiles to `[[assume]]` (C++23) —
//!     zero runtime cost, UB on violation. Only for measured hot paths,
//!     enabled per-directory or per-target, never globally.
//!   - OMNICPP_CONTRACT_MODE_OFF: compiled out entirely (fuzzing builds that
//!     supply adversarial-but-tolerated inputs).
//!
//! Signature-level pre/post contracts (GCC 16 `-fcontracts`, the only
//! shipping implementation as of September 2026) can be authored directly in
//! headers that are only built on the GCC path, e.g. the reflection module.

#include <cstdio>
#include <cstdlib>

namespace warploom::contract {

//! Terminate on a violated invariant. [[noreturn]] lets the compiler prune
//! the guarded path entirely, so the check costs one predictable branch.
[[noreturn]] inline void violate(const char* expression, const char* file,
                                 int line) noexcept {
  std::fprintf(stderr, "omnicpp contract violation: %s (%s:%d)\n", expression,
               file, line);
  std::abort();
}

}  // namespace warploom::contract

#if defined(OMNICPP_CONTRACT_MODE_ASSUME)

#define OMNICPP_CONTRACT(...) [[assume(__VA_ARGS__)]]

#elif defined(OMNICPP_CONTRACT_MODE_OFF)

#define OMNICPP_CONTRACT(...)

#else

#define OMNICPP_CONTRACT(...)                     \
  do {                                            \
    if (!(__VA_ARGS__)) {                         \
      ::warploom::contract::violate(#__VA_ARGS__, __FILE__, __LINE__); \
    }                                             \
  } while (false)

#endif
// S2-B compat footer: legacy `omnicpp::*` spellings keep resolving
// during the transition (removed with the S5 identity pass -
// docs/warploom-core-plan.md, phase B). A using-directive in a
// namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types, and legally coexists
// with real `omnicpp::core` extension blocks elsewhere (extension
// blocks merge). One directive per namespace THIS header declares,
// each under its OWN guard (a shared guard would suppress later
// headers' distinct directives).
#ifndef OMNICPP_COMPAT_CONTRACT_NS
#define OMNICPP_COMPAT_CONTRACT_NS
namespace omnicpp::contract {
    using namespace ::warploom::contract;
}
#endif  // OMNICPP_COMPAT_CONTRACT_NS
