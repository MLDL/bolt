// Copyright (C) 2019. Huawei Technologies Co., Ltd. All rights reserved.

// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef _REDUCTION_H
#define _REDUCTION_H

#include "operator.hpp"

class Reduction : public Operator {
public:
    Reduction(DataType dt, ReductionParamSpec p)
    {
        this->dt = dt;
        this->p = p;
    }

    std::shared_ptr<Operator> clone() override
    {
        std::shared_ptr<Reduction> mem =
            std::shared_ptr<Reduction>(new Reduction(this->dt, this->p));
        *mem = *this;
        return mem;
    }

    OperatorType get_type() override
    {
        return OT_Reduction;
    }

    void run() override
    {
        Tensor inputTensor = this->inputTensors[0];
        Tensor outputTensor = this->outputTensors[0];
        Tensor maskTensor;
        if (this->inputTensors.size() > 1) {
            maskTensor = this->inputTensors[1];
        } else {
            TensorDesc maskDesc;
            maskDesc.nDims = 0;
            maskTensor.resize(maskDesc);
        }

        CHECK_STATUS(
            reduction(inputTensor, maskTensor, this->p, this->temp, outputTensor, &this->archInfo));
    }

    U32 infer_tmp_memory_size() override
    {
        U32 bytes = 0;
        CHECK_STATUS(reduction_infer_forward_tmp_bytes(
            this->inputTensors[0], this->p, this->outputTensors[0], &bytes, &this->archInfo));
        return bytes;
    }

    EE infer_output_tensors_size(
        std::vector<Tensor *> inTensors, std::vector<Tensor *> outTensors) override
    {
        Tensor maskTensor;
        if (inTensors.size() > 1) {
            maskTensor = *(inTensors[1]);
        } else {
            TensorDesc maskDesc;
            maskDesc.nDims = 0;
            maskTensor.resize(maskDesc);
        }
        return reduction_infer_output_size(inTensors[0], maskTensor, this->p, outTensors[0]);
    }

private:
    ReductionParamSpec p;
};

#endif
