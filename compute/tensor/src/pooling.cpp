// Copyright (C) 2019. Huawei Technologies Co., Ltd. All rights reserved.

// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "tensor_computing.h"
#ifdef _USE_GENERAL
#include "cpu/general/tensor_computing_general.h"
#endif
#ifdef _USE_NEON
#include "cpu/arm/tensor_computing_arm.h"
#endif
#ifdef _USE_MALI
#include "gpu/mali/tensor_computing_mali.h"
#endif
#ifdef _USE_X86
#include "cpu/x86/tensor_computing_x86.h"
#endif

inline EE pooling_infer_output_size_cpu(
    TensorDesc inputDesc, PoolingParamSpec poolingParamSpec, TensorDesc *outputDesc)
{
    if (nullptr == outputDesc) {
        CHECK_STATUS(NULL_POINTER);
    }
    DataType idt;
    DataFormat idf;
    U32 in, ic, ih, iw;
    CHECK_STATUS(tensor4dGet(inputDesc, &idt, &idf, &in, &ic, &ih, &iw));
    U32 strideH = poolingParamSpec.stride_h;
    U32 strideW = poolingParamSpec.stride_w;
    U32 paddingT = poolingParamSpec.padding_top;
    U32 paddingB = poolingParamSpec.padding_bottom;
    U32 paddingL = poolingParamSpec.padding_left;
    U32 paddingR = poolingParamSpec.padding_right;
    U32 kernelSizeH = poolingParamSpec.kernel_h;
    U32 kernelSizeW = poolingParamSpec.kernel_w;
    RoundMode rm = poolingParamSpec.rm;
    U32 oh = 0, ow = 0;
    switch (rm) {
        case CEIL: {
            oh = (U32)(ceil((double(ih + paddingT + paddingB - kernelSizeH) / strideH))) + 1;
            ow = (U32)(ceil((double(iw + paddingL + paddingR - kernelSizeW) / strideW))) + 1;
            break;
        }
        case FLOOR: {
            oh = (U32)(floor((double(ih + paddingT + paddingB - kernelSizeH) / strideH))) + 1;
            ow = (U32)(floor((double(iw + paddingL + paddingR - kernelSizeW) / strideW))) + 1;
            break;
        }
        default: {
            CHECK_STATUS(NOT_SUPPORTED);
        }
    }
    *outputDesc = tensor4df(idt, idf, in, ic, oh, ow);
    return SUCCESS;
}

EE pooling_infer_output_size(
    Tensor *inputTensor, PoolingParamSpec poolingParamSpec, Tensor *outputTensor, ArchInfo_t archInfo)
{
    if (inputTensor == nullptr) {
        CHECK_STATUS(NULL_POINTER);
    }
    if (outputTensor == nullptr) {
        CHECK_STATUS(NULL_POINTER);
    }
    TensorDesc inputDesc = inputTensor->get_desc();
    TensorDesc outputDesc = outputTensor->get_desc();
    EE ret = NOT_SUPPORTED;
    if (0 == poolingParamSpec.kernel_h && 0 == poolingParamSpec.kernel_w) {  // Global pooling
        CHECK_REQUIREMENT(4 == inputDesc.nDims);
        poolingParamSpec.kernel_h = inputDesc.dims[1];
        poolingParamSpec.kernel_w = inputDesc.dims[0];
    }
    if (IS_MALI_GPU(archInfo->arch)) {
#ifdef _USE_MALI
        GCLMemDesc gclmemInputDesc = ocl_get_desc(*inputTensor);
        GCLMemDesc gclmemOutputDesc = ocl_get_desc(*outputTensor);
        ret = pooling_infer_output_size_mali(
            inputDesc, poolingParamSpec, &outputDesc, &gclmemInputDesc, &gclmemOutputDesc);
        ocl_set_desc(inputTensor, gclmemInputDesc);
        ocl_set_desc(outputTensor, gclmemOutputDesc);
#endif
    } else {
        ret = pooling_infer_output_size_cpu(inputDesc, poolingParamSpec, &outputDesc);
    }
    outputTensor->resize(outputDesc);
    return ret;
}

