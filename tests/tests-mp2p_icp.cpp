/* -------------------------------------------------------------------------
 *   A Modular Optimization framework for Localization and mApping  (MOLA)
 * Copyright (C) 2018-2019 Jose Luis Blanco, University of Almeria
 * See LICENSE for license information.
 * ------------------------------------------------------------------------- */

/**
 * @file   test-mp2p_icp-olae.cpp
 * @brief  Unit tests for the mp2p_icp OLAE solver
 * @author Jose Luis Blanco Claraco
 * @date   May 12, 2019
 */

#include <mp2p_icp/ICP_OLAE.h>
#include <mrpt/core/exceptions.h>
#include <mrpt/poses/CPose3D.h>
#include <mrpt/poses/CPose3DQuat.h>
#include <mrpt/poses/Lie/SE.h>
#include <mrpt/poses/Lie/SO.h>
#include <mrpt/random.h>
#include <mrpt/system/CTimeLogger.h>
#include <mrpt/system/filesystem.h>  // fileNameStripInvalidChars()
#include <mrpt/tfest/se3.h>  // classic Horn method
#include <cstdlib>
#include <sstream>

// Used to validate OLAE. However, it may make the Gauss-Newton solver, or the
// robust kernel with outliers to fail.
static bool TEST_LARGE_ROTATIONS = nullptr != ::getenv("TEST_LARGE_ROTATIONS");
static bool DO_SAVE_STAT_FILES   = nullptr != ::getenv("DO_SAVE_STAT_FILES");
static const size_t num_reps     = 2000;

using TPoints = std::vector<mrpt::math::TPoint3D>;
using TPlanes = std::vector<mp2p_icp::plane_patch_t>;

TPoints generate_points(const size_t nPts)
{
    auto& rnd = mrpt::random::getRandomGenerator();

    TPoints pA;
    pA.resize(nPts);

    for (size_t i = 0; i < nPts; i++)
    {
        pA[i].x = rnd.drawUniform(-50.0, 50.0);
        pA[i].y = rnd.drawUniform(-50.0, 50.0);
        pA[i].z = rnd.drawUniform(-50.0, 50.0);
    }
    return pA;
}

TPlanes generate_planes(const size_t nPlanes)
{
    auto& rnd = mrpt::random::getRandomGenerator();

    TPlanes plA;
    plA.resize(nPlanes);

    for (size_t i = 0; i < nPlanes; i++)
    {
        plA[i].centroid.x = rnd.drawUniform(-50.0, 50.0);
        plA[i].centroid.y = rnd.drawUniform(-50.0, 50.0);
        plA[i].centroid.z = rnd.drawUniform(-50.0, 50.0);

        auto n = mrpt::math::TVector3D(
            rnd.drawUniform(-1.0, 1.0), rnd.drawUniform(-1.0, 1.0),
            rnd.drawUniform(-1.0, 1.0));
        n *= 1.0 / n.norm();

        plA[i].plane = mrpt::math::TPlane(plA[i].centroid, n);
    }
    return plA;
}

