// Copyright 2023 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the cascade library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>

#include <boost/numeric/conversion/cast.hpp>

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_reduce.h>
#include <oneapi/tbb/parallel_scan.h>

#include <cascade/detail/logging_impl.hpp>
#include <cascade/detail/sim_data.hpp>
#include <cascade/sim.hpp>

#if defined(__clang__) || defined(__GNUC__)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

#endif

#include <heyoka/mdspan.hpp>

#if defined(__clang__) || defined(__GNUC__)

#pragma GCC diagnostic pop

#endif

namespace cascade
{

namespace detail
{

namespace
{

#if !defined(NDEBUG) && (defined(__GNUC__) || defined(__clang__))

// Debug helper to compute the index of the first different
// bit between n1 and n2, starting from the MSB.
template <typename T>
int first_diff_bit(T n1, T n2)
{
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);

    const auto res_xor = n1 ^ n2;

    if (res_xor == 0u) {
        return std::numeric_limits<T>::digits;
    } else {
        if constexpr (std::is_same_v<T, unsigned>) {
            return __builtin_clz(res_xor);
        } else if constexpr (std::is_same_v<T, unsigned long>) {
            return __builtin_clzl(res_xor);
        } else if constexpr (std::is_same_v<T, unsigned long long>) {
            return __builtin_clzll(res_xor);
        } else {
            assert(false);
            throw;
        }
    }
}

#endif

} // namespace

} // namespace detail

