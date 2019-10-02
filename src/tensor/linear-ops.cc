// tensor/linear-ops.cc

// Copyright      2019  Johns Hopkins University (author: Daniel Povey)

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#include "tensor/linear-ops.h"

namespace kaldi {
namespace tensor {


void PlusEqOp::Expand(std::vector<std::unique_ptr<Op> > *ops) const {
  if (a_.DeviceType() == kCpuDevice) ExpandCpu(ops);
  else ExpandCuda(ops);
}

void PlusEqOp::ExpandCpu(std::vector<std::unique_ptr<Op> > *ops) const {
  Op *new_op;
  if (ReferenceMode()) {
    // In reference mode on CPU always use the reference implementation.
    // Reference mode is only supported on CPU so we use the normal Ops
    // on GPU.
    SET_TO_TEMPLATED_CPU_OP_ALL(new_op, a_.Dtype(), PlusEqRefOp, a_, b_);
    ops->push_back(new_op);
    return;
  }

  // The implementation requires us to first reduce the patterns,
  // so we don't have too many combinations of codes to handle.
  Pattern a_pattern = a_.Impl().pattern,
      b_pattern = b_.Impl().pattern;
  ReducePatterns({a_pattern, b_pattern});

  // The few lines below construct Tensors a and b which have the same data as
  // a_ and b_, but with reduced patterns; we use a_ and b_ directly if the
  // reduction made no difference.
  Tensor a(a_), b(b_);
  if (a_pattern != a_.Impl().pattern)
    a = WithPattern(a, a_pattern);
  if (b_pattern != b_.Impl().pattern)
    b = WithPattern(b, b_pattern);


  int64 combined_code = CombineCodes(a_pattern.GetCode(),
                                     b_pattern.GetCode());

  /*
    'combined_code' may be viewed as a hex number 0xAAABBB where AAA is
    the code of a_pattern and BBB is the code of b_pattern.  See
    documentation for ComputePatternCode() in pattern-utils.h for
    more documentation on the meanings of the values and our notation
    with X,x,1.
       Quick legend:
             X means dim >1, stride = 1
             x means dim >1, stride != 1
             1 means dim == 1, stride = 0.
                 (Note: the numbers in case-statements below exclude negative
                 strides because bit 11 of the 12-bit chunks would be set if
                 there were a negative stride).
       Rightmost position in these (xX)-type notations below is the
       highest-numbered axis / lowest-number raxis
  */


  // We implemented the blas-like operations for general T as well as the versions
  // that use BLAS, so we don't need to check if the type is float or double.

  // We are doing a += b.
  switch(combined_code) {
    // A scalar += scalar,
    case 0x000000:   // () +=  ()
      SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), ScalarPlusEqScalarCpuOp, a, b);
      break;
      // We may split apart some of the following cases in future.
      // They all represent, vector += vector.
    case 0x101101:  //  (X) += (X)
    case 0x001001:  //  (x) += (x)
    case 0x101001:  //  (X) += (x)
    case 0x001101:  //  (X) += (x)
      SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), StvectorPlusEqStvectorCpuOp, a, b);
      break;
      // Scalar += (sum of) vector or strided vector
    case 0x000101:  //  () += (X)
    case 0x000001:  //  () += (X)
      SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), ScalarPlusEqStvectorCpuOp, a, b);
      break;
      // vector or strided vector += scalar.
      // We could later split apart the strided and non-strided cases.
    case 0x101000:  //  (x) += ()
    case 0x001000:  //  (X) += ()
      SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), StvectorPlusEqScalarCpuOp, a, b);
      break;
      // scalar += matrix
    case 0x000103: { // () += (xX)
      int32 num_rows = b.Pattern().dims[1];
      // Create a temporary- a column vector, which is what we call
      // a vector whose nontrivial axis is raxis 1 instead of raxis 0.
      Tensor temp({num_rows, 1}, {a.Dtype(), a.Device()});
      // Below we do temp += b.  We could use PlusEqOp for this and also for the
      // following reduction, but doing it this way avoids an unnecessary layer
      // of expansion.
      SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(),
                               ColVectorEqMatrixCpuOp, temp, b);
      ops->push_back(new_op);
      // Normalize the temporary vector so its nontrivial axis is raxis 0, by
      // removing the current raxis 0 and having current raxis 1 shift down.
      Tensor temp_normalized = SqueezeR(temp, 0);
      SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(),
                               ScalarPlusEqStvectorCpuOp, a,
                               temp_normalized);
      break;
    }
    case 0x101103: // (X) += (xX)
    case 0x001103: // (x) += (xX)
      // vector += matrix.  Implicitly this is a row vector, since its
      // nontrivial axis is in the same position as the column axis of the
      // matrix.  So we are summing the rows of the matrix.
      SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), StvectorPlusEqMatrix);

    default:
          // The reference op, which might be slow especially if there is
          // reduction.  We'll continue trying to add special handling for common
          // cases.
          SET_TO_TEMPLATED_OP_ALL(new_op, a_.Dtype(), PlusEqRefCpuOp, a_, b_);
  }
    } else {  // CPU, but not float or double.
      switch (dtype) {
        case kInt32Dtype:
          new_op = new PlusEqRefOp<int32>(a_, b_);
        default:
          KALDI_ERR << "Unexpected dtype: " << dtype;
      }
    }
    ops->push_back(new_op);
    return;
  } else {
    KALDI_ASSERT(a.DeviceType() == kCuda);
#if HAVE_CUDA == 1
    if (a.Dtype() == kFloat || a.Dtype() == kDouble) {
      // For certain special cases we have a BLAS implementation.
      switch(combined_code) {
        // We may split apart some of the following cases in future.
        // They all represent, vector += vector.
        case 0x101101:  //  (X) += (X)
        case 0x001001:  //  (x) += (x)
        case 0x101001:  //  (X) += (x)
        case 0x001101:  //  (X) += (x)
          SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), a.DeviceType(), StvectorPlusEqStvectorCudaOp, a, b);
          break;
          // Scalar += (sum of) vector or strided vector
        case 0x000101:  //  () += (X)
        case 0x000001:  //  () += (X)
          SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), a.DeviceType(), ScalarPlusEqStvectorCudaOp, a, b);
          break;
          // vector or strided vector += scalar.
          // We could later split apart the strided and non-strided cases.
        case 0x101000:  //  (x) += ()
        case 0x001000:  //  (X) += ()
          SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), a.DeviceType(), StvectorPlusEqScalarCudaOp, a, b);
          break;
          // scalar += matrix
        case 0x000103: { // () += (xX)
          int32 num_rows = b.Pattern().dims[1];
          // Create a temporary- a column vector, which is what we call
          // a vector whose nontrivial axis is raxis 1 instead of raxis 0.
          Tensor temp({num_rows, 1}, {a.Dtype(), a.Device()});
          // Below we do temp += b.  We could use PlusEqOp for this and also for the
          // following reduction, but doing it this way avoids an unnecessary layer
          // of expansion.
          SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), a.DeviceType(),
                                       ColVectorEqMatrixOp, temp, b);
          ops->push_back(new_op);
          // Normalize the temporary vector so its nontrivial axis is raxis 0, by
          // removing the current raxis 0 and having current raxis 1 shift down.
          Tensor temp_normalized = Squeeze(temp, 0);
          SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), a.DeviceType(),
                                   ScalarPlusEqStvectorOp, a, temp_normalized);
        }
      }
