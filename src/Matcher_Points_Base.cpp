/* -------------------------------------------------------------------------
 *  A repertory of multi primitive-to-primitive (MP2) ICP algorithms in C++
 * Copyright (C) 2018-2020 Jose Luis Blanco, University of Almeria
 * See LICENSE for license information.
 * ------------------------------------------------------------------------- */
/**
 * @file   Matcher_Points_Base.cpp
 * @brief  Pointcloud matcher auxiliary class for iterating over point layers.
 * @author Jose Luis Blanco Claraco
 * @date   June 25, 2020
 */

#include <mp2p_icp/Matcher_Points_Base.h>
#include <chrono>
#include <numeric>  // iota
#include <random>

using namespace mp2p_icp;

void Matcher_Points_Base::match(
    const pointcloud_t& pcGlobal, const pointcloud_t& pcLocal,
    const mrpt::poses::CPose3D& localPose, Pairings& out) const
{
    MRPT_START

    out = Pairings();

    // Analyze point cloud layers, one by one:
    for (const auto& glLayerKV : pcGlobal.point_layers)
    {
        const auto& name = glLayerKV.first;

        if (!weight_pt2pt_layers.empty() &&
            weight_pt2pt_layers.count(name) == 0)
            // If we have weights and this layer is not listed, Skip it:
            continue;

        // Look for a matching layer in "local":
        auto itLocal = pcLocal.point_layers.find(name);
        if (itLocal == pcLocal.point_layers.end()) continue;

        const mrpt::maps::CPointsMap::Ptr& glLayer = glLayerKV.second;
        const mrpt::maps::CPointsMap::Ptr& lcLayer = itLocal->second;

        ASSERT_(glLayer);
        ASSERT_(lcLayer);

        const size_t nBefore = out.paired_points.size();

        implMatchOneLayer(*glLayer, *lcLayer, localPose, out);

        const size_t nAfter = out.paired_points.size();

        if (!weight_pt2pt_layers.empty() && nAfter != nBefore)
        {
            const double w = weight_pt2pt_layers.at(name);
            out.point_weights.emplace_back(nAfter - nBefore, w);
        }
    }
    MRPT_END
}

void Matcher_Points_Base::initializeLayerWeights(
    const mrpt::containers::Parameters& p)
{
    weight_pt2pt_layers.clear();
    ASSERT_(p.isMap());

    for (const auto& kv : p.asMap())
    {
        const std::string ly = kv.first;
        const double      w  = std::any_cast<double>(kv.second);

        weight_pt2pt_layers[ly] = w;
    }
}

Matcher_Points_Base::TransformedLocalPointCloud
    Matcher_Points_Base::transform_local_to_global(
        const mrpt::maps::CPointsMap& pcLocal,
        const mrpt::poses::CPose3D& localPose, const std::size_t maxLocalPoints,
        const uint64_t localPointsSampleSeed)
{
    MRPT_START
    TransformedLocalPointCloud r;

    const auto lambdaKeepBBox = [&](float x, float y, float z) {
        mrpt::keep_max(r.localMax.x, x);
        mrpt::keep_max(r.localMax.y, y);
        mrpt::keep_max(r.localMax.z, z);

        mrpt::keep_min(r.localMin.x, x);
        mrpt::keep_min(r.localMin.y, y);
        mrpt::keep_min(r.localMin.z, z);
    };

    const auto& lxs = pcLocal.getPointsBufferRef_x();
    const auto& lys = pcLocal.getPointsBufferRef_y();
    const auto& lzs = pcLocal.getPointsBufferRef_z();

    const size_t nLocalPoints = pcLocal.size();

    if (maxLocalPoints == 0 || nLocalPoints <= maxLocalPoints)
    {
        // All points:
        r.x_locals.resize(nLocalPoints);
        r.y_locals.resize(nLocalPoints);
        r.z_locals.resize(nLocalPoints);

        for (size_t i = 0; i < nLocalPoints; i++)
        {
            localPose.composePoint(
                lxs[i], lys[i], lzs[i], r.x_locals[i], r.y_locals[i],
                r.z_locals[i]);
            lambdaKeepBBox(r.x_locals[i], r.y_locals[i], r.z_locals[i]);
        }
    }
    else
    {
        // random subset:
        r.idxs.emplace(maxLocalPoints);
        std::iota(r.idxs->begin(), r.idxs->end(), 0);

        const unsigned int seed =
            localPointsSampleSeed != 0
                ? localPointsSampleSeed
                : std::chrono::system_clock::now().time_since_epoch().count();

        MRPT_TODO("Partial shuffle only?");
        std::shuffle(
            r.idxs->begin(), r.idxs->end(), std::default_random_engine(seed));

        r.x_locals.resize(maxLocalPoints);
        r.y_locals.resize(maxLocalPoints);
        r.z_locals.resize(maxLocalPoints);

        for (size_t ri = 0; ri < maxLocalPoints; ri++)
        {
            const auto i = (*r.idxs)[ri];
            localPose.composePoint(
                lxs[i], lys[i], lzs[i], r.x_locals[ri], r.y_locals[ri],
                r.z_locals[ri]);
            lambdaKeepBBox(r.x_locals[ri], r.y_locals[ri], r.z_locals[ri]);
        }
    }

    return r;
    MRPT_END
}
