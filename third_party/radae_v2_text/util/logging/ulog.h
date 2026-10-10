//==========================================================================
// Name:            ulog.h  (AetherSDR shim)
//
// Purpose:         Satisfy rade_text.cpp's `#include "../util/logging/ulog.h"`
//                  without vendoring freedv-backend's whole logging subsystem.
//
// This file is NOT upstream code. Everything else under
// third_party/radae_v2_text/pipeline/ is byte-identical to
// tmiw/freedv-backend, and this shim is what keeps it that way: the vendored
// tree mirrors upstream's directory shape (pipeline/ beside util/logging/) so
// the relative include resolves here instead of needing the source edited.
// A re-vendor is therefore a straight copy with no patch step.
//
// rade_text.cpp uses exactly two of ulog's macros -- log_debug (x5) and
// log_info (x1), both printf-style varargs. Vendoring ulog.c plus
// ulog_async.cpp (~1000 lines, its own thread and ring buffer) to serve six
// call sites would be the wrong trade: AetherSDR already has a logging
// system, and two macros is a smaller seam than two logging frameworks.
//
// Routed to the RADE V2 text category so these lines obey the same runtime
// filtering as the rest of the codec's logging rather than printing
// unconditionally. log_debug -> qCDebug, log_info -> qCInfo: upstream's
// log_info is the decoded-callsign line, which is genuinely informational.
//
// SPDX-License-Identifier: BSD-2-Clause
//==========================================================================

#ifndef AETHER_RADE_V2_TEXT_ULOG_SHIM_H
#define AETHER_RADE_V2_TEXT_ULOG_SHIM_H

#include <QLoggingCategory>

// Declared here and defined in src/core/RADEV2Engine.cpp alongside the other
// RADE V2 categories, so there is one place that registers them all.
Q_DECLARE_LOGGING_CATEGORY(lcRadeV2Text)

// Qt's categorised logging takes a printf-style format directly, so the
// varargs pass straight through. The do/while wrapper keeps these usable as
// statements in an if/else without braces, which upstream's macros also are.
#define log_debug(...) \
    do { qCDebug(lcRadeV2Text, __VA_ARGS__); } while (0)
#define log_info(...) \
    do { qCInfo(lcRadeV2Text, __VA_ARGS__); } while (0)

// Not used by rade_text.cpp today. Defined anyway so a future re-vendor that
// starts calling them compiles rather than failing on a missing macro -- and
// mapped to real severities so such a call is not silently swallowed.
#define log_trace(...) do { qCDebug(lcRadeV2Text, __VA_ARGS__); } while (0)
#define log_warn(...)  do { qCWarning(lcRadeV2Text, __VA_ARGS__); } while (0)
#define log_error(...) do { qCCritical(lcRadeV2Text, __VA_ARGS__); } while (0)
#define log_fatal(...) do { qCCritical(lcRadeV2Text, __VA_ARGS__); } while (0)

#endif // AETHER_RADE_V2_TEXT_ULOG_SHIM_H
