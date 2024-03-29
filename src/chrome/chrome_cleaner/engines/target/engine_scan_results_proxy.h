// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_CHROME_CLEANER_ENGINES_TARGET_ENGINE_SCAN_RESULTS_PROXY_H_
#define CHROME_CHROME_CLEANER_ENGINES_TARGET_ENGINE_SCAN_RESULTS_PROXY_H_

#include "base/memory/ref_counted.h"
#include "base/single_thread_task_runner.h"
#include "chrome/chrome_cleaner/interfaces/engine_sandbox.mojom.h"
#include "chrome/chrome_cleaner/pup_data/pup_data.h"

namespace chrome_cleaner {

// Accessors to send the scan results over the Mojo connection.
class EngineScanResultsProxy
    : public base::RefCountedThreadSafe<EngineScanResultsProxy> {
 public:
  EngineScanResultsProxy(
      mojom::EngineScanResultsAssociatedPtr scan_results_ptr,
      scoped_refptr<base::SingleThreadTaskRunner> task_runner);

  scoped_refptr<base::SingleThreadTaskRunner> task_runner() const {
    return task_runner_;
  }

  void UnbindScanResultsPtr();

  // Notifies the broker process that UwS was found. Will be called on an
  // arbitrary thread from the sandboxed engine.
  virtual void FoundUwS(UwSId pup_id, const PUPData::PUP& pup);

  // Notifies the broker process that scan is done. Will be called on an
  // arbitrary thread from the sandboxed engine.
  virtual void ScanDone(uint32_t result);

 protected:
  virtual ~EngineScanResultsProxy();

 private:
  friend class base::RefCountedThreadSafe<EngineScanResultsProxy>;

  // Invokes scan_results_ptr_->FoundUwS from the IPC thread.
  void OnFoundUwS(UwSId pup_id, const PUPData::PUP& pup);

  // Invokes scan_results_ptr_->Done from the IPC thread.
  void OnDone(uint32_t result);

  // An EngineScanResults that will send the results over the Mojo connection.
  mojom::EngineScanResultsAssociatedPtr scan_results_ptr_;

  // A task runner for the IPC thread.
  scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
};

}  // namespace chrome_cleaner

#endif  // CHROME_CHROME_CLEANER_ENGINES_TARGET_ENGINE_SCAN_RESULTS_PROXY_H_
