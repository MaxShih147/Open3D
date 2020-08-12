// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "open3d/core/Indexer.h"
#include "open3d/core/SparseIndexer.h"
#include "open3d/core/kernel/CPULauncher.h"
#include "open3d/core/kernel/SpecialOp.h"
#include "open3d/pipelines/integration/MarchingCubesConst.h"

namespace open3d {
namespace core {
namespace kernel {

void CPUIntegrateKernel(int64_t workload_idx) {}

void SpecialOpEWCPU(const std::vector<Tensor>& input_tensors,
                    const std::vector<SparseTensorList>& input_sparse_tls,
                    Tensor& output_tensor,
                    SparseTensorList& output_sparse_tl,
                    SpecialOpCode op_code) {
    switch (op_code) {
        case SpecialOpCode::Integrate: {
            // sparse_tls: tsdf grid
            // tensors: depth, intrinsic, extrinsic
            SizeVector grid_shape = output_sparse_tl.shapes_[0];
            float voxel_size = input_tensors[3][0].Item<float>();
            float sdf_trunc = input_tensors[4][0].Item<float>();

            SparseIndexer sparse_indexer(output_sparse_tl,
                                         grid_shape.NumElements());
            NDArrayIndexer indexer3d(grid_shape,
                                     DtypeUtil::ByteSize(Dtype::Float32));
            SizeVector chw = input_tensors[0].GetShape();
            NDArrayIndexer indexer2d({chw[1], chw[2]},
                                     DtypeUtil::ByteSize(Dtype::Float32),
                                     input_tensors[0].GetDataPtr());

            Projector projector(input_tensors[1], input_tensors[2], voxel_size);

            int64_t n = sparse_indexer.NumWorkloads();
            CPULauncher::LaunchGeneralKernel(n, [=](int64_t workload_idx) {
                int64_t key_idx, value_idx;
                sparse_indexer.GetSparseWorkloadIdx(workload_idx, &key_idx,
                                                    &value_idx);

                int64_t xl, yl, zl;
                indexer3d.ConvertOffsetTo3D(value_idx, &xl, &yl, &zl);

                void* key_ptr = sparse_indexer.GetWorkloadKeyPtr(key_idx);
                int64_t xg = *(static_cast<int64_t*>(key_ptr) + 0);
                int64_t yg = *(static_cast<int64_t*>(key_ptr) + 1);
                int64_t zg = *(static_cast<int64_t*>(key_ptr) + 2);

                int64_t resolution = indexer3d.GetShape(0);
                int64_t x = (xg * resolution + xl);
                int64_t y = (yg * resolution + yl);
                int64_t z = (zg * resolution + zl);

                float xc, yc, zc, u, v;
                projector.Transform(static_cast<float>(x),
                                    static_cast<float>(y),
                                    static_cast<float>(z), &xc, &yc, &zc);
                projector.Project(xc, yc, zc, &u, &v);

                if (!indexer2d.InBoundary2D(u, v)) {
                    return;
                }

                int64_t offset;
                indexer2d.Convert2DToOffset(static_cast<int64_t>(u),
                                            static_cast<int64_t>(v), &offset);
                float depth = *static_cast<const float*>(
                        indexer2d.GetPtrFromOffset(offset));

                float sdf = depth - zc;
                if (depth <= 0 || zc <= 0 || sdf < -sdf_trunc) {
                    return;
                }
                sdf = sdf < sdf_trunc ? sdf : sdf_trunc;
                sdf /= sdf_trunc;

                void* tsdf_ptr = sparse_indexer.GetWorkloadValuePtr(key_idx, 0,
                                                                    value_idx);
                void* weight_ptr = sparse_indexer.GetWorkloadValuePtr(
                        key_idx, 1, value_idx);

                float tsdf_sum = *static_cast<float*>(tsdf_ptr);
                float weight_sum = *static_cast<float*>(weight_ptr);
                *static_cast<float*>(tsdf_ptr) =
                        (weight_sum * tsdf_sum + sdf) / (weight_sum + 1);
                *static_cast<float*>(weight_ptr) = weight_sum + 1;
            });
            utility::LogInfo("[SpecialOpEWCPU] CPULauncher finished");
            break;
        };

        case SpecialOpCode::ExtractSurface: {
            utility::LogInfo("ExtractSurface");
            // input_sparse_tls: tsdf grid
            // output_sparse_tl: surface grid
            // tensors: voxel_size, sdf_trunc
            SizeVector grid_shape = output_sparse_tl.shapes_[0];
            float voxel_size = input_tensors[0][0].Item<float>();

            // res x res x res
            NDArrayIndexer indexer3d(grid_shape,
                                     DtypeUtil::ByteSize(Dtype::Int32));
            // 27 x n
            NDArrayIndexer indexer2d(input_tensors[1].GetShape(),
                                     DtypeUtil::ByteSize(Dtype::Bool),
                                     input_tensors[1].GetDataPtr());
            // n => res x res x res
            SparseIndexer tsdf_indexer(input_sparse_tls[0],
                                       grid_shape.NumElements());

            // 27 x n => res x res x res
            SparseIndexer tsdf_nb_indexer(input_sparse_tls[1],
                                          grid_shape.NumElements());

            SparseIndexer surf_indexer(output_sparse_tl,
                                       grid_shape.NumElements());

            int64_t n = tsdf_indexer.NumWorkloads();
            int64_t m = input_tensors[1].GetShape()[1];

            Device device = output_sparse_tl.device_;
            Tensor count(std::vector<int>{0}, {1}, Dtype::Int32, device);
            int* count_ptr = static_cast<int*>(count.GetDataPtr());

            // TODO: adaptive
            int total_count = 1700000;
            Tensor vertices_x({total_count}, Dtype::Float32, device);
            Tensor vertices_y({total_count}, Dtype::Float32, device);
            Tensor vertices_z({total_count}, Dtype::Float32, device);
            float* vertices_x_ptr =
                    static_cast<float*>(vertices_x.GetDataPtr());
            float* vertices_y_ptr =
                    static_cast<float*>(vertices_y.GetDataPtr());
            float* vertices_z_ptr =
                    static_cast<float*>(vertices_z.GetDataPtr());

            CPULauncher::LaunchGeneralKernel(n, [=](int64_t workload_idx) {
                int64_t key_idx, value_idx;
                tsdf_indexer.GetSparseWorkloadIdx(workload_idx, &key_idx,
                                                  &value_idx);

                int64_t resolution = indexer3d.GetShape(0);

                float tsdf_o =
                        *static_cast<float*>(tsdf_indexer.GetWorkloadValuePtr(
                                key_idx, 0, value_idx));
                float weight_o =
                        *static_cast<float*>(tsdf_indexer.GetWorkloadValuePtr(
                                key_idx, 1, value_idx));
                if (weight_o == 0) return;

                int64_t xl, yl, zl;
                indexer3d.ConvertOffsetTo3D(value_idx, &xl, &yl, &zl);
                for (int i = 0; i < 3; ++i) {
                    int64_t xl_i = xl + int(i == 0);
                    int64_t yl_i = yl + int(i == 1);
                    int64_t zl_i = zl + int(i == 2);

                    int dx = xl_i / resolution;
                    int dy = yl_i / resolution;
                    int dz = zl_i / resolution;

                    int nb_idx = (dx + 1) + (dy + 1) * 3 + (dz + 1) * 9;

                    int64_t nb_mask_offset;
                    indexer2d.Convert2DToOffset(key_idx, nb_idx,
                                                &nb_mask_offset);
                    bool nb_valid = *static_cast<bool*>(
                            indexer2d.GetPtrFromOffset(nb_mask_offset));
                    if (!nb_valid) continue;

                    int64_t nb_value_idx;
                    indexer3d.Convert3DToOffset(
                            xl_i - dx * resolution, yl_i - dy * resolution,
                            zl_i - dz * resolution, &nb_value_idx);
                    float tsdf_i = *static_cast<float*>(
                            tsdf_nb_indexer.GetWorkloadValuePtr(
                                    nb_idx * m + key_idx, 0, nb_value_idx));
                    float weight_i = *static_cast<float*>(
                            tsdf_nb_indexer.GetWorkloadValuePtr(
                                    nb_idx * m + key_idx, 1, nb_value_idx));

                    if (weight_i > 0 && tsdf_i * tsdf_o < 0) {
                        float ratio = tsdf_i / (tsdf_i - tsdf_o);

                        int* vertex_ind = static_cast<int*>(
                                surf_indexer.GetWorkloadValuePtr(key_idx, i,
                                                                 value_idx));

                        void* key_ptr = tsdf_indexer.GetWorkloadKeyPtr(key_idx);
                        int64_t xg = *(static_cast<int64_t*>(key_ptr) + 0);
                        int64_t yg = *(static_cast<int64_t*>(key_ptr) + 1);
                        int64_t zg = *(static_cast<int64_t*>(key_ptr) + 2);

                        int idx;
                        // https://stackoverflow.com/questions/4034908/fetch-and-add-using-openmp-atomic-operations
#ifdef _OPENMP
#pragma omp atomic capture
#endif
                        {
                            idx = *count_ptr;
                            *count_ptr += 1;
                        }
                        *vertex_ind = idx;
                        vertices_x_ptr[idx] =
                                voxel_size *
                                (xg * resolution + xl + ratio * int(i == 0));
                        vertices_y_ptr[idx] =
                                voxel_size *
                                (yg * resolution + yl + ratio * int(i == 1));
                        vertices_z_ptr[idx] =
                                voxel_size *
                                (zg * resolution + zl + ratio * int(i == 2));
                    }
                }
            });
            int actual_count = count[0].Item<int>();
            std::cout << actual_count << "\n";

            output_tensor = Tensor({3, actual_count}, Dtype::Float32, device);
            output_tensor[0].Slice(0, 0, actual_count) =
                    vertices_x.Slice(0, 0, actual_count);
            output_tensor[1].Slice(0, 0, actual_count) =
                    vertices_y.Slice(0, 0, actual_count);
            output_tensor[2].Slice(0, 0, actual_count) =
                    vertices_z.Slice(0, 0, actual_count);
            break;
        };

            //         case SpecialOpCode::MarchingCubesPass0: {
            //             utility::LogInfo("MC Pass0");
            //             // input_sparse_tls: tsdf grid
            //             // output_sparse_tl: surface grid
            //             // tensors: voxel_size, sdf_trunc
            //             SizeVector grid_shape = output_sparse_tl.shapes_[0];
            //             float voxel_size = input_tensors[0][0].Item<float>();

            //             // res x res x res
            //             NDArrayIndexer indexer3d(grid_shape,
            //                                      DtypeUtil::ByteSize(Dtype::Int32));
            //             // 27 x n
            //             NDArrayIndexer indexer2d(input_tensors[1].GetShape(),
            //                                      DtypeUtil::ByteSize(Dtype::Bool),
            //                                      input_tensors[1].GetDataPtr());
            //             // n => res x res x res
            //             SparseIndexer tsdf_indexer(input_sparse_tls[0],
            //                                        grid_shape.NumElements());

            //             // 27 x n => res x res x res
            //             SparseIndexer tsdf_nb_indexer(input_sparse_tls[1],
            //                                           grid_shape.NumElements());

            //             SparseIndexer surf_nb_indexer(output_sparse_tl,
            //                                           grid_shape.NumElements());

            //             int64_t n = tsdf_indexer.NumWorkloads();
            //             int64_t m = input_tensors[1].GetShape()[1];

            //             Device device = output_sparse_tl.device_;
            //             Tensor tri_count(std::vector<int>{0}, {1},
            //             Dtype::Int32, device); int* tri_count_ptr =
            //             static_cast<int*>(count.GetDataPtr());

            //             // Pass 0: allocate points and
            //             CPULauncher::LaunchGeneralKernel(n, [=](int64_t
            //             workload_idx) {
            //                 int64_t key_idx, value_idx;
            //                 tsdf_indexer.GetSparseWorkloadIdx(workload_idx,
            //                 &key_idx,
            //                                                   &value_idx);

            //                 int64_t xl, yl, zl;
            //                 indexer3d.ConvertOffsetTo3D(value_idx, &xl, &yl,
            //                 &zl); int64_t resolution = indexer3d.GetShape(0);

            //                 // Enumerate 8 neighbor corners (including
            //                 itself) int table_index = 0; for (int i = 0; i <
            //                 8; ++i) {
            //                     int64_t xl_i = xl + (i & 1);
            //                     int64_t yl_i = yl + (i & 2);
            //                     int64_t zl_i = zl + (i & 4);

            //                     int dx = (xl_i + 1) / resolution;
            //                     int dy = (yl_i + 1) / resolution;
            //                     int dz = (zl_i + 1) / resolution;

            //                     int nb_idx = (dx + 1) + (dy + 1) * 3 + (dz +
            //                     1) * 9;

            //                     int64_t offset;
            //                     indexer2d.Convert2DToOffset(key_idx, nb_idx,
            //                     &offset); bool nb_valid =
            //                     *static_cast<bool*>(
            //                             indexer2d.GetPtrFromOffset(offset));
            //                     if (!nb_valid) return;

            //                     int64_t nb_value_idx;
            //                     indexer3d.Convert3DToOffset(
            //                             xl_i - dx * resolution, yl_i - dy *
            //                             resolution, zl_i - dz * resolution,
            //                             &nb_value_x);
            //                     float tsdf_i = *static_cast<float*>(
            //                             tsdf_nb_indexer.GetWorkloadValuePtr(
            //                                     nb_idx * m + key_idx, 0,
            //                                     nb_value_idx));
            //                     float weight_i = *static_cast<float*>(
            //                             tsdf_nb_indexer.GetWorkloadValuePtr(
            //                                     nb_idx * m + key_idx, 1,
            //                                     nb_value_idx));
            //                     if (weight_i == 0 || fabsf(tsdf_i) >= 0.95)
            //                     return;

            //                     table_index |= ((tsdf_i < 0) ? (1 << i) : 0);
            //                 }
            //                 if (table_index == 0 || table_index == 255)
            //                 return;

            //                 // Enumerate 12 edges
            //                 int edges_w_vertices = edge_table[table_index];
            //                 for (int i = 0; i < 12; ++i) {
            //                     if (edges_w_vertices & (1 << edge)) {
            //                         int64_t xl_i = xl + edge_shifts[i][0];
            //                         int64_t yl_i = yl + edge_shifts[i][1];
            //                         int64_t zl_i = zl + edge_shifts[i][2];
            //                         int edge_i = edge_shifts[i][3];

            //                         int dx = (xl_i + 1) / resolution;
            //                         int dy = (yl_i + 1) / resolution;
            //                         int dz = (zl_i + 1) / resolution;

            //                         int nb_idx = (dx + 1) + (dy + 1) * 3 +
            //                         (dz + 1) * 9;

            //                         // no need to check nb_valid now
            //                         int64_t nb_value_idx;
            //                         indexer3d.Convert3DToOffset(
            //                                 xl_i - dx * resolution, yl_i - dy
            //                                 * resolution, zl_i - dz *
            //                                 resolution, &nb_value_x);
            //                         int* vertex_idx = static_cast<int*>(
            //                                 surf_nb_indexer.GetWorkloadValuePtr(
            //                                         nb_idx * m + key_idx,
            //                                         edge_i, nb_value_idx));
            //                         // to be allocated in the next pass
            //                         *vertex_idx = -1;
            //                     }
            //                 }
            // #ifdef _OPENMP
            // #pragma omp critical
            // #endif
            //                 { *tri_count_ptr += tri_count[table_index]; }
            //             });

            //             break;
            //         }

            //         case SpecialOpCode::MarchingCubesPass1: {
            //             // input_sparse_tls: tsdf grid
            //             // output_sparse_tl: surface grid
            //             // tensors: voxel_size, sdf_trunc
            //             SizeVector grid_shape = output_sparse_tl.shapes_[0];
            //             float voxel_size = input_tensors[0][0].Item<float>();

            //             // res x res x res
            //             NDArrayIndexer indexer3d(grid_shape,
            //                                      DtypeUtil::ByteSize(Dtype::Int32));
            //             // 27 x n
            //             NDArrayIndexer indexer2d(input_tensors[1].GetShape(),
            //                                      DtypeUtil::ByteSize(Dtype::Bool),
            //                                      input_tensors[1].GetDataPtr());
            //             // n => res x res x res
            //             SparseIndexer tsdf_indexer(input_sparse_tls[0],
            //                                        grid_shape.NumElements());

            //             // 27 x n => res x res x res
            //             SparseIndexer tsdf_nb_indexer(input_sparse_tls[1],
            //                                           grid_shape.NumElements());

            //             SparseIndexer surf_indexer(output_sparse_tl,
            //                                        grid_shape.NumElements());

            //             int64_t n = tsdf_indexer.NumWorkloads();
            //             int64_t m = input_tensors[1].GetShape()[1];

            //             Device device = output_sparse_tl.device_;
            //             Tensor vtx_count(std::vector<int>{0}, {1},
            //             Dtype::Int32, device); int* vtx_count_ptr =
            //             static_cast<int*>(count.GetDataPtr());

            //             // Pass 1: allocate points and
            //             CPULauncher::LaunchGeneralKernel(n, [=](int64_t
            //             workload_idx) {
            //                 int64_t key_idx, value_idx;
            //                 tsdf_indexer.GetSparseWorkloadIdx(workload_idx,
            //                 &key_idx,
            //                                                   &value_idx);

            //                 int64_t xl, yl, zl;
            //                 indexer3d.ConvertOffsetTo3D(value_idx, &xl, &yl,
            //                 &zl); int64_t resolution = indexer3d.GetShape(0);

            //                 float tsdf_o =
            //                         *static_cast<float*>(tsdf_indexer.GetWorkloadValuePtr(
            //                                 key_idx, 0, value_idx));

            //                 // Enumerate 8 neighbor corners (including
            //                 itself) int table_index = 0; for (int i = 0; i <
            //                 3; ++i) {
            //                     int* vertex_idx =
            //                             static_cast<int*>(surf_indexer.GetWorkloadValuePtr(
            //                                     key_idx, i, value_idx));
            //                     if (*vertex_idx != -1) {
            //                         continue;
            //                     }

            //                     int64_t xl_i = xl + (i == 1);
            //                     int64_t yl_i = yl + (i == 2);
            //                     int64_t zl_i = zl + (i == 3);

            //                     int dx = (xl_i + 1) / resolution;
            //                     int dy = (yl_i + 1) / resolution;
            //                     int dz = (zl_i + 1) / resolution;

            //                     int nb_idx = (dx + 1) + (dy + 1) * 3 + (dz +
            //                     1) * 9;

            //                     // // Must be true
            //                     // int64_t offset;
            //                     // indexer2d.Convert2DToOffset(key_idx,
            //                     nb_idx, &offset);
            //                     // bool nb_valid = *static_cast<bool*>(
            //                     // indexer2d.GetPtrFromOffset(offset));
            //                     // if (!nb_valid) return;

            //                     int64_t nb_value_idx;
            //                     indexer3d.Convert3DToOffset(
            //                             xl_i - dx * resolution, yl_i - dy *
            //                             resolution, zl_i - dz * resolution,
            //                             &nb_value_x);

            //                     float tsdf_i = *static_cast<float*>(
            //                             tsdf_nb_indexer.GetWorkloadValuePtr(
            //                                     nb_idx * m + key_idx, 0,
            //                                     nb_value_idx));

            // #ifdef _OPENMP
            // #pragma omp critical
            // #endif
            //                     { *tri_count_ptr += tri_count[table_index]; }
            //                 }
            //             });

            //             break;
            //         }
        default: { utility::LogError("Unsupported special op"); }
    }

    utility::LogInfo("[SpecialOpEWCPU] Exiting SpecialOpEWCPU");
}
}  // namespace kernel
}  // namespace core
}  // namespace open3d