// transform features
mrpt::poses::CPose3D transform_points_planes(
    const TPoints& pA, TPoints& pB, mrpt::tfest::TMatchingPairList& pointsPairs,
    const TPlanes& plA, TPlanes& plB,
    std::vector<mp2p_icp::matched_plane_t>& planePairs,
    mp2p_icp::TMatchedPointPlaneList& pt2plPairs, const double xyz_noise_std,
    const double n_err_std /* normals noise*/, const double outliers_ratio)
{
    auto& rnd = mrpt::random::getRandomGenerator();

    double Dx, Dy, Dz, yaw, pitch, roll;
    if (TEST_LARGE_ROTATIONS)
    {
        Dx = rnd.drawUniform(-10.0, 10.0);
        Dy = rnd.drawUniform(-10.0, 10.0);
        Dz = rnd.drawUniform(-10.0, 10.0);

        yaw   = mrpt::DEG2RAD(rnd.drawUniform(-180.0, 180.0));
        pitch = mrpt::DEG2RAD(rnd.drawUniform(-89.0, 89.0));
        roll  = mrpt::DEG2RAD(rnd.drawUniform(-89.0, 89.0));
    }
    else
    {
        Dx = rnd.drawUniform(-0.2, 0.2);
        Dy = rnd.drawUniform(-0.2, 0.2);
        Dz = rnd.drawUniform(-0.2, 0.2);

        yaw   = mrpt::DEG2RAD(rnd.drawUniform(-4.0, 4.0));
        pitch = mrpt::DEG2RAD(rnd.drawUniform(-4.0, 4.0));
        roll  = mrpt::DEG2RAD(rnd.drawUniform(-4.0, 4.0));
    }

    const auto pose = mrpt::poses::CPose3D(Dx, Dy, Dz, yaw, pitch, roll);
    // just the rotation, to transform vectors (vs. R^3 points):
    const auto pose_rot_only = mrpt::poses::CPose3D(0, 0, 0, yaw, pitch, roll);

    // Points:
    pB.resize(pA.size());
    for (std::size_t i = 0; i < pA.size(); ++i)
    {
        // outlier?
        const bool is_outlier = (rnd.drawUniform(0.0, 1.0) < outliers_ratio);
        auto       i_b        = i;
        if (is_outlier) rnd.drawUniformUnsignedIntRange(i_b, 0, pA.size() - 1);

        // Transform + noise:
        pose.inverseComposePoint(pA[i], pB[i_b]);

        pB[i].x += rnd.drawGaussian1D(0, xyz_noise_std);
        pB[i].y += rnd.drawGaussian1D(0, xyz_noise_std);
        pB[i].z += rnd.drawGaussian1D(0, xyz_noise_std);

        // Add pairing:
        mrpt::tfest::TMatchingPair pair;
        pair.this_idx = pair.other_idx = i;
        pair.this_x                    = pA[i][0];
        pair.this_y                    = pA[i][1];
        pair.this_z                    = pA[i][2];

        pair.other_x = pB[i][0];
        pair.other_y = pB[i][1];
        pair.other_z = pB[i][2];

        pointsPairs.push_back(pair);
    }

    // Planes: transform + noise
    plB.resize(plA.size());
    pt2plPairs.clear();
    pt2plPairs.reserve(plA.size());

    for (std::size_t i = 0; i < plA.size(); ++i)
    {
        const bool is_outlier = (rnd.drawUniform(0.0, 1.0) < outliers_ratio);
        auto       i_b        = i;
        if (is_outlier) rnd.drawUniformUnsignedIntRange(i_b, 0, plA.size() - 1);

        // Centroid: transform + noise
        plB[i].centroid = pose.inverseComposePoint(plA[i_b].centroid);

        const auto sigma_c = xyz_noise_std;
        const auto sigma_n = n_err_std;

        plB[i].centroid.x += rnd.drawGaussian1D(0, sigma_c);
        plB[i].centroid.y += rnd.drawGaussian1D(0, sigma_c);
        plB[i].centroid.z += rnd.drawGaussian1D(0, sigma_c);

        // Plane: rotate + noise
        plB[i].plane = plA[i_b].plane;
        {
            const mrpt::math::TVector3D ug = plA[i].plane.getNormalVector();
            mrpt::math::TVector3D       ul;
            pose_rot_only.inverseComposePoint(ug, ul);

            auto& coefs = plB[i].plane.coefs;

            // Ax+By+Cz+D=0
            coefs[0] = ul.x + rnd.drawGaussian1D(0, sigma_n);
            coefs[1] = ul.y + rnd.drawGaussian1D(0, sigma_n);
            coefs[2] = ul.z + rnd.drawGaussian1D(0, sigma_n);
            coefs[3] = 0;  // temporary.
            plB[i].plane.unitarize();

            coefs[3] =
                -(coefs[0] * plB[i].centroid.x + coefs[1] * plB[i].centroid.y +
                  coefs[2] * plB[i].centroid.z);
        }

        // Add plane-plane pairing:
        mp2p_icp::matched_plane_t pair;
        pair.p_this  = plA[i];
        pair.p_other = plB[i];
        planePairs.push_back(pair);

        // Add point-plane pairing:
        mp2p_icp::point_plane_pair_t pt2pl;
        pt2pl.pl_this  = plA[i];
        pt2pl.pt_other = mrpt::math::TPoint3Df(
            plB[i].centroid.x, plB[i].centroid.y, plB[i].centroid.z);

        pt2plPairs.push_back(pt2pl);
    }

    return pose;
}

