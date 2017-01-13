#include "solvers/irls_map_solver.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "image/image_data.h"
#include "image_model/image_model.h"

#include "alglib/src/stdafx.h"
#include "alglib/src/optimization.h"

#include "opencv2/core/core.hpp"

#include "glog/logging.h"

namespace super_resolution {

// The objective function used by the ALGLIB solver to compute residuals. This
// version uses analyitical differentiation, meaning that the gradient is
// computed manually.
void AlglibObjectiveFunctionAnalyticalDiff(
    const alglib::real_1d_array& estimated_data,
    double& residual_sum,  // NOLINT
    alglib::real_1d_array& gradient,  // NOLINT
    void* irls_map_solver_ptr) {

  IrlsMapSolver* irls_map_solver =
      reinterpret_cast<IrlsMapSolver*>(irls_map_solver_ptr);

  // Zero out the residual sum and the gradient vector.
  residual_sum = 0;
  const int num_pixels = irls_map_solver->GetNumPixels();
  for (int i = 0; i < num_pixels; ++i) {
    gradient[i] = 0;
  }

  // Compute data term residuals and gradient.
  const int num_images = irls_map_solver->GetNumImages();
  for (int image_index = 0; image_index < num_images; ++image_index) {
    std::pair<double, std::vector<double>> residual_sum_and_gradient =
        irls_map_solver->ComputeDataTermAnalyticalDiff(
            image_index, 0, estimated_data.getcontent());  // TODO: channel!?
    residual_sum += residual_sum_and_gradient.first;
    for (int i = 0; i < num_pixels; ++i) {
      gradient[i] += residual_sum_and_gradient.second[i];
    }
  }

  // Compute regularization residuals and gradient.
  std::pair<double, std::vector<double>> residual_sum_and_gradient =
      irls_map_solver->ComputeRegularizationAnalyticalDiff(
          estimated_data.getcontent());
  residual_sum += residual_sum_and_gradient.first;
  for (int i = 0; i < num_pixels; ++i) {
    gradient[i] += residual_sum_and_gradient.second[i];
  }

  // TODO: also add an optional numerical differentiation objective as before.
}

// The callback function for the ALGLIB solver. Called after every solver
// iteration, which updates the IRLS weights.
void AlglibSolverIterationCallback(
    const alglib::real_1d_array& estimated_data,
    double residual_sum,
    void* irls_map_solver) {

  // TODO: implement the reweighting computation here.
  LOG(INFO) << "Callback: residual sum = " << residual_sum;
}

ImageData IrlsMapSolver::Solve(const ImageData& initial_estimate) {
  // Initialize IRLS weights to 1.
  irls_weights_.resize(GetNumPixels());
  std::fill(irls_weights_.begin(), irls_weights_.end(), 1.0);

  const int num_channels = observations_[0].GetNumChannels();  // TODO: use!

  // Set up the optimization code with ALGLIB.
  // TODO: set these numbers correctly.
  const double epsg = 0.0000000001;
  const double epsf = 0.0;
  const double epsx = 0.0;
  const double numerical_diff_step_size = 1.0e-6;
  const alglib::ae_int_t max_num_iterations = 50;  // 0 = infinite.

  // TODO: multiple channel support?
  alglib::real_1d_array solver_data;
  solver_data.setcontent(
      GetNumPixels(), initial_estimate.GetMutableDataPointer(0));

  alglib::mincgstate solver_state;
  alglib::mincgreport solver_report;
  // TODO: enable numerical diff option?
  // if (solver_options_.use_numerical_differentiation) {
  // alglib::mincgcreatef(solver_data, numerical_diff_step_size, solver_state);
  // } else {
  alglib::mincgcreate(solver_data, solver_state);
  // }
  alglib::mincgsetcond(solver_state, epsg, epsf, epsx, max_num_iterations);
  alglib::mincgsetxrep(solver_state, true);

  // TODO: enable numerical diff option?
  // Solve and get results report.
  // if (solver_options_.use_numerical_differentiation) {
  //   alglib::mincgoptimize(
  //       solver_state,
  //       ObjectiveFunctionNumericalDifferentiation,
  //       SolverIterationCallback,
  //       const_cast<void*>(reinterpret_cast<const void*>(this)));
  // } else {
  alglib::mincgoptimize(
      solver_state,
      AlglibObjectiveFunctionAnalyticalDiff,
      AlglibSolverIterationCallback,
      const_cast<void*>(reinterpret_cast<const void*>(this)));
  // }
  alglib::mincgresults(solver_state, solver_data, solver_report);

  const ImageData estimated_image(solver_data.getcontent(), image_size_);
  return estimated_image;
}

std::pair<double, std::vector<double>>
IrlsMapSolver::ComputeDataTermAnalyticalDiff(
    const int image_index,
    const int channel_index,
    const double* estimated_image_data) const {

  CHECK_NOTNULL(estimated_image_data);

  // Degrade (and re-upsample) the HR estimate with the image model.
  ImageData degraded_hr_image(estimated_image_data, image_size_);
  image_model_.ApplyToImage(&degraded_hr_image, image_index);
  degraded_hr_image.ResizeImage(image_size_, cv::INTER_NEAREST);

  const int num_pixels = GetNumPixels();

  // Compute the individual residuals by comparing pixel values. Sum them up
  // for the final residual sum.
  double residual_sum = 0;
  std::vector<double> residuals;
  residuals.reserve(num_pixels);
  for (int i = 0; i < num_pixels; ++i) {
    const double residual =
        degraded_hr_image.GetPixelValue(0, i) -
        observations_.at(image_index).GetPixelValue(channel_index, i);
    residuals.push_back(residual);
    residual_sum += (residual * residual);
  }

  // Apply transpose operations to the residual image. This is used to compute
  // the gradient.
  ImageData residual_image(residuals.data(), image_size_);
  const int scale = image_model_.GetDownsamplingScale();
  residual_image.ResizeImage(cv::Size(
      image_size_.width / scale,
      image_size_.height / scale));
  image_model_.ApplyTransposeToImage(&residual_image, image_index);

  // Build the gradient vector.
  std::vector<double> gradient(num_pixels);
  std::fill(gradient.begin(), gradient.end(), 0);
  for (int pixel_index = 0; pixel_index < num_pixels; ++pixel_index) {
    // Only 1 channel (channel 0) in the residual image.
    const double partial_derivative =
        2 * residual_image.GetPixelValue(0, pixel_index);
    gradient[pixel_index] = partial_derivative;
  }

  return make_pair(residual_sum, gradient);
}

std::pair<double, std::vector<double>>
IrlsMapSolver::ComputeRegularizationAnalyticalDiff(
    const double* estimated_image_data) const {

  CHECK_NOTNULL(estimated_image_data);

  const int num_pixels = GetNumPixels();
  std::vector<double> gradient(num_pixels);
  std::fill(gradient.begin(), gradient.end(), 0);
  double residual_sum = 0;

  // Apply each regularizer individually.
  for (int i = 0; i < regularizers_.size(); ++i) {
    // Compute the residuals and squared residual sum.
    const double regularization_parameter = regularizers_[i].second;
    std::vector<double> residuals =
        regularizers_[i].first->ApplyToImage(estimated_image_data);

    for (int pixel_index = 0; pixel_index < num_pixels; ++pixel_index) {
      const double weight = sqrt(irls_weights_.at(pixel_index));
      const double residual = regularization_parameter * weight * residuals[i];
      residuals[i] = residual;
      // TODO: ^^^
      //   Should we square this? Keep it unchanged? Maybe this is the issue?
      residual_sum += (residual * residual);
    }

    // Compute the gradient vector.
    std::vector<double> partial_const_terms;
    for (int pixel_index = 0; pixel_index < num_pixels; ++pixel_index) {
      // Each derivative is multiplied by
      //   2 * lambda * w^2 * reg_i
      // where 2 comes from the squared norm (L2) term,
      // lambda is the regularization parameter,
      // w^2 is the squared weight (since the weights are square-rooted in the
      //   residual computation, the raw weight is used here),
      // and reg_i is the value of the regularization term at pixel i.
      // These constants are multiplied with the partial derivatives at each
      // pixel w.r.t. all other pixels, which are computed specifically based on
      // the derivative of the regularizer function.
      partial_const_terms.push_back(
          2 *
          regularization_parameter *
          irls_weights_[pixel_index] *
          residuals[pixel_index]);
    }
    const std::vector<double> partial_derivatives =
        regularizers_[i].first->GetDerivatives(
            estimated_image_data, partial_const_terms);
    for (int pixel_index = 0; pixel_index < num_pixels; ++pixel_index) {
      gradient[pixel_index] += partial_derivatives[pixel_index];
    }
  }

  return make_pair(residual_sum, gradient);
}

}  // namespace super_resolution