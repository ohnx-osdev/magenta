// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

// This header defines thread annotation macros to be used everywhere in Magenta
// outside of publicly exposed headers. See system/public/magenta/compiler.h for
// the publicly exported macros.

// The thread safety analysis system is documented at http://clang.llvm.org/docs/ThreadSafetyAnalysis.html
// and its use in Magenta is documented at docs/thread_annotations.md.  The macros we
// use are:
//
// TA_CAP(x)                    |x| is the capability this type represents, e.g. "mutex".
// TA_GUARDED(x)                the annotated variable is guarded by the capability (e.g. lock) |x|
// TA_ACQ(x)                    function acquires the mutex |x|
// TA_REL(x)                    function releases the mutex |x|
// TA_REQ(x)                    function requires that the caller hold the mutex |x|
// TA_RET_CAP(x)                function returns a reference to the mutex |x|
// TA_SCOPED_CAP                type represents a scoped or RAII-style wrapper around a capability
// TA_NO_THREAD_SAFETY_ANALYSIS function is excluded entirely from thread safety analysis

#ifdef __clang__
#define THREAD_ANNOTATION(x) __attribute__((x))
#else
#define THREAD_ANNOTATION(x)
#endif

#define TA_CAP(x) THREAD_ANNOTATION(capability(x))
#define TA_GUARDED(x) THREAD_ANNOTATION(guarded_by(x))
#define TA_ACQ(x) THREAD_ANNOTATION(acquire_capability(x))
#define TA_REL(x) THREAD_ANNOTATION(release_capability(x))
#define TA_REQ(x) THREAD_ANNOTATION(requires_capability(x))
#define TA_RET_CAP(x) THREAD_ANNOTATION(lock_returned(x))
#define TA_SCOPED_CAP THREAD_ANNOTATION(scoped_lockable)
#define TA_NO_THREAD_SAFETY_ANALYSIS THREAD_ANNOTATION(no_thread_safety_analysis)
