//
// Copyright 2019 Google LLC
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
//

#ifndef DIFFERENTIAL_PRIVACY_ALGORITHMS_BOUNDED_SUM_H_
#define DIFFERENTIAL_PRIVACY_ALGORITHMS_BOUNDED_SUM_H_

#include <limits>

#include "google/protobuf/any.pb.h"
#include "absl/memory/memory.h"
#include "differential_privacy/algorithms/algorithm.h"
#include "differential_privacy/algorithms/approx-bounds.h"
#include "differential_privacy/algorithms/bounded-algorithm.h"
#include "differential_privacy/algorithms/numerical-mechanisms.h"
#include "differential_privacy/algorithms/util.h"
#include "differential_privacy/proto/summary.pb.h"
#include "differential_privacy/base/status.h"

namespace differential_privacy {

// Incrementally provides a differentially private sum, clamped between upper
// and lower values. Bounds can be manually set or privately inferred.
template <typename T,
          typename std::enable_if<std::is_integral<T>::value ||
                                  std::is_floating_point<T>::value>::type* =
              nullptr>
class BoundedSum : public Algorithm<T> {
 public:
  // Builder for BoundedSum algorithm.
  class Builder : public BoundedAlgorithmBuilder<T, BoundedSum<T>, Builder> {
    using AlgorithmBuilder =
        differential_privacy::AlgorithmBuilder<T, BoundedSum<T>, Builder>;
    using BoundedBuilder = BoundedAlgorithmBuilder<T, BoundedSum<T>, Builder>;

   public:
    // Check that bounds are appropriate.
    static base::Status CheckLowerBound(T lower) {
      if (lower < -1 * std::numeric_limits<T>::max()) {
        return base::InvalidArgumentError(
            "Lower bound cannot be higher in magnitude than the max "
            "numeric limit. If manually bounding, please increase it by "
            "at least 1.");
      }
      return base::OkStatus();
    }

   private:
    base::StatusOr<std::unique_ptr<BoundedSum<T>>> BuildAlgorithm() override {
      // Ensure that either bounds are manually set or ApproxBounds is made.
      RETURN_IF_ERROR(BoundedBuilder::BoundsSetup());

      // If manual bounding, construct mechanism so we can fail on build if
      // sensitivity is inappropriate.
      std::unique_ptr<LaplaceMechanism> mechanism = nullptr;
      if (BoundedBuilder::has_upper_ && BoundedBuilder::has_lower_) {
        RETURN_IF_ERROR(CheckLowerBound(BoundedBuilder::lower_));
        ASSIGN_OR_RETURN(mechanism, AlgorithmBuilder::laplace_mechanism_builder_
                                        ->SetEpsilon(AlgorithmBuilder::epsilon_)
                                        .SetSensitivity(std::max(
                                            std::abs(BoundedBuilder::lower_),
                                            std::abs(BoundedBuilder::upper_)))
                                        .Build());
      }

      // Construct BoundedSum.
      auto mech_builder = AlgorithmBuilder::laplace_mechanism_builder_->Clone();
      return absl::WrapUnique(new BoundedSum(
          AlgorithmBuilder::epsilon_, BoundedBuilder::lower_,
          BoundedBuilder::upper_, std::move(mech_builder), std::move(mechanism),
          std::move(BoundedBuilder::approx_bounds_)));
    }
  };

  void AddEntry(const T& t) override {
    if (std::isnan(t)) {
      return;
    }

    // If manual bounds are set, clamp immediately and store sum. Otherwise,
    // feed inputs into ApproxBounds and store temporary partial sums.
    if (!approx_bounds_) {
      pos_sum_[0] += Clamp<T>(lower_, upper_, t);
    } else {
      approx_bounds_->AddEntry(t);

      // Find partial sums.
      if (t >= 0) {
        approx_bounds_->template AddToPartialSums<T>(&pos_sum_, t);
      } else {
        approx_bounds_->template AddToPartialSums<T>(&neg_sum_, t);
      }
    }
  }