EE pooling(Tensor inputTensor,
    PoolingParamSpec poolingParamSpec,
    Tensor tmpTensor,
    Tensor outputTensor,
    ArchInfo_t archInfo)
{
    auto arch = archInfo->arch;
    TensorDesc inputDesc = inputTensor.get_desc();
    void *input = get_ptr_from_tensor(inputTensor, arch);
    TensorDesc outputDesc = outputTensor.get_desc();
    void *output = get_ptr_from_tensor(outputTensor, arch);
    F32 scale[2] = {inputTensor.get_scale(), -1};
    void *tmp = get_ptr_from_tensor(tmpTensor, arch);

    EE ret = NOT_SUPPORTED;
    if (0 == poolingParamSpec.kernel_h && 0 == poolingParamSpec.kernel_w) {  // Global pooling
        CHECK_REQUIREMENT(4 == inputDesc.nDims);
        poolingParamSpec.kernel_h = inputDesc.dims[1];
        poolingParamSpec.kernel_w = inputDesc.dims[0];
    }
    TensorDesc inDescCPU = inputDesc;
    U8 *inputCPU = (U8 *)input;
    TensorDesc outDescCPU = outputDesc;
    U8 *outputCPU = (U8 *)output;
    if (DF_NCHWC8 != inputDesc.df && !IS_MALI_GPU(arch)) {
        U32 paddedC = (inputDesc.dims[2] + 7) / 8 * 8;
        inDescCPU.dims[2] = paddedC;
        inDescCPU.df = DF_NCHWC8;
        outDescCPU.dims[2] = paddedC;
        outDescCPU.df = DF_NCHWC8;
        inputCPU = (U8 *)tmp;
        outputCPU = inputCPU + tensorNumBytes(inDescCPU);
        transformNCHWToNCHWC8(inputDesc, input, inDescCPU, inputCPU);
    }
    if (IS_GENERAL(arch)) {
#ifdef _USE_GENERAL
        ret = pooling_general(inDescCPU, inputCPU, poolingParamSpec, outDescCPU, outputCPU);
#endif
#ifdef _USE_X86
    } else if (IS_X86_AVX2(arch)) {
        ret = pooling_x86(inDescCPU, inputCPU, poolingParamSpec, scale, outDescCPU, outputCPU);
#endif
#ifdef _USE_NEON
    } else if (IS_ARM(arch)) {
        ret = pooling_arm(inDescCPU, inputCPU, poolingParamSpec, scale, outDescCPU, outputCPU);
#endif
#ifdef _USE_MALI
    } else if (IS_MALI_GPU(arch)) {
        ret = pooling_mali(((MaliPara_t)(archInfo->archPara))->handle, inputDesc,
            (const GCLMem_t)input, poolingParamSpec, scale, (GCLMem_t)tmp, outputDesc,
            (GCLMem_t)output);
#endif
    }
    if (DF_NCHWC8 != outputDesc.df && !IS_MALI_GPU(arch)) {
        transformToNCHW(outDescCPU, outputCPU, outputDesc, output);
    }
    outputTensor.set_scale(scale[1]);
    return ret;
}

EE pooling_infer_forward_tmp_bytes(
    Tensor inputTensor, Tensor outputTensor, U32 *bytes, ArchInfo_t archInfo)
{
    if (bytes == nullptr) {
        CHECK_STATUS(NULL_POINTER);
    }
    TensorDesc inputDesc = inputTensor.get_desc();
    EE ret = NOT_SUPPORTED;
    if (IS_MALI_GPU(archInfo->arch)) {
#ifdef _USE_MALI
        ret = pooling_infer_forward_tmp_bytes_mali(
            inputDesc, bytes, ((MaliPara_t)(archInfo->archPara))->forwardRunInfo);
#endif
    } else {
        *bytes = 0;
        if (DF_NCHW == inputDesc.df) {
            U32 paddedC = (inputDesc.dims[2] + 7) / 8 * 8;
            TensorDesc outputDesc = outputTensor.get_desc();
            inputDesc.dims[2] = paddedC;
            outputDesc.dims[2] = paddedC;
            *bytes = tensorNumBytes(inputDesc) + tensorNumBytes(outputDesc);
        }
        ret = SUCCESS;
    }
    return ret;
}
