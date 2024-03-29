// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_KEYBOARD_TEST_TEST_KEYBOARD_CONTROLLER_OBSERVER_H_
#define UI_KEYBOARD_TEST_TEST_KEYBOARD_CONTROLLER_OBSERVER_H_

#include "ui/keyboard/keyboard_controller_observer.h"

namespace keyboard {

// A KeyboardControllerObserver that counts occurrences of events for testing.
struct TestKeyboardControllerObserver : public KeyboardControllerObserver {
  TestKeyboardControllerObserver();
  ~TestKeyboardControllerObserver() override;

  // KeyboardControllerObserver:
  void OnKeyboardEnabledChanged(bool is_enabled) override;

  int enabled_count = 0;
  int disabled_count = 0;
  DISALLOW_COPY_AND_ASSIGN(TestKeyboardControllerObserver);
};

}  // namespace keyboard

#endif  // UI_KEYBOARD_TEST_TEST_KEYBOARD_CONTROLLER_OBSERVER_H_
