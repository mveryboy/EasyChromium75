// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/events/blink/prediction/least_squares_predictor.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/events/blink/prediction/input_predictor_unittest_helpers.h"

namespace ui {
namespace test {

class LSQPredictorTest : public InputPredictorTest {
 public:
  explicit LSQPredictorTest() {}

  void SetUp() override {
    predictor_ = std::make_unique<ui::LeastSquaresPredictor>();
  }

  DISALLOW_COPY_AND_ASSIGN(LSQPredictorTest);
};

TEST_F(LSQPredictorTest, ShouldHasPrediction) {
  LeastSquaresPredictor predictor;
  for (size_t i = 0; i < LeastSquaresPredictor::kSize; i++) {
    // First |kSize| point do not have prediction available.
    EXPECT_FALSE(predictor.HasPrediction());
    InputPredictor::InputData data = {gfx::PointF(1, 1),
                                      FromMilliseconds(8 * i)};
    predictor.Update(data);
  }
  EXPECT_TRUE(predictor.HasPrediction());
}

// Tests the lest squares filter behavior.
// The data set is generated by a "known to work" quadratic fit.
TEST_F(LSQPredictorTest, PredictedValue) {
  std::vector<double> x = {22, 58, 102, 108.094};
  std::vector<double> y = {100, 100, 100, 100};
  std::vector<double> t = {13, 21, 37, 42};
  ValidatePredictor(x, y, t);

  x = {100, 100, 101, 104.126};
  y = {120, 280, 600, 1364.93};
  t = {101, 126, 148, 180};
  ValidatePredictor(x, y, t);
}

// Tests the LSQ predictor predict constant velocity.
TEST_F(LSQPredictorTest, PredictLinearValue) {
  std::vector<double> x = {0, 4, 10, 15, 20, 28, 30, 38};
  std::vector<double> y = {30, 34, 40, 45, 50, 58, 60, 68};
  std::vector<double> t = {0, 4, 10, 15, 20, 28, 30, 38};
  ValidatePredictor(x, y, t);
}

// Tests the LSQ predictor predict quadratic value correctly.
TEST_F(LSQPredictorTest, PredictQuadraticValue) {
  std::vector<double> x = {2, 8, 18, 32, 50};
  std::vector<double> y = {100, 400, 900, 1600, 2500};
  std::vector<double> t = {8, 16, 24, 32, 40};
  ValidatePredictor(x, y, t);
}

// Tests that lsq predictor will not crash when given constant time stamp.
TEST_F(LSQPredictorTest, ConstantTimeStampNotCrash) {
  std::vector<double> x = {100, 101, 102};
  std::vector<double> y = {101, 102, 103};
  std::vector<double> t = {0, 0, 0};
  for (size_t i = 0; i < t.size(); i++) {
    InputPredictor::InputData data = {gfx::PointF(x[i], y[i]),
                                      FromMilliseconds(t[i])};
    predictor_->Update(data);
  }
  ui::InputPredictor::InputData result;
  EXPECT_FALSE(predictor_->GeneratePrediction(
      FromMilliseconds(42), false /* is_resampling */, &result));

  x = {100, 100, 100};
  y = {100, 100, 100};
  t = {100, 100, 100};
  for (size_t i = 0; i < t.size(); i++) {
    InputPredictor::InputData data = {gfx::PointF(x[i], y[i]),
                                      FromMilliseconds(t[i])};
    predictor_->Update(data);
  }
  EXPECT_FALSE(predictor_->GeneratePrediction(
      FromMilliseconds(42), false /* is_resampling */, &result));
}

}  // namespace test
}  // namespace ui