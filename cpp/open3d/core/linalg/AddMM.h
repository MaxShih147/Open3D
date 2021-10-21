// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018-2021 www.open3d.org
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

#pragma once

#include "open3d/core/Tensor.h"

namespace open3d {
namespace core {

/// Computes matrix multiplication C = AB.
template <class T>
void AddMM(const Tensor& A, const Tensor& B, Tensor& C, T alpha, T beta);

#ifdef BUILD_CUDA_MODULE
template <class T>
void AddMMCUDA(void* A_data,
               void* B_data,
               void* C_data,
               int64_t m,
               int64_t k,
               int64_t n,
               T alpha,
               T beta,
               int lda,
               int ldb,
               int ldc);
#endif

template <class T>
void AddMMCPU(void* A_data,
              void* B_data,
              void* C_data,
              int64_t m,
              int64_t k,
              int64_t n,
              T alpha,
              T beta,
              int lda,
              int ldb,
              int ldc);
}  // namespace core
}  // namespace open3d
