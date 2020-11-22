// Copyright (C) 2019. Huawei Technologies Co., Ltd. All rights reserved.

// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "sys.h"
#include "types.h"
#include "tensor_desc.h"
#include "error.h"
#include "gpu/mali/tensor_computing_mali.h"
#include "gpu/mali/fp16/fully_connected_mali_fp16.h"
inline void fully_connected_produce_algos_paras(TensorDesc inputDesc,
    TensorDesc filterDesc,
    std::vector<TensorDesc> outputDescs,
    std::vector<ConvolutionForwardAlgorithm> *fcAlgorithms,
    std::vector<U32> *algoNumIndex,
    std::vector<U32> *vecW,
    std::vector<U32> *vecC,
    std::vector<U32> *vecK)
{
    DataType dt;
    U32 iw, ih, ic, fw, fh, fn;
    tensorSelectGet(filterDesc, &dt, NULL, &fn, NULL, &fh, &fw);
    tensorSelectGet(inputDesc, NULL, NULL, NULL, &ic, &ih, &iw);
    U32 configInfo[3][128];
    U32 configNums[2];
    ConvolutionForwardAlgorithm algo[2];
    U32 algoNum = 1;
    algo[0] = CONVOLUTION_ALGORITHM_DIRECT;
    if (inputDesc.df == DF_NCHW || inputDesc.df == DF_NORMAL) {
        if (ih != 1 || iw != 1 || fh != 1 || fw != 1) {
            U32 item_w = (64 + ih - 1) / ih;
            item_w = (item_w > iw) ? iw : item_w;
            configInfo[0][0] = item_w;
            configInfo[1][0] = 4;
            configInfo[2][0] = 4;
            configNums[0] = 1;
        } else {
            U32 configNum = 0;
            U32 j = 8;
            for (U32 i = 0; i < 3; i++) {
                configInfo[0][configNum] = 1;
                configInfo[1][configNum] = 1 << (2 + i);
                configInfo[2][configNum] = 0;
                configNum++;
                if (ic % j != 0) {
                    break;
                }
                j = j << 1;
            }
            configNums[0] = configNum;
        }
    } else if (inputDesc.df == DF_MKT) {
        U32 configNum = 0;
        U32 align8 = true;
        U32 nj = 8;
        U32 k = 4;
        for (U32 i = 0; i < outputDescs.size(); i++) {
            if (outputDescs[i].dims[1] % 8 != 0) {
                align8 = false;
            }
        }
        for (U32 i = 0; i < 2; i++) {
            for (U32 j = 0; j < nj; j++) {
                configInfo[0][configNum] = j + 1;
                configInfo[1][configNum] = 4;
                configInfo[2][configNum] = k;
                configNum++;
            }
            if (!align8) {
                break;
            }
            nj = 4;
            k = 8;
        }
        configNums[0] = configNum;
    } else {
        CHECK_STATUS(NOT_SUPPORTED);
    }

    for (U32 i = 0; i < algoNum; i++) {
        (*fcAlgorithms).push_back(algo[i]);
        (*algoNumIndex).push_back(configNums[i]);
        U32 be = (i == 0) ? 0 : configNums[i - 1];
        U32 end = configNums[i];
        for (U32 j = be; j < end; j++) {
            if (vecW) {
                (*vecW).push_back(configInfo[0][j]);
            }
            if (vecC) {
                (*vecC).push_back(configInfo[1][j]);
            }
            if (vecK) {
                (*vecK).push_back(configInfo[2][j]);
            }
        }
    }
}
inline EE fully_connected_checkpara_mali(GCLHandle_t handle,
    TensorDesc inputDesc,
    const GCLMem_t input,
    TensorDesc filterDesc,
    std::vector<GCLMem_t> *filter,
    std::vector<GCLMem_t> *bias,
    TensorDesc outputDesc,
    std::vector<GCLMem_t> *output)
{
    if (nullptr == handle || nullptr == input || nullptr == filter || nullptr == output ||
        nullptr == bias) {
        return NULL_POINTER;
    }
    if (filter->size() != output->size() || filter->size() != bias->size() || bias->size() == 0) {
        return NOT_MATCH;
    }
    for (U32 i = 0; i < filter->size(); ++i) {
        if (nullptr == (*filter)[i] || nullptr == (*output)[i] || nullptr == (*bias)[i]) {
            return NULL_POINTER;
        }
    }
    if (inputDesc.df == DF_NCHW || inputDesc.df == DF_NORMAL) {
        U32 in, ic, ih, iw;
        U32 fn, fc, fh, fw;
        U32 oc;
        CHECK_STATUS(tensorSelectGet(inputDesc, NULL, NULL, &in, &ic, &ih, &iw));
        CHECK_STATUS(tensorSelectGet(filterDesc, NULL, NULL, &fn, &fc, &fh, &fw));
        CHECK_STATUS(tensorSelectGet(outputDesc, NULL, NULL, NULL, &oc, NULL, NULL));
        if (filterDesc.df != DF_NCHW) {
            return NOT_SUPPORTED;
        }
        if (input->desc.memFormat != DF_NCWHC4) {
            return NOT_SUPPORTED;
        }
        if ((*filter)[0]->desc.memFormat != DF_NCWHN4C4) {
            return NOT_SUPPORTED;
        }
        if ((*output)[0]->desc.memFormat != DF_NCWHC4) {
            return NOT_SUPPORTED;
        }
        if (in > 1) {
            return NOT_SUPPORTED;
        }
        if (filter->size() > 1) {
            return NOT_SUPPORTED;
        }
        if (fw != iw) {
            return NOT_MATCH;
        }
        if (fh != ih) {
            return NOT_MATCH;
        }
        if (fc != ic) {
            return NOT_MATCH;
        }
        if (fn != oc) {
            return NOT_MATCH;
        }
    }
    if (inputDesc.df == DF_MKT) {
        U32 k;
        U32 fw, fh, fc, fn;
        k = inputDesc.dims[1];
        CHECK_STATUS(tensorSelectGet(filterDesc, NULL, NULL, &fn, &fc, &fh, &fw));
        if (fh != 1 || fw != 1) {
            return NOT_MATCH;
        }
        if (k != fc) {
            return NOT_MATCH;
        }
    }
    return SUCCESS;
}
EE fully_connected_infer_output_size_mali(TensorDesc inputDesc,
    TensorDesc filterDesc,
    TensorDesc *outputDesc,
    GCLMemDesc_t gclmemInputDesc,
    GCLMemDesc_t gclmemOutputDesc)
{
    U32 fn;
    tensorSelectGet(filterDesc, NULL, NULL, &fn, NULL, NULL, NULL);
    if (inputDesc.df == DF_NCHW || inputDesc.df == DF_NORMAL) {
        DataType idt;
        DataFormat idf;
        U32 iw, ih, ic, in;
        tensorSelectGet(inputDesc, &idt, &idf, &in, &ic, &ih, &iw);
        if (outputDesc) {
            *outputDesc = tensor4df(idt, idf, in, fn, 1, 1);
        }
        CHECK_STATUS(infer_gclmem_desc_ncwhc4(
            iw, ih, ic, 0, 0, 1, 1, fn, idt, idt, gclmemInputDesc, gclmemOutputDesc));
        return SUCCESS;
    } else if (inputDesc.df == DF_MKT) {
        bool need_pad = false;
        DataType dt;
        U32 m, k, t;
        get_nlp_mkt_val(inputDesc, &dt, &m, &k, &t);
        if (outputDesc) {
            *outputDesc = inputDesc;
            (*outputDesc).dims[1] = fn;
        }
        std::vector<ConvolutionForwardAlgorithm> fcAlgorithms;
        std::vector<U32> algoNumIndex;
        std::vector<U32> vecW;
        std::vector<TensorDesc> outputDescs;
        outputDescs.push_back(*outputDesc);
        fully_connected_produce_algos_paras(
            inputDesc, filterDesc, outputDescs, &fcAlgorithms, &algoNumIndex, &vecW, NULL, NULL);
        U32 igw, igh, igc;
        U32 ogw, ogh, ogc;
        U32 t_align = t;
        for (U32 i = 0; i < algoNumIndex[0]; i++) {
            U32 j = ALIGN(t, vecW[i]);
            t_align = (t_align < j) ? j : t_align;
        }
        if (t_align != t) {
            need_pad = true;
        }
        map_nlp_mkt_to_ncwhc4(m, k, t_align, &igw, &igh, &igc);
        map_nlp_mkt_to_ncwhc4(m, fn, t, &ogw, &ogh, &ogc);
        igc = igc * 4;
        ogc = ogc * 4;
        CHECK_STATUS(infer_gclmem_desc_ncwhc4(igw, igh, igc, 0, 0, ogw, ogh, ogc, dt, dt,
            gclmemInputDesc, gclmemOutputDesc, need_pad));
        return SUCCESS;
    }
    CHECK_STATUS(NOT_SUPPORTED);
    return NOT_SUPPORTED;
}