  base::StatusOr<Output> GenerateResult(double privacy_budget) override {
    DCHECK_GT(privacy_budget, 0.0)
        << "Privacy budget should be greater than zero.";
    if (privacy_budget == 0.0) return Output();

    Output output;
    double sum = 0;
    double remaining_budget = privacy_budget;

    if (approx_bounds_) {
      // Use a fraction of the privacy budget to find the approximate bounds.
      // Analysis for choosing the fraction in
      // (broken link)
      double bounds_budget = privacy_budget / 2;
      remaining_budget -= bounds_budget;
      ASSIGN_OR_RETURN(Output bounds,
                       approx_bounds_->GenerateResult(bounds_budget));
      T lower = GetValue<T>(bounds.elements(0).value());
      T upper = GetValue<T>(bounds.elements(1).value());
      RETURN_IF_ERROR(Builder::CheckLowerBound(lower));

      // Since sensitivity is determined only by the larger-magnitude bound,
      // set the smaller-magnitude bound to be the negative of the larger. This
      // minimizes clamping and so maximizes accuracy.
      lower_ = std::min(lower, -1 * upper);
      upper_ = std::max(upper, -1 * lower);

      // To find the sum, pass the identity function as the transform. We pass
      // count = 0 because the count should never be used.
      sum = approx_bounds_->template ComputeFromPartials<T>(
          pos_sum_, neg_sum_, [](T x) { return x; }, lower_, upper_, 0);

      // Populate the bounding report with ApproxBounds information.
      *(output.mutable_error_report()->mutable_bounding_report()) =
          approx_bounds_->GetBoundingReport(lower_, upper_);

      // Clear the mechanism. The sensitivity might have changed.
      mechanism_.reset();
    } else {
      // Manual bounds were set and clamping was done upon adding entries.
      sum = pos_sum_[0];
    }

    // Construct mechanism if needed. Mechanism is already constructed if
    // NoiseConfidenceInterval() was called with manual bounds.
    RETURN_IF_ERROR(BuildMechanism());

    // Add noise confidence interval to the error report.
    base::StatusOr<ConfidenceInterval> interval =
        NoiseConfidenceIntervalImpl(kDefaultConfidenceLevel, remaining_budget);
    if (interval.ok()) {
      *(output.mutable_error_report()->mutable_noise_confidence_interval()) =
          interval.ValueOrDie();
    }

    // Add noise to sum. Use the remaining privacy budget.
    double noisy_sum = mechanism_->AddNoise(sum, remaining_budget);
    if (std::is_integral<T>::value) {
      AddToOutput<T>(&output, std::round(noisy_sum));
    } else {
      AddToOutput<T>(&output, noisy_sum);
    }
    return output;
  }

  // Only return noise confidence interval for manually set bounds, since it is
  // dynamic upon result generation for auto-bounds.
  base::StatusOr<ConfidenceInterval> NoiseConfidenceInterval(
      double confidence_level, double privacy_budget = 1) override {
    if (approx_bounds_) {
      return base::InvalidArgumentError(
          "NoiseConfidenceInterval changes per result generation for "
          "automatically-determined sensitivity.");
    }
    return NoiseConfidenceIntervalImpl(confidence_level, privacy_budget);
  }

  void ResetState() override {
    std::fill(pos_sum_.begin(), pos_sum_.end(), 0);
    std::fill(neg_sum_.begin(), neg_sum_.end(), 0);
    if (approx_bounds_) {
      approx_bounds_->ResetState();
      mechanism_ = nullptr;
    }
  }

  T lower() { return lower_; }
  T upper() { return upper_; }

  Summary Serialize() override {
    // Create BoundedSumSummary.
    BoundedSumSummary bs_summary;
    for (T x : pos_sum_) {
      SetValue(bs_summary.add_pos_sum(), x);
    }
    for (T x : neg_sum_) {
      SetValue(bs_summary.add_neg_sum(), x);
    }
    if (approx_bounds_) {
      Summary approx_bounds_summary = approx_bounds_->Serialize();
      approx_bounds_summary.data().UnpackTo(
          bs_summary.mutable_bounds_summary());
    }

    // Create Summary.
    Summary summary;
    summary.mutable_data()->PackFrom(bs_summary);
    return summary;
  }