// Construct the BVH tree for each chunk.
void sim::construct_bvh_trees_parallel()
{
    namespace stdex = std::experimental;

    spdlog::stopwatch sw;

    auto *logger = detail::get_logger();

    // Fetch the number of particles and chunks from m_data.
    const auto nparts = get_nparts();
    const auto nchunks = m_data->nchunks;

    // Initial values for the nodes' bounding boxes.
    constexpr auto finf = std::numeric_limits<float>::infinity();
    constexpr std::array<float, 4> default_lb = {finf, finf, finf, finf};
    constexpr std::array<float, 4> default_ub = {-finf, -finf, -finf, -finf};

    // Overflow check: we need to be able to represent all pointer differences
    // in the Morton codes vector.
    constexpr auto overflow_err_msg = "Overflow detected during the construction of a BVH tree";

    // LCOV_EXCL_START
    if (m_data->srt_mcodes.size()
        > static_cast<std::make_unsigned_t<std::ptrdiff_t>>(std::numeric_limits<std::ptrdiff_t>::max())) {
        throw std::overflow_error(overflow_err_msg);
    }
    // LCOV_EXCL_STOP

    // Views for accessing the sorted lb/ub data.
    using b_size_t = decltype(m_data->lbs.size());
    stdex::mdspan srt_lbs(std::as_const(m_data->srt_lbs).data(),
                          stdex::extents<b_size_t, stdex::dynamic_extent, stdex::dynamic_extent, 4u>(nchunks, nparts));
    stdex::mdspan srt_ubs(std::as_const(m_data->srt_ubs).data(),
                          stdex::extents<b_size_t, stdex::dynamic_extent, stdex::dynamic_extent, 4u>(nchunks, nparts));

    // Views for accessing the sorted Morton code data.
    using m_size_t = decltype(m_data->mcodes.size());
    stdex::mdspan srt_mcodes(std::as_const(m_data->srt_mcodes).data(),
                             stdex::extents<m_size_t, stdex::dynamic_extent, stdex::dynamic_extent>(nchunks, nparts));

    oneapi::tbb::parallel_for(oneapi::tbb::blocked_range(0u, nchunks), [&](const auto &range) {
        for (auto chunk_idx = range.begin(); chunk_idx != range.end(); ++chunk_idx) {
            // Fetch a reference to the tree and clear it out.
            auto &tree = m_data->bvh_trees[chunk_idx];
            tree.clear();

            // Fetch references to the temp buffers and
            // clear them out.
            auto &nc_buf = m_data->nc_buffer[chunk_idx];
            auto &ps_buf = m_data->ps_buffer[chunk_idx];
            auto &nplc_buf = m_data->nplc_buffer[chunk_idx];
            nc_buf.clear();
            ps_buf.clear();
            nplc_buf.clear();

            // Insert the root node.
            // NOTE: nn_level is inited to zero, even if we already know it is 1.
#if defined(__clang__)
            tree.push_back(sim_data::bvh_node{0, boost::numeric_cast<std::uint32_t>(nparts), -1, -1, -1, default_lb,
                                              default_ub, 0, 0});
#else
            tree.emplace_back(0, boost::numeric_cast<std::uint32_t>(nparts), -1, -1, -1, default_lb, default_ub, 0, 0);
#endif

            // The number of nodes at the current tree level.
            std::uint32_t cur_n_nodes = 1;

            // The total number of levels and nodes.
            std::uint32_t n_levels = 0, n_nodes = 0;

            while (cur_n_nodes != 0u) {
                // Fetch the current tree size.
                const auto cur_tree_size = tree.size();

                // The node index range for the iteration at the
                // current level.
                const auto n_begin = cur_tree_size - cur_n_nodes;
                const auto n_end = cur_tree_size;

                // Number of nodes at the next level, inited
                // with the maximum possible value.
                // LCOV_EXCL_START
                if (cur_n_nodes > std::numeric_limits<std::uint32_t>::max() / 2u) {
                    throw std::overflow_error(overflow_err_msg);
                }
                // LCOV_EXCL_STOP
                auto nn_next_level = cur_n_nodes * 2u;

                // Prepare the temp buffers.
                nc_buf.resize(boost::numeric_cast<decltype(nc_buf.size())>(cur_n_nodes));
                ps_buf.resize(boost::numeric_cast<decltype(ps_buf.size())>(cur_n_nodes));
                nplc_buf.resize(boost::numeric_cast<decltype(nplc_buf.size())>(cur_n_nodes));

                // Step 1: determine, for each node in the range,
                // if the node is a leaf or not, and, for an internal node,
                // the number of particles in the left child.
                const auto n_leaf_nodes = oneapi::tbb::parallel_reduce(
                    oneapi::tbb::blocked_range(n_begin, n_end), static_cast<std::uint32_t>(0),
                    [&](const auto &rn, std::uint32_t init) {
                        // Local accumulator for the number of leaf nodes
                        // detected in the range.
                        std::uint32_t loc_n_leaf_nodes = 0;

                        // NOTE: this for loop can *probably* be written in a vectorised
                        // fashion, using the gather primitives as done in heyoka.
                        for (auto node_idx = rn.begin(); node_idx != rn.end(); ++node_idx) {
                            assert(node_idx - n_begin < cur_n_nodes);

                            auto &cur_node = tree[node_idx];

                            // Flag to signal that this is a leaf node.
                            bool is_leaf_node = false;

                            const std::uint64_t *split_ptr = nullptr;

                            const auto mcodes_begin = &srt_mcodes(chunk_idx, 0) + cur_node.begin;
                            const auto mcodes_end = &srt_mcodes(chunk_idx, 0) + cur_node.end;

                            if (cur_node.end - cur_node.begin > 1u && cur_node.split_idx <= 63) {
                                // The node contains more than 1 particle,
                                // and the initial value for split_idx is within
                                // the bit width of a 64-bit integer.
                                // Figure out where the bit at index cur_node.split_idx
                                // (counted from MSB) flips from 0 to 1
                                // for the Morton codes in the range.
                                assert(cur_node.split_idx <= 63);
                                split_ptr = std::lower_bound(
                                    mcodes_begin, mcodes_end, 1u,
                                    [mask = static_cast<std::uint64_t>(1) << (63 - cur_node.split_idx)](
                                        std::uint64_t mcode, unsigned val) { return (mcode & mask) < val; });

                                while (split_ptr == mcodes_begin || split_ptr == mcodes_end) {
                                    // There is no bit flip at the current index.
                                    // We will try the next bit index.

                                    if (cur_node.split_idx == 63) {
                                        // No more bit indices are available.
                                        // This will be a leaf node containing more than 1 particle.
                                        is_leaf_node = true;

                                        break;
                                    }

                                    // Bump up the bit index and look
                                    // again for the bit flip.
                                    ++cur_node.split_idx;
                                    assert(cur_node.split_idx <= 63);
                                    split_ptr = std::lower_bound(
                                        mcodes_begin, mcodes_end, 1u,
                                        [mask = static_cast<std::uint64_t>(1) << (63 - cur_node.split_idx)](
                                            std::uint64_t mcode, unsigned val) { return (mcode & mask) < val; });
                                }
                            } else {
                                // Node with either:
                                // - a single particle, or
                                // - a value of split_idx which is > 63.
                                // The latter means that the node resulted
                                // from splitting a node whose particles'
                                // Morton codes differred at the least significant
                                // bit. This also implies that all the particles
                                // in the node have the same Morton code (this is checked
                                // in the BVH verification function).
                                // In either case, we cannot split any further
                                // and the node is a leaf.
                                is_leaf_node = true;
                            }

                            if (is_leaf_node) {
                                // A leaf node has no children.
                                nc_buf[node_idx - n_begin] = 0;
                                nplc_buf[node_idx - n_begin] = 0;

                                // Update the leaf nodes counter.
                                ++loc_n_leaf_nodes;

                                // NOTE: check that the initial value of the AABB
                                // was properly set.
                                assert(cur_node.lb == default_lb);
                                assert(cur_node.ub == default_ub);

                                // Compute the AABB for this leaf node.
                                for (auto pidx = cur_node.begin; pidx != cur_node.end; ++pidx) {
                                    // NOTE: min/max is fine here, we already checked
                                    // that all AABBs are finite.
                                    for (auto i = 0u; i < 4u; ++i) {
                                        cur_node.lb[i] = std::min(cur_node.lb[i], srt_lbs(chunk_idx, pidx, i));
                                        cur_node.ub[i] = std::max(cur_node.ub[i], srt_ubs(chunk_idx, pidx, i));
                                    }
                                }
                            } else {
                                assert(split_ptr != nullptr);

                                // An internal node has 2 children.
                                nc_buf[node_idx - n_begin] = 2;
                                // NOTE: if we are here, it means that is_leaf_node is false,
                                // which implies that split_ptr was written to at least once.
                                nplc_buf[node_idx - n_begin]
                                    = boost::numeric_cast<std::uint32_t>(split_ptr - mcodes_begin);
                            }
                        }

                        return init + loc_n_leaf_nodes;
                    },
                    std::plus<>{});

                // Decrease nn_next_level by n_leaf_nodes * 2.
                assert(n_leaf_nodes * 2u <= nn_next_level);
                nn_next_level -= n_leaf_nodes * 2u;

                // Step 2: prepare the tree for the new children nodes. This will add
                // new nodes at the end of the tree containing indeterminate
                // values. The properties of these new nodes will be set up
                // in step 4.
                // LCOV_EXCL_START
                if (nn_next_level > std::numeric_limits<decltype(cur_tree_size)>::max() - cur_tree_size) {
                    throw std::overflow_error(overflow_err_msg);
                }
                // LCOV_EXCL_STOP
                tree.resize(cur_tree_size + nn_next_level);

                // Step 3: prefix sum over the number of children for each
                // node in the range.
                oneapi::tbb::parallel_scan(
                    oneapi::tbb::blocked_range<decltype(nc_buf.size())>(0, nc_buf.size()),
                    static_cast<std::uint32_t>(0),
                    [&](const auto &r, auto sum, bool is_final_scan) {
                        auto temp = sum;

                        for (auto i = r.begin(); i < r.end(); ++i) {
                            temp = temp + nc_buf[i];

                            if (is_final_scan) {
                                ps_buf[i] = temp;
                            }
                        }

                        return temp;
                    },
                    std::plus<>{});

                // Step 4: finalise the nodes in the range with the children pointers,
                // and perform the initial setup of the children nodes that were
                // added in step 2.
                oneapi::tbb::parallel_for(oneapi::tbb::blocked_range(n_begin, n_end), [&](const auto &rn) {
                    for (auto node_idx = rn.begin(); node_idx != rn.end(); ++node_idx) {
                        assert(node_idx - n_begin < cur_n_nodes);

                        auto &cur_node = tree[node_idx];

                        // Fetch the number of children.
                        const auto nc = nc_buf[node_idx - n_begin];

                        // Set the nn_level member. This needs to be done
                        // regardless of whether the node is internal or a leaf.
                        cur_node.nn_level = cur_n_nodes;

                        if (nc == 0u) {
                            // NOTE: no need for further finalisation of leaf nodes.
                            // Ensure that the AABB was correctly set up.
                            assert(cur_node.lb != default_lb);
                            assert(cur_node.ub != default_ub);
                        } else {
                            // Internal node.

                            // Fetch the number of particles in the left child.
                            const auto lsize = nplc_buf[node_idx - n_begin];

                            // Compute the index in the tree into which the left child will
                            // be stored.
                            // NOTE: this computation is safe because we checked earlier
                            // that cur_tree_size + nn_next_level can be computed safely.
                            const auto lc_idx = cur_tree_size + ps_buf[node_idx - n_begin] - 2u;
                            assert(lc_idx >= cur_tree_size);
                            assert(lc_idx < tree.size());
                            assert(lc_idx + 1u > cur_tree_size);
                            assert(lc_idx + 1u < tree.size());

                            // Assign the children indices for the current node.
                            cur_node.left = boost::numeric_cast<decltype(cur_node.left)>(lc_idx);
                            cur_node.right = boost::numeric_cast<decltype(cur_node.right)>(lc_idx + 1u);

                            // Set up the children's initial properties.
                            auto &lc = tree[lc_idx];
                            auto &rc = tree[lc_idx + 1u];

                            lc.begin = cur_node.begin;
                            // NOTE: the computation is safe
                            // because we know we can represent nparts
                            // as a std::uint32_t.
                            lc.end = cur_node.begin + lsize;
                            lc.parent = boost::numeric_cast<std::int32_t>(node_idx);
                            lc.left = -1;
                            lc.right = -1;
                            lc.lb = default_lb;
                            lc.ub = default_ub;
                            lc.nn_level = 0;
                            lc.split_idx = cur_node.split_idx + 1;

                            rc.begin = cur_node.begin + lsize;
                            rc.end = cur_node.end;
                            rc.parent = boost::numeric_cast<std::int32_t>(node_idx);
                            rc.left = -1;
                            rc.right = -1;
                            rc.lb = default_lb;
                            rc.ub = default_ub;
                            rc.nn_level = 0;
                            rc.split_idx = cur_node.split_idx + 1;
                        }
                    }
                });

                // Assign the next value for cur_n_nodes.
                // If nn_next_level is zero, this means that
                // all the nodes processed in this iteration
                // were leaves, and this signals the end of the
                // construction of the tree.
                cur_n_nodes = nn_next_level;

                ++n_levels;
                n_nodes += cur_n_nodes;
            }

            // Perform a backwards pass on the tree to compute the AABBs
            // of the internal nodes.

            // Node index range for the last level.
            auto n_begin = tree.size() - tree.back().nn_level;
            auto n_end = tree.size();

#if !defined(NDEBUG)
            // Double check that all nodes in the last level are
            // indeed leaves.
            assert(std::all_of(tree.data() + n_begin, tree.data() + n_end,
                               [](const auto &cur_node) { return cur_node.left == -1; }));
#endif

            // NOTE: because the AABBs for the leaf nodes were already computed,
            // we can skip the AABB computation for the nodes in the last level,
            // which are all guaranteed to be leaf nodes.
            // NOTE: if n_begin == 0u, it means the tree consists
            // only of the root node, which is itself a leaf.
            if (n_begin == 0u) {
                assert(n_end == 1u);
            } else {
                // Compute the range of the penultimate level.
                auto new_n_end = n_begin;
                n_begin -= tree[n_begin - 1u].nn_level;
                n_end = new_n_end;

                while (true) {
                    oneapi::tbb::parallel_for(oneapi::tbb::blocked_range(n_begin, n_end), [&](const auto &rn) {
                        for (auto node_idx = rn.begin(); node_idx != rn.end(); ++node_idx) {
                            auto &cur_node = tree[node_idx];

                            if (cur_node.left == -1) {
                                // Leaf node, the bounding box was computed earlier.
                                // Just verify it in debug mode.
#if !defined(NDEBUG)
                                auto dbg_lb = default_lb, dbg_ub = default_ub;

                                for (auto pidx = cur_node.begin; pidx != cur_node.end; ++pidx) {
                                    for (auto i = 0u; i < 4u; ++i) {
                                        dbg_lb[i] = std::min(dbg_lb[i], srt_lbs(chunk_idx, pidx, i));
                                        dbg_ub[i] = std::max(dbg_ub[i], srt_ubs(chunk_idx, pidx, i));
                                    }
                                }

                                assert(cur_node.lb == dbg_lb);
                                assert(cur_node.ub == dbg_ub);
#endif
                            } else {
                                // Internal node, compute its AABB from the children.
                                auto &lc = tree[static_cast<decltype(tree.size())>(cur_node.left)];
                                auto &rc = tree[static_cast<decltype(tree.size())>(cur_node.right)];

                                for (auto j = 0u; j < 4u; ++j) {
                                    // NOTE: min/max is fine here, we already checked
                                    // that all AABBs are finite.
                                    cur_node.lb[j] = std::min(lc.lb[j], rc.lb[j]);
                                    cur_node.ub[j] = std::max(lc.ub[j], rc.ub[j]);
                                }
                            }
                        }
                    });

                    if (n_begin == 0u) {
                        // We reached the root node, break out.
                        assert(n_end == 1u);
                        break;
                    } else {
                        // Compute the range of the previous level.
                        new_n_end = n_begin;
                        n_begin -= tree[n_begin - 1u].nn_level;
                        n_end = new_n_end;
                    }
                }
            }

            SPDLOG_LOGGER_DEBUG(logger, "Tree levels/nodes for chunk {}: {}/{}", chunk_idx, n_levels, n_nodes);
        }
    });

    logger->trace("BVH construction time: {}s", sw);

#if !defined(NDEBUG)
    verify_bvh_trees_parallel();
#endif
}