EE fully_connected_infer_forward_algorithm_mali(GCLHandle_t handle,
    TensorDesc inputDesc,
    TensorDesc filterDesc,
    std::vector<TensorDesc> outputDescs,
    ForwardRunInfoMali_t forwardRunInfo)
{
    if (forwardRunInfo == nullptr) {
        CHECK_STATUS(NULL_POINTER);
    }
    ConvolutionForwardAlgorithm algorithm = (ConvolutionForwardAlgorithm)(forwardRunInfo->algorithm);
    if (algorithm != CONVOLUTION_ALGORITHM_NULL) {
        return SUCCESS;
    }
    DataType dt;
    U32 fn;
    tensorSelectGet(filterDesc, &dt, NULL, &fn, NULL, NULL, NULL);
    std::vector<ConvolutionForwardAlgorithm> fcAlgorithms;
    std::vector<U32> algoNumIndex;
    std::vector<U32> vecW;
    std::vector<U32> vecC;
    std::vector<U32> vecK;
    fully_connected_produce_algos_paras(
        inputDesc, filterDesc, outputDescs, &fcAlgorithms, &algoNumIndex, &vecW, &vecC, &vecK);
    if (vecW.size() == 1) {
        forwardRunInfo->best_w[0] = vecW[0];
        forwardRunInfo->best_k[0] = vecK[0];
        forwardRunInfo->best_c[0] = vecC[0];
        forwardRunInfo->algorithm = fcAlgorithms[0];
        return SUCCESS;
    }

    CHECK_STATUS(gcl_clean_kernelVec(handle));
    CHECK_STATUS(gcl_enable_queue_profiling(handle));
    U32 sliceNum = outputDescs.size();
    GCLMem_t input = gcl_create_gclmem();
    GCLMem_t tmpbuf = gcl_create_gclmem();
    std::vector<GCLMem_t> filter;
    std::vector<GCLMem_t> bias;
    std::vector<GCLMem_t> output;
    for (U32 i = 0; i < sliceNum; ++i) {
        GCLMem_t filterTmp = gcl_create_gclmem();
        GCLMem_t biasTmp = gcl_create_gclmem();
        GCLMem_t outTmp = gcl_create_gclmem();
        filter.push_back(filterTmp);
        bias.push_back(biasTmp);
        output.push_back(outTmp);
    }

    std::vector<ForwardRunInfoMali> runInfos;
    U32 stride[3] = {0, 0, 0};
    U32 offset[3] = {0, 0, 0};
    GCLMemDesc inputMemDesc = gcl_mem_desc(stride, offset, DT_U8, DF_NCWHC4);
    GCLMemDesc outputMemDesc = gcl_mem_desc(stride, offset, DT_U8, DF_NCWHC4);
    CHECK_STATUS(fully_connected_infer_output_size_mali(
        inputDesc, filterDesc, NULL, &inputMemDesc, &outputMemDesc));
    std::vector<GCLMemDesc> filterMemDescs;
    U32 maxBytes = 0;
    U32 maxFilterSize = 0;
    for (U32 i = 0; i < algoNumIndex.size(); i++) {
        U32 bytes = 0;
        ForwardRunInfoMali runInfo;
        runInfo.algorithm = fcAlgorithms[i];
        U32 be = (i == 0) ? 0 : algoNumIndex[i - 1];
        U32 end = algoNumIndex[i];
        for (U32 j = be; j < end; j++) {
            GCLMemDesc filterMemDesc = gcl_mem_desc(stride, offset, DT_U8, DF_NCWHC4);
            runInfo.best_w[0] = vecW[j];
            runInfo.best_c[0] = vecC[j];
            runInfo.best_k[0] = vecK[j];
            if (fully_connected_transform_filter_bytes_mali(
                    filterDesc, &filterMemDesc, &bytes, &runInfo) != SUCCESS) {
                continue;
            }
            maxBytes = (maxBytes < bytes) ? bytes : maxBytes;
            if (fully_connected_infer_forward_tmp_bytes_mali(
                    inputDesc, filterDesc, &bytes, &runInfo) != SUCCESS) {
                continue;
            }
            maxBytes = (maxBytes < bytes) ? bytes : maxBytes;
            maxFilterSize = (maxFilterSize < filterMemDesc.byteSize) ? filterMemDesc.byteSize
                                                                     : maxFilterSize;
            filterMemDescs.push_back(filterMemDesc);
            runInfos.push_back(runInfo);
        }
    }

    MemFlags flags = CL_MEM_READ_WRITE;
    if (inputDesc.df == DF_MKT) {
        U32 stride[3] = {(fn + 3) / 4, 1, 1};
        U32 offset[3] = {0, 0, 0};
        CHECK_STATUS(gclmem_set_desc_padding(
            &bias[0]->desc, stride, offset, dt, DF_NHWC, GCL_MEM_IMG_1D, flags));
    } else {
        U32 stride[3] = {fn, 1, 1};
        U32 offset[3] = {0, 0, 0};
        CHECK_STATUS(gclmem_set_desc_padding(
            &bias[0]->desc, stride, offset, dt, DF_NHWC, GCL_MEM_BUF, flags));
    }

    U32 algosNum = runInfos.size();
    if (algosNum == 0) {
        CHECK_STATUS(NOT_SUPPORTED);
    }
    TensorDesc biasDesc = tensor1d(dt, fn);
    filterMemDescs[0].byteSize = maxFilterSize;
    input->desc = inputMemDesc;
    output[0]->desc = outputMemDesc;
    filter[0]->desc = filterMemDescs[0];
    tmpbuf->desc.byteSize = maxBytes;
    gcl_create_memory(handle, input);
    for (U32 i = 0; i < sliceNum; ++i) {
        filter[i]->desc = filter[0]->desc;
        bias[i]->desc = bias[0]->desc;
        output[i]->desc = output[0]->desc;
        gcl_create_memory(handle, filter[i]);
        gcl_create_memory(handle, bias[i]);
        gcl_create_memory(handle, output[i]);
    }
    if (maxBytes) {
        gcl_create_memory(handle, tmpbuf);
    }

    U32 runKernelBe = 0;
    U32 runKernelEnd = 0;
    double minTime = DBL_MAX;
    ForwardRunInfoMali bestRunInfo;
    for (U32 i = 0; i < algosNum; i++) {
        filter[0]->desc = filterMemDescs[i];
        if (sliceNum > 1) {
            U32 item_k = runInfos[i].best_k[0];
            for (U32 j = 0; j < sliceNum; j++) {
                U32 fn = outputDescs[j].dims[1];
                output[j]->desc.stride[2] = (fn + 3) / 4;
                filter[j]->desc.stride[2] = (fn + item_k - 1) / item_k;
                bias[j]->desc.stride[0] = (inputDesc.df == DF_MKT) ? (fn + 3) / 4 : fn;
            }
        }
        if (fully_connected_mali(handle, inputDesc, input, filterDesc, &filter, biasDesc, &bias,
                maxBytes, tmpbuf, outputDescs[0], &output, &runInfos[i]) == SUCCESS) {
            runKernelEnd = handle->kernelVec->size();
            gcl_run_kernelVec_timing(handle, runKernelBe, runKernelEnd);
            runKernelBe = runKernelEnd;
            if (minTime > handle->t_execute) {
                minTime = handle->t_execute;
                bestRunInfo = runInfos[i];
            }
        }
    }
    if (minTime == DBL_MAX) {
        CHECK_STATUS(NOT_SUPPORTED);
    }
    *forwardRunInfo = bestRunInfo;
    CHECK_STATUS(gcl_finish(handle));
    gcl_destroy_gclmem(input);
    gcl_destroy_gclmem(tmpbuf);
    for (auto p : filter) {
        gcl_destroy_gclmem(p);
    }
    for (auto p : output) {
        gcl_destroy_gclmem(p);
    }
    for (auto p : bias) {
        gcl_destroy_gclmem(p);
    }
    runInfos.clear();
    filterMemDescs.clear();
    CHECK_STATUS(gcl_clean_kernelVec(handle));
    CHECK_STATUS(gcl_off_queue_profiling(handle));
    return SUCCESS;
}
EE fully_connected_transform_filter_bytes_mali(TensorDesc filterDesc,
    GCLMemDesc_t gclmemFilterDesc,
    U32 *bytes,
    ForwardRunInfoMali_t forwardRunInfo)
{
    EE ret = SUCCESS;
    switch (filterDesc.dt) {
        case DT_F16: {
            ret = fully_connected_transform_filter_bytes_mali_fp16(
                filterDesc, gclmemFilterDesc, bytes, forwardRunInfo);
            break;
        }
        case DT_I8: {
            ret = NOT_SUPPORTED;
            break;
        }
        default:
            ret = NOT_SUPPORTED;
            break;
    }
    return ret;
}