  base::Status Merge(const Summary& summary) override {
    if (!summary.has_data()) {
      return base::InvalidArgumentError(
          "Cannot merge summary with no bounded sum data.");
    }

    // Add bounded sum partial values.
    BoundedSumSummary bs_summary;
    if (!summary.data().UnpackTo(&bs_summary)) {
      return base::InvalidArgumentError(
          "Bounded sum summary unable to be unpacked.");
    }
    if (pos_sum_.size() != bs_summary.pos_sum_size() ||
        neg_sum_.size() != bs_summary.neg_sum_size()) {
      return base::InvalidArgumentError(
          "Merged BoundedSum must have the same amount of partial sum "
          "values as this BoundedSum.");
    }
    for (int i = 0; i < pos_sum_.size(); ++i) {
      pos_sum_[i] += GetValue<T>(bs_summary.pos_sum(i));
    }
    for (int i = 0; i < neg_sum_.size(); ++i) {
      neg_sum_[i] += GetValue<T>(bs_summary.neg_sum(i));
    }
    if (approx_bounds_) {
      Summary approx_bounds_summary;
      approx_bounds_summary.mutable_data()->PackFrom(
          bs_summary.bounds_summary());
      RETURN_IF_ERROR(approx_bounds_->Merge(approx_bounds_summary));
    }
    return base::OkStatus();
  }

  int64_t MemoryUsed() override {
    int64_t memory = sizeof(BoundedSum<T>) +
                   sizeof(T) * (pos_sum_.capacity() + neg_sum_.capacity());
    if (approx_bounds_) {
      memory += approx_bounds_->MemoryUsed();
    }
    if (mechanism_) {
      memory += mechanism_->MemoryUsed();
    }
    if (laplace_mechanism_builder_) {
      memory += sizeof(*laplace_mechanism_builder_);
    }
    return memory;
  }

 protected:
  // Protected constructor to allow for testing.
  BoundedSum(double epsilon, T lower, T upper,
             std::unique_ptr<LaplaceMechanism::Builder> mechanism_builder,
             std::unique_ptr<LaplaceMechanism> mechanism,
             std::unique_ptr<ApproxBounds<T>> approx_bounds = nullptr)
      : Algorithm<T>(epsilon),
        lower_(lower),
        upper_(upper),
        laplace_mechanism_builder_(std::move(mechanism_builder)),
        mechanism_(std::move(mechanism)),
        approx_bounds_(std::move(approx_bounds)) {
    // If automatically determining bounds, we need partial values for each bin
    // of the ApproxBounds logarithmic histogram. Otherwise, we only need to
    // store one already-clamped value.
    if (approx_bounds_) {
      pos_sum_.resize(approx_bounds_->NumPositiveBins(), 0);
      neg_sum_.resize(approx_bounds_->NumPositiveBins(), 0);
    } else {
      pos_sum_.push_back(0);
    }
  }

 private:
  base::Status BuildMechanism() {
    if (!mechanism_) {
      ASSIGN_OR_RETURN(
          mechanism_,
          laplace_mechanism_builder_->SetEpsilon(Algorithm<T>::GetEpsilon())
              .SetSensitivity(std::max(std::abs(lower_), std::abs(upper_)))
              .Build());
    }
    return base::OkStatus();
  }

  base::StatusOr<ConfidenceInterval> NoiseConfidenceIntervalImpl(
      double confidence_level, double privacy_budget = 1) {
    if (!mechanism_) {
      return base::InvalidArgumentError(
          "Mechanism not yet constructed. Try getting noise confidence "
          "interval after generating result.");
    }
    return mechanism_->NoiseConfidenceInterval(confidence_level,
                                               privacy_budget);
  }

  // Vectors of partial values stored for automatic clamping.
  std::vector<T> pos_sum_, neg_sum_;

  // If manually set, these values are determined upon construction. Otherwise,
  // they are found in GenerateResult().
  T lower_, upper_;

  // Used to construct mechanism once bounds are obtained for auto-bounding.
  std::unique_ptr<LaplaceMechanism::Builder> laplace_mechanism_builder_;

  // Will be available upon BoundedSum for manual bounding, and constructed upon
  // GenerateResult for auto-bounding.
  std::unique_ptr<LaplaceMechanism> mechanism_;

  // If this is not nullptr, we are automatically determining bounds. Otherwise,
  // lower and upper contain the manually set bounds.
  std::unique_ptr<ApproxBounds<T>> approx_bounds_;
};

}  // namespace differential_privacy

#endif  // DIFFERENTIAL_PRIVACY_ALGORITHMS_BOUNDED_SUM_H_