void sim::verify_bvh_trees_parallel() const
{
    namespace stdex = std::experimental;

    const auto nparts = get_nparts();
    const auto nchunks = m_data->nchunks;

    // Views for accessing the lbs/ubs data.
    using b_size_t = decltype(m_data->lbs.size());
    stdex::mdspan lbs(m_data->lbs.data(),
                      stdex::extents<b_size_t, stdex::dynamic_extent, stdex::dynamic_extent, 4u>(nchunks, nparts));
    stdex::mdspan ubs(m_data->ubs.data(),
                      stdex::extents<b_size_t, stdex::dynamic_extent, stdex::dynamic_extent, 4u>(nchunks, nparts));

    // Same for the sorted counterparts.
    stdex::mdspan srt_lbs(m_data->srt_lbs.data(),
                          stdex::extents<b_size_t, stdex::dynamic_extent, stdex::dynamic_extent, 4u>(nchunks, nparts));
    stdex::mdspan srt_ubs(m_data->srt_ubs.data(),
                          stdex::extents<b_size_t, stdex::dynamic_extent, stdex::dynamic_extent, 4u>(nchunks, nparts));

    // Morton codes views.
    using m_size_t = decltype(m_data->mcodes.size());
    stdex::mdspan mcodes(m_data->mcodes.data(),
                         stdex::extents<m_size_t, stdex::dynamic_extent, stdex::dynamic_extent>(nchunks, nparts));
    stdex::mdspan srt_mcodes(m_data->srt_mcodes.data(),
                             stdex::extents<m_size_t, stdex::dynamic_extent, stdex::dynamic_extent>(nchunks, nparts));

    // View for accessing the indices vector.
    using idx_size_t = decltype(m_data->vidx.size());
    stdex::mdspan vidx(m_data->vidx.data(),
                       stdex::extents<idx_size_t, stdex::dynamic_extent, stdex::dynamic_extent>(nchunks, nparts));

    oneapi::tbb::parallel_for(oneapi::tbb::blocked_range(0u, nchunks), [&](const auto &range) {
        for (auto chunk_idx = range.begin(); chunk_idx != range.end(); ++chunk_idx) {
            const auto &bvh_tree = m_data->bvh_trees[chunk_idx];

            std::set<size_type> pset;

            for (decltype(bvh_tree.size()) i = 0; i < bvh_tree.size(); ++i) {
                const auto &cur_node = bvh_tree[i];

                // The node must contain 1 or more particles.
                assert(cur_node.end > cur_node.begin);

                // The node must have either 0 or 2 children.
                if (cur_node.left == -1) {
                    assert(cur_node.right == -1);
                } else {
                    assert(cur_node.left > 0);
                    assert(cur_node.right > 0);
                }

                if (cur_node.end - cur_node.begin == 1u) {
                    // A node with a single particle is a leaf and must have no children.
                    assert(cur_node.left == -1);
                    assert(cur_node.right == -1);

                    // Add the particle to the global particle set,
                    // ensuring the particle has not been added to pset yet.
                    assert(pset.find(boost::numeric_cast<size_type>(cur_node.begin)) == pset.end());
                    pset.insert(boost::numeric_cast<size_type>(cur_node.begin));
                } else if (cur_node.left == -1) {
                    // A leaf with multiple particles.
                    assert(cur_node.right == -1);

                    // All particles must have the same Morton code.
                    const auto mc = srt_mcodes(chunk_idx, cur_node.begin);

                    // Make also sure that all particles are accounted
                    // for in pset.
                    assert(pset.find(boost::numeric_cast<size_type>(cur_node.begin)) == pset.end());
                    pset.insert(boost::numeric_cast<size_type>(cur_node.begin));

                    for (auto j = cur_node.begin + 1u; j < cur_node.end; ++j) {
                        assert(srt_mcodes(chunk_idx, j) == mc);

                        assert(pset.find(boost::numeric_cast<size_type>(j)) == pset.end());
                        pset.insert(boost::numeric_cast<size_type>(j));
                    }
                }

                if (cur_node.left != -1) {
                    // A node with children.
                    assert(cur_node.left > 0);
                    assert(cur_node.right > 0);

                    const auto uleft = static_cast<std::uint32_t>(cur_node.left);
                    const auto uright = static_cast<std::uint32_t>(cur_node.right);

                    // The children indices must be greater than the current node's
                    // index and within the tree.
                    assert(uleft > i && uleft < bvh_tree.size());
                    assert(uright > i && uright < bvh_tree.size());

                    // Check that the ranges of the children are consistent with
                    // the range of the current node.
                    assert(bvh_tree[uleft].begin == cur_node.begin);
                    assert(bvh_tree[uleft].end < cur_node.end);
                    assert(bvh_tree[uright].begin == bvh_tree[uleft].end);
                    assert(bvh_tree[uright].end == cur_node.end);

                    // The node's split_idx value must not be larger than 63.
                    assert(cur_node.split_idx <= 63);

#if defined(__GNUC__) || defined(__clang__)
                    // Check that a node with children was split correctly (i.e.,
                    // cur_node.split_idx corresponds to the index of the first
                    // different bit at the boundary between first and second child).
                    const auto split_idx = bvh_tree[uleft].end - 1u;
                    assert(
                        detail::first_diff_bit(srt_mcodes(chunk_idx, split_idx), srt_mcodes(chunk_idx, split_idx + 1u))
                        == cur_node.split_idx);
                    assert(srt_mcodes(chunk_idx, split_idx) == mcodes(chunk_idx, vidx(chunk_idx, split_idx)));
#endif
                } else {
                    // A node with no children. In this case the maximum
                    // split_idx value can be 64, if the node was created
                    // from the split of a node whose particles' Morton codes
                    // differred at the last possible bit.
                    assert(cur_node.split_idx <= 64);
                }

                // Check the parent info.
                if (i == 0u) {
                    assert(cur_node.parent == -1);
                } else {
                    assert(cur_node.parent >= 0);

                    const auto upar = static_cast<std::uint32_t>(cur_node.parent);

                    assert(upar < i);
                    assert(cur_node.begin >= bvh_tree[upar].begin);
                    assert(cur_node.end <= bvh_tree[upar].end);
                    assert(cur_node.begin == bvh_tree[upar].begin || cur_node.end == bvh_tree[upar].end);
                }

                // nn_level must alway be nonzero.
                assert(cur_node.nn_level > 0u);

                // Check that the AABB of the node is correct.
                constexpr auto finf = std::numeric_limits<float>::infinity();
                std::array<float, 4> lb = {finf, finf, finf, finf};
                std::array<float, 4> ub = {-finf, -finf, -finf, -finf};

                for (auto j = cur_node.begin; j < cur_node.end; ++j) {
                    for (auto k = 0u; k < 4u; ++k) {
                        assert(srt_lbs(chunk_idx, j, k) == lbs(chunk_idx, vidx(chunk_idx, j), k));
                        lb[k] = std::min(lb[k], srt_lbs(chunk_idx, j, k));
                        assert(srt_ubs(chunk_idx, j, k) == ubs(chunk_idx, vidx(chunk_idx, j), k));
                        ub[k] = std::max(ub[k], srt_ubs(chunk_idx, j, k));
                    }
                }

                assert(lb == cur_node.lb);
                assert(ub == cur_node.ub);
            }
        }
    });
}

} // namespace cascade