EE fully_connected_transform_filter_mali(GCLHandle_t handle,
    TensorDesc filterDesc,
    GCLMem_t filter,
    TensorDesc *fltmemDesc,
    std::vector<GCLMem_t> fltmem,
    ForwardRunInfoMali_t forwardRunInfo)
{
    EE ret = SUCCESS;
    switch (filterDesc.dt) {
        case DT_F16: {
            ret = fully_connected_transform_filter_mali_fp16(
                handle, filterDesc, filter, fltmemDesc, fltmem, forwardRunInfo);
            break;
        }
        case DT_I8: {
            ret = NOT_SUPPORTED;
            break;
        }
        default:
            ret = NOT_SUPPORTED;
            break;
    }
    return ret;
}

EE fully_connected_infer_forward_tmp_bytes_mali(
    TensorDesc inputDesc, TensorDesc filterDesc, U32 *bytes, ForwardRunInfoMali_t forwardRunInfo)
{
    EE ret = SUCCESS;
    switch (inputDesc.dt) {
        case DT_F16: {
            ret = fully_connected_infer_forward_tmp_bytes_mali_fp16(
                inputDesc, filterDesc, bytes, forwardRunInfo);
            break;
        }
        case DT_I8: {
            ret = NOT_SUPPORTED;
            break;
        }
        default:
            ret = NOT_SUPPORTED;
            break;
    }
    return ret;
}

EE fully_connected_mali(GCLHandle_t handle,
    TensorDesc inputDesc,
    const GCLMem_t input,
    TensorDesc filterDesc,
    std::vector<GCLMem_t> *filter,
    TensorDesc biasDesc,
    std::vector<GCLMem_t> *bias,
    U32 tmpBytes,
    GCLMem_t tmpBuf,
    TensorDesc outputDesc,
    std::vector<GCLMem_t> *output,
    ForwardRunInfoMali_t forwardRunInfo)
{
    EE ret = SUCCESS;
    ret = fully_connected_checkpara_mali(
        handle, inputDesc, input, filterDesc, filter, bias, outputDesc, output);
    switch (inputDesc.dt) {
        case DT_F16: {
            ret = fully_connected_mali_fp16(handle, inputDesc, input, filterDesc, *filter, biasDesc,
                *bias, tmpBytes, tmpBuf, outputDesc, *output, forwardRunInfo);
            break;
        }
        case DT_I8: {
            ret = NOT_SUPPORTED;
            break;
        }
        default:
            ret = NOT_SUPPORTED;
            break;
    }
    return ret;
}