bool TEST_mp2p_icp_olae(
    const size_t numPts, const size_t numLines, const size_t numPlanes,
    const double xyz_noise_std = .0, const double n_err_std = .0,
    bool use_robust = false, const double outliers_ratio = .0)
{
    using namespace mrpt::poses::Lie;

    MRPT_START

    const std::string tstName = mrpt::format(
        "TEST_mp2p_icp_olae_nPt=%06u_nLin=%06u_nPl=%06u_xyzStd=%.04f_nStd=%."
        "04f_outliers=%6.03f_robust=%i",
        static_cast<unsigned int>(numPts), static_cast<unsigned int>(numLines),
        static_cast<unsigned int>(numPlanes), xyz_noise_std, n_err_std,
        outliers_ratio, use_robust ? 1 : 0);

    std::cout << "[TEST] " << tstName << "\n";

    mrpt::system::CTimeLogger profiler;
    profiler.setMinLoggingLevel(mrpt::system::LVL_ERROR);  // to make it quiet

    // Repeat the test many times, with different random values:
    mp2p_icp::OLAE_Match_Result res;
    mp2p_icp::P2P_Match_Result  res2;
    mrpt::poses::CPose3D        gt_pose;

    const auto max_allowed_error =
        std::min(1.0, 0.1 + 10 * xyz_noise_std + 50 * n_err_std);

    // Collect stats: execution time, OLAE norm(error), Honr norm(error)
    mrpt::math::CMatrixDouble stats(num_reps, 3);

    double avr_err_olea = .0, avr_err_horn = .0;

    for (size_t rep = 0; rep < num_reps; rep++)
    {
        // The input points & planes
        const TPoints pA  = generate_points(numPts);
        const TPlanes plA = generate_planes(numPlanes);

        TPoints pB;
        TPlanes plB;

        mrpt::tfest::TMatchingPairList   pointPairs;
        mp2p_icp::TMatchedPlaneList      planePairs;
        mp2p_icp::TMatchedPointPlaneList pt2plPairs;

        gt_pose = transform_points_planes(
            pA, pB, pointPairs, plA, plB, planePairs, pt2plPairs, xyz_noise_std,
            n_err_std, outliers_ratio);

        // ========  TEST: olae_match ========
        {
            mp2p_icp::OLAE_Match_Input in;
            in.paired_points     = pointPairs;
            in.paired_planes     = planePairs;
            in.use_robust_kernel = use_robust;

            profiler.enter("olea_match");

            mp2p_icp::olae_match(in, res);

            const double dt_last = profiler.leave("olea_match");

            // Collect stats:

            // Measure errors in SE(3) if we have many points, in SO(3)
            // otherwise:
            const auto pos_error = gt_pose - res.optimal_pose;
            const auto err_log_n =
                SO<3>::log(pos_error.getRotationMatrix()).norm();
#if 0
            numPts < (numPlanes + numLines)
                    ? SO<3>::log(pos_error.getRotationMatrix()).norm()
                    : SE<3>::log(pos_error).norm();
#endif
            if (outliers_ratio < 1e-5 && err_log_n > max_allowed_error)
            {
                std::cout << " -Ground_truth : " << gt_pose.asString() << "\n"
                          << " -OLEA_output  : " << res.optimal_pose.asString()
                          << "\n -GT_rot:\n"
                          << gt_pose.getRotationMatrix() << "\n";
                ASSERT_BELOW_(err_log_n, max_allowed_error);
            }

            stats(rep, 0) = dt_last;
            stats(rep, 1) = err_log_n;
            avr_err_olea += err_log_n;
        }

        // ========  TEST: Classic Horn ========
        if (numPts > 0 && numLines == 0 && numPlanes == 0)
        {
            double                   out_scale;
            mrpt::poses::CPose3DQuat out_transform;

            profiler.enter("se3_l2");
            mrpt::tfest::se3_l2(
                pointPairs, out_transform, out_scale, true /* force scale=1 */);
            profiler.leave("se3_l2");

            const mrpt::poses::CPose3D optimal_pose(out_transform);

            const auto pos_error = gt_pose - optimal_pose;
            const auto err_log_n =
                SO<3>::log(pos_error.getRotationMatrix()).norm();

            // Don't make the tests fail if we have outliers, since it IS
            // expected that, sometimes, we don't get to the optimum
            if (outliers_ratio < 1e-5 && err_log_n > max_allowed_error)
            {
                std::cout << " -Ground_truth : " << gt_pose.asString() << "\n"
                          << " -Horn_output  : " << res2.optimal_pose.asString()
                          << "\n -GT_rot:\n"
                          << gt_pose.getRotationMatrix() << "\n";
                ASSERT_BELOW_(err_log_n, max_allowed_error);
            }

            stats(rep, 2) = err_log_n;
            avr_err_horn += err_log_n;
        }
    }  // for each repetition

    avr_err_olea /= num_reps;
    avr_err_horn /= num_reps;

    const double dt_olea = profiler.getMeanTime("olea_match");
    const double dt_p2p  = profiler.getMeanTime("se3_l2");

    std::cout << " -Ground_truth   : " << gt_pose.asString() << "\n"
              << " -OLEA_output    : " << res.optimal_pose.asString() << "\n"
              << " -OLAE avr. error: " << avr_err_olea
              << "  Time: " << dt_olea * 1e6 << " [us]\n"
              << " -Horn avr. error : " << avr_err_horn
              << "  Time: " << dt_p2p * 1e6 << " [us]\n";

    if (DO_SAVE_STAT_FILES)
        stats.saveToTextFile(
            mrpt::system::fileNameStripInvalidChars(tstName) +
            std::string(".txt"));

    return true;  // all ok.
    MRPT_END
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
    try
    {
        auto& rnd = mrpt::random::getRandomGenerator();
        rnd.randomize();

        const double nXYZ = 0.1;  // [meters] std. noise of XYZ points
        const double nN   = mrpt::DEG2RAD(0.5);  // normals noise

        // arguments: nPts, nLines, nPlanes
        // Points only. Noiseless:
        ASSERT_(TEST_mp2p_icp_olae(3 /*pt*/, 0 /*li*/, 0 /*pl*/));
        ASSERT_(TEST_mp2p_icp_olae(4 /*pt*/, 0 /*li*/, 0 /*pl*/));
        ASSERT_(TEST_mp2p_icp_olae(10 /*pt*/, 0 /*li*/, 0 /*pl*/));
        ASSERT_(TEST_mp2p_icp_olae(100 /*pt*/, 0 /*li*/, 0 /*pl*/));
        ASSERT_(TEST_mp2p_icp_olae(1000 /*pt*/, 0 /*li*/, 0 /*pl*/));

        // Points only. Noisy:
        ASSERT_(TEST_mp2p_icp_olae(100 /*pt*/, 0 /*li*/, 0 /*pl*/, nXYZ));
        ASSERT_(TEST_mp2p_icp_olae(1000 /*pt*/, 0 /*li*/, 0 /*pl*/, nXYZ));

        // Planes only. Noiseless:
        ASSERT_(TEST_mp2p_icp_olae(0 /*pt*/, 0 /*li*/, 3 /*pl*/));
        ASSERT_(TEST_mp2p_icp_olae(0 /*pt*/, 0 /*li*/, 10 /*pl*/));
        ASSERT_(TEST_mp2p_icp_olae(0 /*pt*/, 0 /*li*/, 100 /*pl*/));

        // Planes only. Noisy:
        ASSERT_(TEST_mp2p_icp_olae(0 /*pt*/, 0 /*li*/, 10 /*pl*/, 0, nN));
        ASSERT_(TEST_mp2p_icp_olae(0 /*pt*/, 0 /*li*/, 100 /*pl*/, 0, nN));
        // Points and planes, noisy.
        ASSERT_(TEST_mp2p_icp_olae(1 /*pt*/, 0 /*li*/, 3 /*pl*/));
        ASSERT_(TEST_mp2p_icp_olae(2 /*pt*/, 0 /*li*/, 1 /*pl*/));
        ASSERT_(TEST_mp2p_icp_olae(20 /*pt*/, 0 /*li*/, 10 /*pl*/, nXYZ, nN));
        ASSERT_(TEST_mp2p_icp_olae(400 /*pt*/, 0 /*li*/, 100 /*pl*/, nXYZ, nN));

        // Points only. Noisy w. outliers:
        for (int robust = 0; robust <= 1; robust++)
        {
            for (double Or = .05; Or < 0.96; Or += 0.05)
            {
                ASSERT_(TEST_mp2p_icp_olae(
                    100 /*pt*/, 0, 0, nXYZ, .0, robust != 0, Or));
                ASSERT_(TEST_mp2p_icp_olae(
                    1000 /*pt*/, 0, 0, nXYZ, .0, robust != 0, Or));
            }
        }
    }
    catch (std::exception& e)
    {
        std::cerr << mrpt::exception_to_str(e) << "\n";
        return 1;
    }
}
