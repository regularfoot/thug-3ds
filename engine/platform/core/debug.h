#ifndef __CORE_DEBUG_H
#define __CORE_DEBUG_H
// Debug is a native platform service. Never let the legacy SDK macros install
// GNU token-pasting or change assertion-dependent class layouts per TU.
#include <core/defines.h>
#include <source_port/diagnostics.h>
#define Dbg_SetScreenAssert(on) do { Dbg::screen_assert(on); } while (0)
#endif