#else
      KALDI_ERR << "You have not compiled for CUDA but are trying to use GPU."
          "Please configure for GPU use and recompile."
#endif  //  HAVE_CUDA == 1
  }
}


void PlusEqOp::ExpandCpu(std::vector<std::unique_ptr<Op> > *ops) const {
  Op *new_op;
  // The implementation requires us to first normalize the patterns,
  // so we don't have too many combinations of codes to handle.
  Pattern a_pattern = a_.Impl().pattern,
      b_pattern = b_.Impl().pattern;
  NormalizePatterns({a_pattern, b_pattern});

  // The few lines below construct Tensors a and b which have the same data as
  // a_ and b_, but with reduced patterns; we use a_ and b_ directly if the
  // reduction made no difference.
  Tensor a(a_), b(b_);
  if (a_pattern != a_.Impl().pattern)
    a = WithPattern(a, a_pattern);
  if (b_pattern != b_.Impl().pattern)
    b = WithPattern(b, b_pattern);



}

void AssignOp::Expand() const {
  Op *new_op;

  if (a.Dtype() != b.Dtype()) {
    if (a.Device() != b.Device()) {
      KALDI_ERR << "Cross-device copying combined with type convesion not "
          "supported yet.";
      // Actually it would be easy to support just by creating a temporary
      // (search above for `temp` for an example).
    }


  }
  if (a.Device() != b.Device()) {
    KALDI_ERR << "Cross-device copying not supported yet.";
  }

  if (ReferenceMode() && a_.DeviceType() == kCpuDevice) {
    // In reference mode on CPU always use the reference implementation.
    // Reference mode is only supported on CPU so we use the normal Ops
    // on GPU.
    SET_TO_TEMPLATED_OP_ALLPAIRS(new_op, a_.Dtype(), b.Dtype(),
                                 AssignRefOp, a_, b_);
    ops->push_back(new_op);
    return;
  }

  // The generic implementation requires us to first normalize the patterns.
  Pattern a_pattern = a_.Impl().pattern,
      b_pattern = b_.Impl().pattern;
  NormalizePatterns({a_pattern, b_pattern});

  KALDI_ASSERT(Compatible(a_, b_));  // dtype and device, check they match.

  Tensor a(a_), b(b_);

  if (a_pattern != a_.Impl().pattern)
    a = WithPattern(a, a_pattern);
  if (b_pattern != b_.Impl().pattern)
    b = WithPattern(b, b_pattern);

  /*
    The case-statement values in the switch statement below may be interpreted
    in groups of 3 hex characters, are 0xAAABBB, pertaining to Tensors a and b
    respectively.  See GetPatternCode() in pattern-utils.h for documentation on
    the meanings of the values and our notation with X,x,1.

  */
  int64 combined_code = CombineCodes(a_pattern.GetCode(),
                                     b_pattern.GetCode());

  /*
    The case-statement values in the switch statement below may be interpreted
    in groups of 3 hex characters, are 0xAAABBB, pertaining to Tensors a and b.
    See ComputePatternCode() in pattern-utils.h for documentation on the meanings of
    the values and our notation with X,x,1.
       Quick legend:
             X means dim >1, stride = 1
             x means dim >1, stride != 1
             1 means dim == 1, stride = 0.
                 (Note: the numbers in case-statements below exclude negative
                 strides because bit 11 of the 12-bit chunks would be set if
                 there were a negative stride).
   */

  // We are doing a += b.
  switch(combined_code) {
    // A scalar += scalar,
    case 0x000000:   // () +=  ()
      SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), a.DeviceType(), ScalarPlusEqScalarOp, a, b);
      break;
    // We may split apart some of the following cases in future.
    // They all represent, vector += vector.
    case 0x101101:  //  (X) += (X)
    case 0x001001:  //  (x) += (x)
    case 0x101001:  //  (X) += (x)
    case 0x001101:  //  (X) += (x)
      SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), a.DeviceType(), StvectorPlusEqStvectorOp, a, b);
      break;
    // Scalar += (sum of) vector or strided vector
    case 0x000101:  //  () += (X)
    case 0x000001:  //  () += (X)
      SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), a.DeviceType(), ScalarPlusEqStvectorOp, a, b);
      break;
    // vector or strided vector += scalar.
    // We could later split apart the strided and non-strided cases.
    case 0x101000:  //  (x) += ()
    case 0x001000:  //  (X) += ()
      SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), a.DeviceType(), StvectorPlusEqScalarOp, a, b);
      break;
    // scalar += matrix
    case 0x000103: { // () += (xX)
      int32 num_rows = b.Pattern().dims[1];
      // Create a temporary- a column vector, which is what we call
      // a vector whose nontrivial axis is raxis 1 instead of raxis 0.
      Tensor temp({num_rows, 1}, {a.Dtype(), a.Device()});
      // Below we do temp += b.  We could use PlusEqOp for this and also for the
      // following reduction, but doing it this way avoids an unnecessary layer
      // of expansion.
      SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), a.DeviceType(),
                               ColVectorEqMatrixOp, temp, b);
      ops->push_back(new_op);
      // Normalize the temporary vector so its nontrivial axis is raxis 0, by
      // removing the current raxis 0 and having current raxis 1 shift down.
      Tensor temp_normalized = Squeeze(temp, 0);
      SET_TO_TEMPLATED_OP_REAL(new_op, a.Dtype(), a.DeviceType(),
                               ScalarPlusEqStvectorOp, a, temp_normalized);
    }


    default:
      // Later we can add a more generic implementation that handles arbitrary
      // patterns.
      KALDI_ERR << "Unhandled code: " << std::hex << combined_code;
  }
  ops->push_back(new_op);
}



