// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_RENDERER_V8_UNWINDER_H_
#define CHROME_RENDERER_V8_UNWINDER_H_

#include "base/containers/flat_set.h"
#include "base/profiler/unwinder.h"
#include "v8/include/v8.h"

// Implements stack frame unwinding for V8 generated code frames, for use with
// the StackSamplingProfiler.
class V8Unwinder : public base::Unwinder {
 public:
  explicit V8Unwinder(const v8::UnwindState& unwind_state);
  ~V8Unwinder() override;

  V8Unwinder(const V8Unwinder&) = delete;
  V8Unwinder& operator=(const V8Unwinder&) = delete;

  // Unwinder:
  void AddNonNativeModules(base::ModuleCache* module_cache) override;
  bool CanUnwindFrom(const base::Frame* current_frame) const override;
  base::UnwindResult TryUnwind(base::RegisterContext* thread_context,
                               uintptr_t stack_top,
                               base::ModuleCache* module_cache,
                               std::vector<base::Frame>* stack) const override;

 private:
  const v8::UnwindState unwind_state_;
  base::flat_set<const base::ModuleCache::Module*> v8_modules_;
};

#endif  // CHROME_RENDERER_V8_UNWINDER_H_