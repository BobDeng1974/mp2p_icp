/* -------------------------------------------------------------------------
 *  A repertory of multi primitive-to-primitive (MP2) ICP algorithms in C++
 * Copyright (C) 2018-2019 Jose Luis Blanco, University of Almeria
 * See LICENSE for license information.
 * ------------------------------------------------------------------------- */
/**
 * @file   ICP_GaussNewton.h
 * @brief  ICP registration for points and planes
 * @author Jose Luis Blanco Claraco
 * @date   May 11, 2019
 */
#pragma once

#include <mp2p_icp/ICP_Base.h>
#include <mp2p_icp/pointcloud.h>

namespace mp2p_icp
{
/** ICP registration for points, planes, and lines, using an iterative
 * Gauss-Newton numerical solver.
 *
 * \ingroup mp2p_icp_grp
 */
class ICP_GaussNewton : public ICP_Base
{
    DEFINE_MRPT_OBJECT(ICP_GaussNewton)

   protected:
    // See base class docs
    void impl_ICP_iteration(
        ICP_State& s, const Parameters& p, ICP_iteration_result& out) override;
};

}  // namespace mp2p_icp