void AddProduct(float alpha, float beta,
                const TensorImpl &a, const TensorImpl &b, const TensorImpl *c){

  if (a.pattern.code < b.pattern.code) {
    // Ensure, via a recursion, that a.pattern.code >= b.pattern.code.
    // This avoids us having to test for the swapped versions of the patterns.
    AddProduct(alpha, beta, b, a, c);
    return;
  }

  CheckDeviceAndDtype(a, b, *c);


  int64 combined_code = CombineCodes(a.pattern.code, b.pattern.code,
                                     c->pattern.code);

  /*
    The case-statement values in the switch statement below may be
    interpreted in groups of 3 hex characters, are 0xAAABBBCCC,
    pertaining to Tensors a, b and c respectively.  See
    GetPatternCode() in pattern-utils.h for documentation on
    the meanings of the values and our notation with X,x,1.
   */
  switch(combined_code) {
    case 0x000000000:
      // () * () -> ()
      // scalar * scalar -> scalar
      AddProductScalar3(a, b, c);
      return;
    case 0x101000101:
      //  (X) * ()-> (X)
      // vector * scalar -> vector
      AddProductVecScalarVec(a, b, c);
      return;
    case 0x101101101:
      // (X) * (X) -> (X)
      // vector .* vector -> vector
      AddProductVec3(a, b, c);
      return;
    case 0x103101202:
      // (x,X) * (X)  -> (X,1)
      // vector * matrix -> vector.unsqueeze(-1)
      AddProductMatVecVec(a, b, c);
      return;
    case 0x203101202:
      // (X,x) * (X) -> (X,1)
      // transposed-matrix * vector -> vector.unsqueeze(-1)
      AddProductTmatVecVec(a, b, c);
      return;
    case 0x202101103:
      // (X,1) * (X) -> (x,X)
      // vector * vector -> matrix (outer product)
      AddProductVec2Mat(a, b, c);
      return;


    default:
      break;

  }

  // If we reached this point, it means we could
  // not handle this request with any of the basic operations above.
  // Something is a little differ


  SubTensor a_temp(a), b_temp(b), c_temp(*c);

  PadAxes(&(a.pattern), &(b.pattern), &(c.pattern));

  CompressPatterns({&a_temp, &b_temp, &c_temp});
}



}  // namespace kaldi
}  // namespace tensor