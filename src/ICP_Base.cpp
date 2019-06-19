/* -------------------------------------------------------------------------
 *  A repertory of multi primitive-to-primitive (MP2) ICP algorithms in C++
 * Copyright (C) 2018-2019 Jose Luis Blanco, University of Almeria
 * See LICENSE for license information.
 * ------------------------------------------------------------------------- */
/**
 * @file   ICP_Base.cpp
 * @brief  Virtual interface for ICP algorithms. Useful for RTTI class searches.
 * @author Jose Luis Blanco Claraco
 * @date   Jun 10, 2019
 */

#include <mp2p_icp/ICP_Base.h>
#include <mrpt/core/exceptions.h>
#include <mrpt/poses/Lie/SE.h>
#include <mrpt/tfest/se3.h>

IMPLEMENTS_VIRTUAL_MRPT_OBJECT_NS_PREFIX(
    ICP_Base, mrpt::rtti::CObject, mp2p_icp);

using namespace mp2p_icp;

void ICP_Base::align(
    const pointcloud_t& pcs1, const pointcloud_t& pcs2,
    const mrpt::math::TPose3D& init_guess_m2_wrt_m1, const Parameters& p,
    Results& result)
{
    using namespace std::string_literals;

    MRPT_START
    // ICP uses KD-trees.
    // kd-trees have each own mutexes to ensure well-defined behavior in
    // multi-threading apps.

    ASSERT_EQUAL_(pcs1.point_layers.size(), pcs2.point_layers.size());
    ASSERT_(
        !pcs1.point_layers.empty() ||
        (!pcs1.planes.empty() && !pcs2.planes.empty()));

    // Reset output:
    result = Results();

    // Count of points:
    size_t pointcount1 = 0, pointcount2 = 0;
    for (const auto& kv1 : pcs1.point_layers)
    {
        // Ignore this layer?
        if (p.weight_pt2pt_layers.count(kv1.first) == 0) continue;

        pointcount1 += kv1.second->size();
        pointcount2 += pcs2.point_layers.at(kv1.first)->size();
    }
    ASSERT_(pointcount1 > 0 || !pcs1.planes.empty());
    ASSERT_(pointcount2 > 0 || !pcs2.planes.empty());

    // ------------------------------------------------------
    // Main ICP loop
    // ------------------------------------------------------
    ICP_State state(pcs1, pcs2);

    state.current_solution = mrpt::poses::CPose3D(init_guess_m2_wrt_m1);
    auto prev_solution     = state.current_solution;

    // Prepare params for "find pairings" for each layer:
    prepareMatchingParams(state, p);

    for (result.nIterations = 0; result.nIterations < p.maxIterations;
         result.nIterations++)
    {
        ICP_iteration_result iter_result;

        // Call to algorithm-specific implementation of one ICP iteration:
        impl_ICP_iteration(state, p, iter_result);

        if (!iter_result.success)
        {
            // Nothing we can do !!
            result.terminationReason = IterTermReason::NoPairings;
            result.goodness          = 0;
            break;
        }

        // Update to new solution:
        state.current_solution = iter_result.new_solution;

        // If matching has not changed, we are done:
        const auto deltaSol = state.current_solution - prev_solution;
        const mrpt::math::CVectorFixed<double, 6> dSol =
            mrpt::poses::Lie::SE<3>::log(deltaSol);
        const double delta_xyz = dSol.blockCopy<3, 1>(0, 0).norm();
        const double delta_rot = dSol.blockCopy<3, 1>(3, 0).norm();

#if 0
		std::cout << "Dxyz: " << std::abs(delta_xyz)
			<< " Drot:" << std::abs(delta_rot)
			<< " p: " << solution.asString() << "\n";
#endif

        if (std::abs(delta_xyz) < p.minAbsStep_trans &&
            std::abs(delta_rot) < p.minAbsStep_rot)
        {
            result.terminationReason = IterTermReason::Stalled;
            break;
        }

        prev_solution = state.current_solution;
    }

    if (result.nIterations >= p.maxIterations)
        result.terminationReason = IterTermReason::MaxIterations;

    // Ratio of points with a valid pairing:
    if (!state.layerOfLargestPc.empty())
        result.goodness =
            state.mres.at(state.layerOfLargestPc).correspondencesRatio;

    // Store output:
    result.optimal_tf.mean = state.current_solution;
    result.optimal_scale   = state.current_scale;
    MRPT_TODO("covariance of the estimation");
    // See: http://censi.mit.edu/pub/research/2007-icra-icpcov-slides.pdf

    MRPT_END
}

void ICP_Base::prepareMatchingParams(
    ICP_State& state, const Parameters& p) const
{
    MRPT_START

    // Prepare params for "find pairings" for each layer & find largest point
    // cloud:
    std::size_t pointCountLargestPc = 0;

    for (const auto& kv1 : state.pc1.point_layers)
    {
        const bool is_layer_of_planes =
            (kv1.first == pointcloud_t::PT_LAYER_PLANE_CENTROIDS);

        mrpt::maps::TMatchingParams& mp = state.mps[kv1.first];

        if (!is_layer_of_planes)
        {
            if (p.weight_pt2pt_layers.count(kv1.first) == 0) continue;

            const auto& m1 = kv1.second;
            ASSERT_(m1);

            if (m1->size() > pointCountLargestPc)
            {
                pointCountLargestPc    = m1->size();
                state.layerOfLargestPc = kv1.first;
            }

            // Matching params for point-to-point:
            // Distance threshold
            mp.maxDistForCorrespondence = p.thresholdDist;

            // Angular threshold
            mp.maxAngularDistForCorrespondence = p.thresholdAng;
            mp.onlyKeepTheClosest              = true;
            mp.onlyUniqueRobust                = false;
            mp.decimation_other_map_points     = std::max(
                1U,
                static_cast<unsigned>(m1->size() / (1.0 * p.maxPairsPerLayer)));

            // For decimation: cycle through all possible points, even if we
            // decimate them, in such a way that different points are used
            // in each iteration.
            mp.offset_other_map_points = 0;
        }
        else
        {
            // Matching params for plane-to-plane (their centroids only at
            // this point):
            // Distance threshold: + extra since  plane centroids must not
            // show up at the same location
            mp.maxDistForCorrespondence = p.thresholdDist + 2.0;
            // Angular threshold
            mp.maxAngularDistForCorrespondence = 0.;
            mp.onlyKeepTheClosest              = true;
            mp.decimation_other_map_points     = 1;
        }
    }

    MRPT_END
}
