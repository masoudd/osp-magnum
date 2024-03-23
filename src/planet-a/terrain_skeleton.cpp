/**
 * Open Space Program
 * Copyright © 2019-2024 Open Space Program Project
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "terrain_skeleton.h"
#include "icosahedron.h"

using osp::bitvector_resize;
using osp::Vector3l;
using osp::Vector3;

namespace planeta
{

void unsubdivide_level_by_distance(std::uint8_t const lvl, osp::Vector3l const pos, TerrainSkeleton const& rTrn, SubdivScratchpad& rSP)
{
    TerrainSkeleton::Level const& rLvl   = rTrn.levels[lvl];
    SubdivScratchpadLevel&        rLvlSP = rSP .levels[lvl];

    auto const maybe_distance_check = [&rTrn, &rSP, &rLvl, &rLvlSP] (SkTriId const sktriId)
    {
        if (rSP.distanceTestDone.test(sktriId.value))
        {
            return; // Already checked
        }

        SkTriGroupId const childrenId = rTrn.skel.tri_at(sktriId).children;
        if ( ! childrenId.has_value() )
        {
            return; // Must be subdivided to be considered for unsubdivision lol
        }

        SkTriGroup const& children = rTrn.skel.tri_group_at(childrenId);
        if (   children.triangles[0].children.has_value()
            || children.triangles[1].children.has_value()
            || children.triangles[2].children.has_value()
            || children.triangles[3].children.has_value() )
        {
            return; // For parents to unsubdivide, all children must be unsubdivided too
        }

        rLvlSP.distanceTestNext.push_back(sktriId);
        rSP.distanceTestDone.set(sktriId.value);
    };

    // Use a floodfill-style algorithm to avoid needing to check every triangle

    // Initial seed for floodfill are all subdivided triangles that neighbor a non-subdivided one
    for (std::size_t const sktriInt : rLvl.hasNonSubdivedNeighbor.ones())
    {
        maybe_distance_check(SkTriId(sktriInt));
    }

    while (rLvlSP.distanceTestNext.size() != 0)
    {
        std::swap(rLvlSP.distanceTestProcessing, rLvlSP.distanceTestNext);
        rLvlSP.distanceTestNext.clear();

        for (SkTriId const sktriId : rLvlSP.distanceTestProcessing)
        {
            Vector3l const center = rTrn.sktriCenter[sktriId];
            bool const tooFar = ! osp::is_distance_near(pos, center, rSP.distanceThresholdUnsubdiv[lvl]);

            LGRN_ASSERTM(rTrn.skel.tri_at(sktriId).children.has_value(),
                         "Non-subdivided triangles must not be added to distance test.");

            if (tooFar)
            {
                // All checks passed
                rSP.tryUnsubdiv.set(sktriId.value);

                // Floodfill by checking neighbors next
                SkeletonTriangle const& sktri = rTrn.skel.tri_at(sktriId);
                for (SkTriId const neighbor : sktri.neighbors)
                if (neighbor.has_value())
                {
                    maybe_distance_check(neighbor);
                }
            }
        }
    }
}

void unsubdivide_level_check_rules(std::uint8_t const lvl, TerrainSkeleton const& rTrn, SubdivScratchpad& rSP)
{
    TerrainSkeleton::Level const& rLvl   = rTrn.levels[lvl];
    SubdivScratchpadLevel&        rLvlSP = rSP .levels[lvl];

    auto const violates_rules = [&rTrn, &rLvl, &rSP] (SkTriId const sktriId, SkeletonTriangle const& sktri) noexcept -> bool
    {
        int subdivedNeighbors = 0;
        for (SkTriId const neighbor : sktri.neighbors)
        if (neighbor.has_value())
        {
            SkeletonTriangle const &rNeighbor = rTrn.skel.tri_at(neighbor);
            // Pretend neighbor is unsubdivided when it's in tryUnsubdiv, overrided
            // by cantUnsubdiv
            if (rNeighbor.children.has_value()
                && (  ! rSP.tryUnsubdiv .test(neighbor.value)
                     || rSP.cantUnsubdiv.test(neighbor.value) ) )
            {
                // Neighbor is subdivided
                ++subdivedNeighbors;

                // Check Rule B
                int        const  neighborEdge  = rNeighbor.find_neighbor_index(sktriId);
                SkTriGroup const& neighborGroup = rTrn.skel.tri_group_at(rNeighbor.children);

                switch (neighborEdge)
                {
                case 0:
                    if (neighborGroup.triangles[0].children.has_value()) return true;
                    if (neighborGroup.triangles[1].children.has_value()) return true;
                    break;
                case 1:
                    if (neighborGroup.triangles[1].children.has_value()) return true;
                    if (neighborGroup.triangles[2].children.has_value()) return true;
                    break;
                case 2:
                    if (neighborGroup.triangles[2].children.has_value()) return true;
                    if (neighborGroup.triangles[0].children.has_value()) return true;
                    break;
                }
            }
        }

        // Rule A
        if (subdivedNeighbors >= 2)
        {
            return true;
        }

        return false;
    };

    auto const check_recurse = [&violates_rules, &rTrn, &rLvl, &rSP] (auto const& self, SkTriId const sktriId) -> void
    {
        SkeletonTriangle const& sktri = rTrn.skel.tri_at(sktriId);

        if (violates_rules(sktriId, sktri))
        {
            rSP.cantUnsubdiv.set(sktriId.value);

            // Recurse into neighbors if they're also tryUnsubdiv
            for (SkTriId const neighbor : sktri.neighbors)
            if (rSP.tryUnsubdiv.test(neighbor.value) && ! rSP.cantUnsubdiv.test(neighbor.value))
            {
                self(self, neighbor);
            }
        }
    };

    for (std::size_t const sktriInt : rSP.tryUnsubdiv.ones())
    {
        if ( ! rSP.cantUnsubdiv.test(sktriInt) )
        {
            check_recurse(check_recurse, SkTriId::from_index(sktriInt));
        }
    }
}

void unsubdivide_level(std::uint8_t const lvl, TerrainSkeleton& rTrn, SubdivScratchpad& rSP)
{
    auto const wont_unsubdivide = [&rSP] (SkTriId const sktriId) -> bool
    {
        return ( ! rSP.tryUnsubdiv.test(sktriId.value) || rSP.cantUnsubdiv.test(sktriId.value) );
    };

    TerrainSkeleton::Level& rLvl   = rTrn.levels[lvl];
    SubdivScratchpadLevel&   rLvlSP = rSP .levels[lvl];

    for (std::size_t const sktriInt : rSP.tryUnsubdiv.ones())
    if ( ! rSP.cantUnsubdiv.test(sktriInt) )
    {
        // All checks passed, 100% confirmed sktri will be unsubdivided

        SkTriId const sktriId = SkTriId::from_index(sktriInt);
        SkeletonTriangle &rTri = rTrn.skel.tri_at(sktriId);

        LGRN_ASSERT(!rLvl.hasSubdivedNeighbor.test(sktriInt));
        for (SkTriId const neighborId : rTri.neighbors)
        if ( neighborId.has_value() && wont_unsubdivide(neighborId) )
        {
            SkeletonTriangle const& rNeighborTri = rTrn.skel.tri_at(neighborId);
            if ( rNeighborTri.children.has_value() )
            {
                rLvl.hasNonSubdivedNeighbor.set(neighborId.value);
                rLvl.hasSubdivedNeighbor.set(sktriInt);
            }
            else
            {
                bool neighborHasSubdivedNeighbor = false;
                for (SkTriId const neighborNeighborId : rNeighborTri.neighbors)
                if (   neighborNeighborId.has_value()
                    && neighborNeighborId != sktriId
                    && wont_unsubdivide(neighborNeighborId)
                    && rTrn.skel.is_tri_subdivided(neighborNeighborId) )
                {
                    neighborHasSubdivedNeighbor = true;
                    break;
                }

                if (neighborHasSubdivedNeighbor)
                {
                    rLvl.hasSubdivedNeighbor.set(neighborId.value);
                }
                else
                {
                    rLvl.hasSubdivedNeighbor.reset(neighborId.value);
                }
            }
        }

        LGRN_ASSERT( ! rLvl.hasSubdivedNeighbor.test(tri_id(rTri.children, 0).value) );
        LGRN_ASSERT( ! rLvl.hasSubdivedNeighbor.test(tri_id(rTri.children, 1).value) );
        LGRN_ASSERT( ! rLvl.hasSubdivedNeighbor.test(tri_id(rTri.children, 2).value) );
        LGRN_ASSERT( ! rLvl.hasSubdivedNeighbor.test(tri_id(rTri.children, 3).value) );

        rLvl.hasNonSubdivedNeighbor.reset(sktriInt);

        rSP.onUnsubdiv(sktriId, rTri, rTrn, rSP.onUnsubdivUserData);

        rTrn.skel.tri_unsubdiv(SkTriId(sktriInt), rTri);
    }

    rSP.tryUnsubdiv.reset();
    rSP.cantUnsubdiv.reset();
}

SkTriGroupId subdivide(
        SkTriId             sktriId,
        SkeletonTriangle&   rSkTri,
        std::uint8_t        lvl,
        bool                hasNextLevel,
        TerrainSkeleton&    rTrn,
        SubdivScratchpad&   rSP)
{
    LGRN_ASSERTM(rTrn.skel.tri_group_ids().exists(tri_group_id(sktriId)), "SkTri does not exist");
    LGRN_ASSERTM(!rSkTri.children.has_value(), "Already subdivided");

    TerrainSkeleton::Level& rLvl = rTrn.levels[lvl];

    std::array<SkTriId,  3> const neighbors = { rSkTri.neighbors[0], rSkTri.neighbors[1], rSkTri.neighbors[2] };
    std::array<SkVrtxId, 3> const corners   = { rSkTri .vertices[0], rSkTri .vertices[1], rSkTri .vertices[2] };

    // Create or get vertices between the 3 corners
    std::array<MaybeNewId<SkVrtxId>, 3> const middlesNew = rTrn.skel.vrtx_create_middles(corners);
    std::array<SkVrtxId, 3>             const middles    = {middlesNew[0].id, middlesNew[1].id, middlesNew[2].id};

    // Actually do the subdivision ( create a new group (4 triangles) as children )
    // manual borrow checker hint: rSkTri becomes invalid here >:)
    auto const [groupId, rGroup] = rTrn.skel.tri_subdiv(sktriId, rSkTri, middles);

    // kinda stupid to resize these here, but WHO CARES LOL XD
    auto const triCapacity  = rTrn.skel.tri_group_ids().capacity() * 4;
    bitvector_resize(rSP    .distanceTestDone,       triCapacity);
    bitvector_resize(rLvl   .hasSubdivedNeighbor,    triCapacity);
    bitvector_resize(rLvl   .hasNonSubdivedNeighbor, triCapacity);
    rTrn.sktriCenter.resize(triCapacity);
    auto const vrtxCapacity = rTrn.skel.vrtx_ids().capacity();
    rTrn.skPositions.resize(vrtxCapacity);
    rTrn.skNormals  .resize(vrtxCapacity);

    if (hasNextLevel)
    {
        rSP.levels[lvl+1].distanceTestNext.insert(rSP.levels[lvl+1].distanceTestNext.end(), {
            tri_id(groupId, 0),
            tri_id(groupId, 1),
            tri_id(groupId, 2),
            tri_id(groupId, 3),
        });
        rSP.distanceTestDone.set(tri_id(groupId, 0).value);
        rSP.distanceTestDone.set(tri_id(groupId, 1).value);
        rSP.distanceTestDone.set(tri_id(groupId, 2).value);
        rSP.distanceTestDone.set(tri_id(groupId, 3).value);
    }

    rSP.onSubdiv(sktriId, groupId, corners, middlesNew, rTrn, rSP.onSubdivUserData);

    // hasSubdivedNeighbor is only for Non-subdivided triangles
    rLvl.hasSubdivedNeighbor.reset(sktriId.value);

    bool hasNonSubdivNeighbor = false;

    // Check neighbours along all 3 edges
    for (int selfEdgeIdx = 0; selfEdgeIdx < 3; ++selfEdgeIdx)
    if ( SkTriId const neighborId = neighbors[selfEdgeIdx];
         neighborId.has_value() )
    {
        SkeletonTriangle& rNeighbor = rTrn.skel.tri_at(neighborId);
        if (rNeighbor.children.has_value())
        {
            // Assign bi-directional connection (neighbor's neighbor)
            int const neighborEdgeIdx = rNeighbor.find_neighbor_index(sktriId);

            SkTriGroup &rNeighborGroup = rTrn.skel.tri_group_at(rNeighbor.children);

            auto const [selfEdge, neighborEdge]
                = rTrn.skel.tri_group_set_neighboring(
                    {.id = groupId,            .rGroup = rGroup,         .edge = selfEdgeIdx},
                    {.id = rNeighbor.children, .rGroup = rNeighborGroup, .edge = neighborEdgeIdx});

            if (hasNextLevel)
            {
                TerrainSkeleton::Level &rNextLvl = rTrn.levels[lvl+1];
                if (rTrn.skel.tri_at(neighborEdge.childB).children.has_value())
                {
                    bitvector_resize(rNextLvl.hasSubdivedNeighbor, triCapacity);
                    rNextLvl.hasSubdivedNeighbor.set(selfEdge.childA.value);

                    bitvector_resize(rNextLvl.hasNonSubdivedNeighbor, triCapacity);
                    rNextLvl.hasNonSubdivedNeighbor.set(neighborEdge.childB.value);
                }

                if (rTrn.skel.tri_at(neighborEdge.childA).children.has_value())
                {
                    bitvector_resize(rNextLvl.hasSubdivedNeighbor, triCapacity);
                    rNextLvl.hasSubdivedNeighbor.set(selfEdge.childB.value);

                    bitvector_resize(rNextLvl.hasNonSubdivedNeighbor, triCapacity);
                    rNextLvl.hasNonSubdivedNeighbor.set(neighborEdge.childA.value);
                }
            }

            bool neighborHasNonSubdivedNeighbor = false;
            for (SkTriId const neighborNeighborId : rNeighbor.neighbors)
            if (   neighborNeighborId.has_value()
                && neighborNeighborId != sktriId
                && ! rTrn.skel.is_tri_subdivided(neighborNeighborId) )
            {
                neighborHasNonSubdivedNeighbor = true;
                break;
            }

            if (neighborHasNonSubdivedNeighbor)
            {
                rLvl.hasNonSubdivedNeighbor.set(neighborId.value);
            }
            else
            {
                rLvl.hasNonSubdivedNeighbor.reset(neighborId.value);
            }
        }
        else
        {
            // Neighbor is not subdivided
            hasNonSubdivNeighbor = true;
            rLvl.hasSubdivedNeighbor.set(neighborId.value);
        }
    }

    if (hasNonSubdivNeighbor)
    {
        rLvl.hasNonSubdivedNeighbor.set(sktriId.value);
    }
    else
    {
        rLvl.hasNonSubdivedNeighbor.reset(sktriId.value);
    }

    // Check for rule A and rule B violations
    // This can immediately subdivide other triangles recursively
    // Rule A: if neighbour has 2 subdivided neighbours, subdivide it too
    // Rule B: for corner children (childIndex != 3), parent's neighbours must be subdivided
    for (int selfEdgeIdx = 0; selfEdgeIdx < 3; ++selfEdgeIdx)
    {
        SkTriId const neighborId = rTrn.skel.tri_at(sktriId).neighbors[selfEdgeIdx];
        if (neighborId.has_value())
        {
            SkeletonTriangle& rNeighbor = rTrn.skel.tri_at(neighborId);
            if ( rNeighbor.children.has_value() )
            {
                continue; // Neighbor already subdivided. nothing to do
            }

            // Check Rule A by seeing if any other neighbor's neighbors are subdivided
            auto const is_other_subdivided = [&rSkel = rTrn.skel, &rNeighbor, sktriId] (SkTriId const other)
            {
                return    other != sktriId
                       && other.has_value()
                       && rSkel.is_tri_subdivided(other);
            };

            if (   is_other_subdivided(rNeighbor.neighbors[0])
                || is_other_subdivided(rNeighbor.neighbors[1])
                || is_other_subdivided(rNeighbor.neighbors[2]) )
            {
                // Rule A violation, more than 2 neighbors subdivided
                subdivide(neighborId, rTrn.skel.tri_at(neighborId), lvl, hasNextLevel, rTrn, rSP);
                bitvector_resize(rSP.distanceTestDone, rTrn.skel.tri_group_ids().capacity() * 4);
                rSP.distanceTestDone.set(neighborId.value);
            }
            else if (!rSP.distanceTestDone.test(neighborId.value))
            {
                // No Rule A violation, but floodfill distance-test instead
                rSP.levels[lvl].distanceTestNext.push_back(neighborId);
                rSP.distanceTestDone.set(neighborId.value);
            }

        }
        else // Neighbour doesn't exist, its parent is not subdivided. Rule B violation
        {
            LGRN_ASSERTM(tri_sibling_index(sktriId) != 3,
                         "Center triangles are always surrounded by their siblings");
            LGRN_ASSERTM(lvl != 0, "No level above level 0");

            SkTriId const parent = rTrn.skel.tri_group_at(tri_group_id(sktriId)).parent;

            LGRN_ASSERTM(parent.has_value(), "bruh");

            auto const& parentNeighbors = rTrn.skel.tri_at(parent).neighbors;

            LGRN_ASSERTM(parentNeighbors[selfEdgeIdx].has_value(),
                         "something screwed up XD");

            SkTriId const neighborParent = parentNeighbors[selfEdgeIdx].value();

            // Adds to ctx.rTerrain.levels[level-1].distanceTestNext
            subdivide(neighborParent, rTrn.skel.tri_at(neighborParent), lvl-1, true, rTrn, rSP);
            rSP.distanceTestDone.set(neighborParent.value);

            rSP.levelNeedProcess = std::min<uint8_t>(rSP.levelNeedProcess, lvl-1);
        }
    }

    return groupId;
}


void subdivide_level_by_distance(Vector3l const pos, std::uint8_t const lvl, TerrainSkeleton& rTrn, SubdivScratchpad& rSP)
{
    LGRN_ASSERT(lvl == rSP.levelNeedProcess);

    TerrainSkeleton::Level &rLvl   = rTrn.levels[lvl];
    SubdivScratchpadLevel  &rLvlSP = rSP .levels[lvl];

    bool const hasNextLevel = (lvl+1 < rSP.levelMax);

    while ( ! rSP.levels[lvl].distanceTestNext.empty() )
    {
        std::swap(rLvlSP.distanceTestProcessing, rLvlSP.distanceTestNext);
        rLvlSP.distanceTestNext.clear();

        bitvector_resize(rSP.distanceTestDone, rTrn.skel.tri_group_ids().capacity() * 4);

        for (SkTriId const sktriId : rLvlSP.distanceTestProcessing)
        {
            Vector3l const center = rTrn.sktriCenter[sktriId];

            LGRN_ASSERT(rSP.distanceTestDone.test(sktriId.value));
            bool const distanceNear = osp::is_distance_near(pos, center, rSP.distanceThresholdSubdiv[lvl]);
            ++rSP.distanceCheckCount;

            if (distanceNear)
            {
                SkeletonTriangle &rTri = rTrn.skel.tri_at(sktriId);
                if (rTri.children.has_value())
                {
                    if (hasNextLevel)
                    {
                        SkTriGroupId const children = rTri.children;
                        rSP.levels[lvl+1].distanceTestNext.insert(rSP.levels[lvl+1].distanceTestNext.end(), {
                            tri_id(children, 0),
                            tri_id(children, 1),
                            tri_id(children, 2),
                            tri_id(children, 3),
                        });
                        rSP.distanceTestDone.set(tri_id(children, 0).value);
                        rSP.distanceTestDone.set(tri_id(children, 1).value);
                        rSP.distanceTestDone.set(tri_id(children, 2).value);
                        rSP.distanceTestDone.set(tri_id(children, 3).value);
                    }
                }
                else
                {
                    subdivide(sktriId, rTri, lvl, hasNextLevel, rTrn, rSP);
                }
            }

            // Fix up Rule B violations
            while (rSP.levelNeedProcess != lvl)
            {
                subdivide_level_by_distance(pos, rSP.levelNeedProcess, rTrn, rSP);
            }
        }
    }

    LGRN_ASSERT(lvl == rSP.levelNeedProcess);
    ++rSP.levelNeedProcess;
}

void calc_sphere_tri_center(SkTriGroupId const groupId, TerrainSkeleton& rTrn, float const maxRadius, float const height)
{
    using osp::math::int_2pow;

    SkTriGroup const &group = rTrn.skel.tri_group_at(groupId);

    for (int i = 0; i < 4; ++i)
    {
        SkTriId          const  sktriId = tri_id(groupId, i);
        SkeletonTriangle const& tri     = group.triangles[i];

        SkVrtxId const va = tri.vertices[0].value();
        SkVrtxId const vb = tri.vertices[1].value();
        SkVrtxId const vc = tri.vertices[2].value();

        // average without overflow
        Vector3l const posAvg = rTrn.skPositions[va] / 3
                              + rTrn.skPositions[vb] / 3
                              + rTrn.skPositions[vc] / 3;

        Vector3 const nrmSum = rTrn.skNormals[va]
                             + rTrn.skNormals[vb]
                             + rTrn.skNormals[vc];

        LGRN_ASSERT(group.depth < gc_icoTowerOverHorizonVsLevel.size());
        float const terrainMaxHeight = height + maxRadius * gc_icoTowerOverHorizonVsLevel[group.depth];

        // 0.5 * terrainMaxHeight           : halve for middle
        // int_2pow<int>(rTerrain.scale)    : Vector3l conversion factor
        // / 3.0f                           : average from sum of 3 values
        Vector3l const riseToMid = Vector3l(nrmSum * (0.5f * terrainMaxHeight * osp::math::int_2pow<int>(rTrn.scale) / 3.0f));

        rTrn.sktriCenter[sktriId] = posAvg + riseToMid;
    }
}

void debug_check_rules(TerrainSkeleton &rTrn)
{
    // iterate all existing triangles
    for (std::size_t sktriInt = 0; sktriInt < rTrn.skel.tri_group_ids().capacity() * 4; ++sktriInt)
    if (SkTriId const sktriId = SkTriId(sktriInt);
        rTrn.skel.tri_group_ids().exists(tri_group_id(sktriId)))
    {
        SkeletonTriangle const& sktri = rTrn.skel.tri_at(sktriId);
        SkTriGroup       const& group = rTrn.skel.tri_group_at(tri_group_id(sktriId));

        int subdivedNeighbors = 0;
        int nonSubdivedNeighbors = 0;
        for (int edge = 0; edge < 3; ++edge)
        {
            SkTriId const neighbor = sktri.neighbors[edge];
            if (neighbor.has_value())
            {
                if (rTrn.skel.is_tri_subdivided(neighbor))
                {
                    ++subdivedNeighbors;
                }
                else
                {
                    ++nonSubdivedNeighbors;
                }
            }
            else
            {
                // Neighbor doesn't exist. parent MUST have neighbor
                SkTriId const parent = rTrn.skel.tri_group_at(tri_group_id(sktriId)).parent;
                LGRN_ASSERTM(parent.has_value(), "bruh");
                auto const& parentNeighbors = rTrn.skel.tri_at(parent).neighbors;
                LGRN_ASSERTM(parentNeighbors[edge].has_value(), "Rule B Violation");

                LGRN_ASSERTM(rTrn.skel.is_tri_subdivided(parentNeighbors[edge]) == false,
                             "Incorrectly set neighbors");
            }
        }

        if ( ! sktri.children.has_value() )
        {
            LGRN_ASSERTM(subdivedNeighbors < 2, "Rule A Violation");
        }

        // Verify hasSubdivedNeighbor and hasNonSubdivedNeighbor bitvectors
        if (group.depth < rTrn.levels.size())
        {
            TerrainSkeleton::Level& rLvl = rTrn.levels[group.depth];

            // lazy!
            auto const triCapacity  = rTrn.skel.tri_group_ids().capacity() * 4;
            bitvector_resize(rLvl.hasSubdivedNeighbor,    triCapacity);
            bitvector_resize(rLvl.hasNonSubdivedNeighbor, triCapacity);

            if (sktri.children.has_value())
            {
                LGRN_ASSERTMV(rLvl.hasNonSubdivedNeighbor.test(sktriInt) == (nonSubdivedNeighbors != 0),
                              "Incorrectly set hasNonSubdivedNeighbor",
                              sktriInt,
                              int(group.depth),
                              rLvl.hasNonSubdivedNeighbor.test(sktriInt),
                              nonSubdivedNeighbors);
                LGRN_ASSERTM(rLvl.hasSubdivedNeighbor.test(sktriInt) == false,
                            "hasSubdivedNeighbor is only for non-subdivided tris");
            }
            else
            {
                LGRN_ASSERTMV(rLvl.hasSubdivedNeighbor.test(sktriInt) == (subdivedNeighbors != 0),
                              "Incorrectly set hasSubdivedNeighbor",
                              sktriInt,
                              int(group.depth),
                              rLvl.hasSubdivedNeighbor.test(sktriInt),
                              subdivedNeighbors);
                LGRN_ASSERTM(rLvl.hasNonSubdivedNeighbor.test(sktriInt) == false,
                            "hasNonSubdivedNeighbor is only for subdivided tris");
            }
        }
    }
}



} // namespace planeta
