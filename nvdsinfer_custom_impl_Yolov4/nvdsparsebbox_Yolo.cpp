/*
 * Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include "nvdsinfer_custom_impl.h"
#include "trt_utils.h"

static const int NUM_CLASSES_YOLO = 80;

extern "C" bool NvDsInferParseCustomYoloV4(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList);

extern "C" bool NvDsInferParseCustomYoloV4Tiny(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList);

extern "C" bool NvDsInferParseCustomYoloV3(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList);

extern "C" bool NvDsInferParseCustomYoloV3Tiny(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList);

extern "C" bool NvDsInferParseCustomYoloV2(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList);

extern "C" bool NvDsInferParseCustomYoloV2Tiny(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList);

extern "C" bool NvDsInferParseCustomYoloTLT(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList);

/* This is a sample bounding box parsing function for the sample YoloV3 detector model */
static NvDsInferParseObjectInfo convertBBox(const float& bx, const float& by, const float& bw,
                                     const float& bh, const int& strideH, const int& strideW, const uint& netW,
                                     const uint& netH)
{
    NvDsInferParseObjectInfo b;
    // Restore coordinates to network input resolution
    float xCenter = bx * strideW;
    float yCenter = by * strideH;
    float x0 = xCenter - bw / 2;
    float y0 = yCenter - bh / 2;
    float x1 = x0 + bw;
    float y1 = y0 + bh;

    x0 = clamp(x0, 0, netW);
    y0 = clamp(y0, 0, netH);
    x1 = clamp(x1, 0, netW);
    y1 = clamp(y1, 0, netH);

    b.left = x0;
    b.width = clamp(x1 - x0, 0, netW);
    b.top = y0;
    b.height = clamp(y1 - y0, 0, netH);

    return b;
}

static std::vector<NvDsInferParseObjectInfo>
nonMaximumSuppression(const float nmsThresh, std::vector<NvDsInferParseObjectInfo> binfo)
{
    auto overlap1D = [](float x1min, float x1max, float x2min, float x2max) -> float {
        if (x1min > x2min)
        {
            std::swap(x1min, x2min);
            std::swap(x1max, x2max);
        }
        return x1max < x2min ? 0 : std::min(x1max, x2max) - x2min;
    };
    auto computeIoU
        = [&overlap1D](NvDsInferParseObjectInfo& bbox1, NvDsInferParseObjectInfo& bbox2) -> float {
        float overlapX
            = overlap1D(bbox1.left, bbox1.left + bbox1.width, bbox2.left, bbox2.left + bbox2.width);
        float overlapY
            = overlap1D(bbox1.top, bbox1.top + bbox1.height, bbox2.top, bbox2.top + bbox2.height);
        float area1 = (bbox1.width) * (bbox1.height);
        float area2 = (bbox2.width) * (bbox2.height);
        float overlap2D = overlapX * overlapY;
        float u = area1 + area2 - overlap2D;
        return u == 0 ? 0 : overlap2D / u;
    };

    std::stable_sort(binfo.begin(), binfo.end(),
                     [](const NvDsInferParseObjectInfo& b1, const NvDsInferParseObjectInfo& b2) {
                         return b1.detectionConfidence > b2.detectionConfidence;
                     });
    std::vector<NvDsInferParseObjectInfo> out;
    for (auto i : binfo)
    {
        bool keep = true;
        for (auto j : out)
        {
            if (keep)
            {
                float overlap = computeIoU(i, j);
                keep = overlap <= nmsThresh;
            }
            else
                break;
        }
        if (keep) out.push_back(i);
    }
    return out;
}

static std::vector<NvDsInferParseObjectInfo> nms(
    const float nmsThresh,
    std::vector<NvDsInferParseObjectInfo>& binfo,
    const uint numClasses)
{
    std::vector<NvDsInferParseObjectInfo> result;
    std::vector<std::vector<NvDsInferParseObjectInfo>> splitBoxes(numClasses);
    for (auto& box : binfo)
    {
        splitBoxes.at(box.classId).push_back(box);
    }

    for (auto& boxes : splitBoxes)
    {
        boxes = nonMaximumSuppression(nmsThresh, boxes);
        result.insert(result.end(), boxes.begin(), boxes.end());
    }
    return result;
}

static void addBBoxProposal(const float bx, const float by, const float bw, const float bh,
                     const uint strideH, const uint strideW, const uint& netW, const uint& netH, const int maxIndex,
                     const float maxProb, std::vector<NvDsInferParseObjectInfo>& binfo)
{
    NvDsInferParseObjectInfo bbi = convertBBox(bx, by, bw, bh, strideH, strideW, netW, netH);
    if (bbi.width < 1 || bbi.height < 1) return;

    bbi.detectionConfidence = maxProb;
    bbi.classId = maxIndex;
    binfo.push_back(bbi);
}

static std::vector<NvDsInferParseObjectInfo>
decodeYoloV2Tensor(
    const float* detections, const std::vector<float> &anchors,
    const uint gridSizeW, const uint gridSizeH, const uint strideH, const uint strideW, const uint numBBoxes,
    const uint numOutputClasses, const uint& netW,
    const uint& netH)
{
    std::vector<NvDsInferParseObjectInfo> binfo;
    for (uint y = 0; y < gridSizeH; ++y) {
        for (uint x = 0; x < gridSizeW; ++x) {
            for (uint b = 0; b < numBBoxes; ++b)
            {
                const float pw = anchors[b * 2];
                const float ph = anchors[b * 2 + 1];

                const int numGridCells = gridSizeH * gridSizeW;
                const int bbindex = y * gridSizeW + x;
                const float bx
                    = x + detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 0)];
                const float by
                    = y + detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 1)];
                const float bw
                    = pw * exp (detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 2)]);
                const float bh
                    = ph * exp (detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 3)]);

                const float objectness
                    = detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 4)];

                float maxProb = 0.0f;
                int maxIndex = -1;

                for (uint i = 0; i < numOutputClasses; ++i)
                {
                    float prob
                        = (detections[bbindex
                                      + numGridCells * (b * (5 + numOutputClasses) + (5 + i))]);

                    if (prob > maxProb)
                    {
                        maxProb = prob;
                        maxIndex = i;
                    }
                }
                maxProb = objectness * maxProb;

                addBBoxProposal(bx, by, bw, bh, strideH, strideW, netW, netH, maxIndex, maxProb, binfo);
            }
        }
    }
    return binfo;
}

static std::vector<NvDsInferParseObjectInfo>
decodeYoloV3Tensor(
    const float* detections, const std::vector<int> &mask, const std::vector<float> &anchors,
    const uint gridSizeW, const uint gridSizeH, const uint strideH, const uint strideW, const uint numBBoxes,
    const uint numOutputClasses, const uint& netW,
    const uint& netH)
{
    std::vector<NvDsInferParseObjectInfo> binfo;
    for (uint y = 0; y < gridSizeH; ++y) {
        for (uint x = 0; x < gridSizeW; ++x) {
            for (uint b = 0; b < numBBoxes; ++b)
            {
                const float pw = anchors[mask[b] * 2];
                const float ph = anchors[mask[b] * 2 + 1];

                const int numGridCells = gridSizeH * gridSizeW;
                const int bbindex = y * gridSizeW + x;
                const float bx
                    = x + detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 0)];
                const float by
                    = y + detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 1)];
                const float bw
                    = pw * detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 2)];
                const float bh
                    = ph * detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 3)];

                const float objectness
                    = detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 4)];

                float maxProb = 0.0f;
                int maxIndex = -1;

                for (uint i = 0; i < numOutputClasses; ++i)
                {
                    float prob
                        = (detections[bbindex
                                      + numGridCells * (b * (5 + numOutputClasses) + (5 + i))]);

                    if (prob > maxProb)
                    {
                        maxProb = prob;
                        maxIndex = i;
                    }
                }
                maxProb = objectness * maxProb;

                addBBoxProposal(bx, by, bw, bh, strideH, strideW, netW, netH, maxIndex, maxProb, binfo);
            }
        }
    }
    return binfo;
}

static std::vector<NvDsInferParseObjectInfo>
decodeYoloV4Tensor(
    const float* detections, const std::vector<int> &mask, const std::vector<float> &anchors,
    const uint gridSizeW, const uint gridSizeH, const uint strideH, const uint strideW, const uint numBBoxes,
    const uint numOutputClasses, const uint& netW,
    const uint& netH, const float confThresh)
{
    std::vector<NvDsInferParseObjectInfo> binfo;
    for (uint y = 0; y < gridSizeH; ++y) {
        for (uint x = 0; x < gridSizeW; ++x) {
            for (uint b = 0; b < numBBoxes; ++b)
            {
                const float pw = anchors[mask[b] * 2];
                const float ph = anchors[mask[b] * 2 + 1];

                const int numGridCells = gridSizeH * gridSizeW;
                const int bbindex = y * gridSizeW + x;
                const float bx
                    = x + detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 0)];
                const float by
                    = y + detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 1)];
                const float bw
                    = pw * detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 2)];
                const float bh
                    = ph * detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 3)];

                const float objectness
                    = detections[bbindex + numGridCells * (b * (5 + numOutputClasses) + 4)];

                float maxProb = 0.0f;
                int maxIndex = -1;

                for (uint i = 0; i < numOutputClasses; ++i)
                {
                    float prob
                        = (detections[bbindex
                                      + numGridCells * (b * (5 + numOutputClasses) + (5 + i))]);

                    if (prob > maxProb)
                    {
                        maxProb = prob;
                        maxIndex = i;
                    }
                }
                maxProb = objectness * maxProb;

                if (maxProb > confThresh) {
                    addBBoxProposal(bx, by, bw, bh, strideH, strideW, netW, netH, maxIndex, maxProb, binfo);
                }
            }
        }
    }
    return binfo;
}

static inline std::vector<const NvDsInferLayerInfo*>
SortLayers(const std::vector<NvDsInferLayerInfo> & outputLayersInfo)
{
    std::vector<const NvDsInferLayerInfo*> outLayers;
    for (auto const &layer : outputLayersInfo) {
        outLayers.push_back (&layer);
    }
    std::sort(outLayers.begin(), outLayers.end(),
        [](const NvDsInferLayerInfo* a, const NvDsInferLayerInfo* b) {
            return a->inferDims.d[1] < b->inferDims.d[1];
        });
    return outLayers;
}

static bool NvDsInferParseYoloV4(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList,
    const std::vector<float> &anchors,
    const std::vector<std::vector<int>> &masks,
    float beta_nms)
{
    const uint kNUM_BBOXES = 3;
    const float confThresh = detectionParams.perClassThreshold[0];
    // const float confThresh = 0.45;

    const std::vector<const NvDsInferLayerInfo*> sortedLayers =
        SortLayers (outputLayersInfo);

    if (sortedLayers.size() != masks.size()) {
        std::cerr << "ERROR: yoloV4 output layer.size: " << sortedLayers.size()
                  << " does not match mask.size: " << masks.size() << std::endl;
        return false;
    }

    if (NUM_CLASSES_YOLO != detectionParams.numClassesConfigured)
    {
        std::cerr << "WARNING: Num classes mismatch. Configured:"
                  << detectionParams.numClassesConfigured
                  << ", detected by network: " << NUM_CLASSES_YOLO << std::endl;
    }

    std::vector<NvDsInferParseObjectInfo> objects;

    for (uint idx = 0; idx < masks.size(); ++idx) {
        const NvDsInferLayerInfo &layer = *sortedLayers[idx]; // 255 x Grid x Grid

        assert(layer.inferDims.numDims == 3);
        const uint gridSizeH = layer.inferDims.d[1];
        const uint gridSizeW = layer.inferDims.d[2];
        const uint strideH = DIVUP(networkInfo.height, gridSizeH);
        const uint strideW = DIVUP(networkInfo.width, gridSizeW);
        // assert(stride == DIVUP(networkInfo.height, gridSizeH));

        std::vector<NvDsInferParseObjectInfo> outObjs =
            decodeYoloV4Tensor((const float*)(layer.buffer), masks[idx], anchors, 
                       gridSizeW, gridSizeH, strideH, strideW, kNUM_BBOXES,
                       NUM_CLASSES_YOLO, networkInfo.width, networkInfo.height, confThresh);
        objects.insert(objects.end(), outObjs.begin(), outObjs.end());
    }

    objectList = objects;
    // objectList.clear();
    // objectList = nms(beta_nms, objects, NUM_CLASSES_YOLO);

    return true;
}

static bool NvDsInferParseYoloV3(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList,
    const std::vector<float> &anchors,
    const std::vector<std::vector<int>> &masks)
{
    const uint kNUM_BBOXES = 3;

    const std::vector<const NvDsInferLayerInfo*> sortedLayers =
        SortLayers (outputLayersInfo);

    if (sortedLayers.size() != masks.size()) {
        std::cerr << "ERROR: yoloV3 output layer.size: " << sortedLayers.size()
                  << " does not match mask.size: " << masks.size() << std::endl;
        return false;
    }

    if (NUM_CLASSES_YOLO != detectionParams.numClassesConfigured)
    {
        std::cerr << "WARNING: Num classes mismatch. Configured:"
                  << detectionParams.numClassesConfigured
                  << ", detected by network: " << NUM_CLASSES_YOLO << std::endl;
    }

    std::vector<NvDsInferParseObjectInfo> objects;

    for (uint idx = 0; idx < masks.size(); ++idx) {
        const NvDsInferLayerInfo &layer = *sortedLayers[idx]; // 255 x Grid x Grid

        assert(layer.inferDims.numDims == 3);
        const uint gridSizeH = layer.inferDims.d[1];
        const uint gridSizeW = layer.inferDims.d[2];
        const uint stride = DIVUP(networkInfo.width, gridSizeW);
        assert(stride == DIVUP(networkInfo.height, gridSizeH));

        std::vector<NvDsInferParseObjectInfo> outObjs =
            decodeYoloV3Tensor((const float*)(layer.buffer), masks[idx], anchors, gridSizeW, gridSizeH, stride, stride, kNUM_BBOXES,
                       NUM_CLASSES_YOLO, networkInfo.width, networkInfo.height);
        objects.insert(objects.end(), outObjs.begin(), outObjs.end());
    }

    objectList = objects;

    return true;
}


/* C-linkage to prevent name-mangling */
extern "C" bool NvDsInferParseCustomYoloV4(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList)
{
    static const std::vector<float> kANCHORS = {
        12.0, 16.0, 19.0, 36.0, 40.0, 28.0, 36.0, 75.0, 76.0,
        55.0, 72.0, 146.0, 142.0, 110.0, 192.0, 243.0, 459.0, 401.0};
    static const std::vector<std::vector<int>> kMASKS = {
        // {0, 1, 2},
        // {3, 4, 5},
        // {6, 7, 8}};
        {6, 7, 8},
        {3, 4, 5},
        {0, 1, 2}}; 
    static const float beta_nms = 0.2;

    // It's a hassle
    // const float confThresh = 0.6;
    return NvDsInferParseYoloV4 (
        outputLayersInfo, networkInfo, detectionParams, objectList,
        kANCHORS, kMASKS, beta_nms);
}

/* C-linkage to prevent name-mangling */
extern "C" bool NvDsInferParseCustomYoloV4Tiny(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList)
{
    static const std::vector<float> kANCHORS = {
        10.0, 14.0, 23.0, 27.0, 37.0, 58.0, 
        81.0, 82.0, 135.0, 169.0, 344.0, 319.0};
    static const std::vector<std::vector<int>> kMASKS = {
        //{0, 1, 2},
        {3, 4, 5},
        {1, 2, 3}};
    static const float beta_nms = 0.2;

    return NvDsInferParseYoloV4 (
        outputLayersInfo, networkInfo, detectionParams, objectList,
        kANCHORS, kMASKS, beta_nms);
}

extern "C" bool NvDsInferParseCustomYoloV3(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList)
{
    static const std::vector<float> kANCHORS = {
        10.0, 13.0, 16.0,  30.0,  33.0, 23.0,  30.0,  61.0,  62.0,
        45.0, 59.0, 119.0, 116.0, 90.0, 156.0, 198.0, 373.0, 326.0};
    static const std::vector<std::vector<int>> kMASKS = {
        {6, 7, 8},
        {3, 4, 5},
        {0, 1, 2}}; 
    return NvDsInferParseYoloV3 (
        outputLayersInfo, networkInfo, detectionParams, objectList,
        kANCHORS, kMASKS);
}

extern "C" bool NvDsInferParseCustomYoloV3Tiny(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList)
{
    static const std::vector<float> kANCHORS = {
        10, 14, 23, 27, 37, 58, 81, 82, 135, 169, 344, 319};
    static const std::vector<std::vector<int>> kMASKS = {
        {3, 4, 5},
        //{0, 1, 2}}; // as per output result, select {1,2,3}
        {1, 2, 3}};

    return NvDsInferParseYoloV3 (
        outputLayersInfo, networkInfo, detectionParams, objectList,
        kANCHORS, kMASKS);
}

static bool NvDsInferParseYoloV2(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList)
{
    // copy anchor data from yolov2.cfg file
    std::vector<float> anchors = {0.57273, 0.677385, 1.87446, 2.06253, 3.33843,
        5.47434, 7.88282, 3.52778, 9.77052, 9.16828};
    const uint kNUM_BBOXES = 5;

    if (outputLayersInfo.empty()) {
        std::cerr << "Could not find output layer in bbox parsing" << std::endl;;
        return false;
    }
    const NvDsInferLayerInfo &layer = outputLayersInfo[0];

    if (NUM_CLASSES_YOLO != detectionParams.numClassesConfigured)
    {
        std::cerr << "WARNING: Num classes mismatch. Configured:"
                  << detectionParams.numClassesConfigured
                  << ", detected by network: " << NUM_CLASSES_YOLO << std::endl;
    }

    assert(layer.inferDims.numDims == 3);
    const uint gridSizeH = layer.inferDims.d[1];
    const uint gridSizeW = layer.inferDims.d[2];
    const uint stride = DIVUP(networkInfo.width, gridSizeW);
    assert(stride == DIVUP(networkInfo.height, gridSizeH));
    for (auto& anchor : anchors) {
        anchor *= stride;
    }
    std::vector<NvDsInferParseObjectInfo> objects =
        decodeYoloV2Tensor((const float*)(layer.buffer), anchors, gridSizeW, gridSizeH, stride, stride, kNUM_BBOXES,
                   NUM_CLASSES_YOLO, networkInfo.width, networkInfo.height);

    objectList = objects;

    return true;
}

extern "C" bool NvDsInferParseCustomYoloV2(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList)
{
    return NvDsInferParseYoloV2 (
        outputLayersInfo, networkInfo, detectionParams, objectList);
}

extern "C" bool NvDsInferParseCustomYoloV2Tiny(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList)
{
    return NvDsInferParseYoloV2 (
        outputLayersInfo, networkInfo, detectionParams, objectList);
}

extern "C" bool NvDsInferParseCustomYoloTLT(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferParseObjectInfo>& objectList)
{

    if(outputLayersInfo.size() != 4)
    {
        std::cerr << "Mismatch in the number of output buffers."
                  << "Expected 4 output buffers, detected in the network :"
                  << outputLayersInfo.size() << std::endl;
        return false;
    }

    const int topK = 200;
    const int* keepCount = static_cast <const int*>(outputLayersInfo.at(0).buffer);
    const float* boxes = static_cast <const float*>(outputLayersInfo.at(1).buffer);
    const float* scores = static_cast <const float*>(outputLayersInfo.at(2).buffer);
    const float* cls = static_cast <const float*>(outputLayersInfo.at(3).buffer);

    for (int i = 0; (i < keepCount[0]) && (objectList.size() <= topK); ++i)
    {
        const float* loc = &boxes[0] + (i * 4);
        const float* conf = &scores[0] + i;
        const float* cls_id = &cls[0] + i;

        if(conf[0] > 1.001)
            continue;

        if((loc[0] < 0) || (loc[1] < 0) || (loc[2] < 0) || (loc[3] < 0))
            continue;

        if((loc[0] > networkInfo.width) || (loc[2] > networkInfo.width) || (loc[1] > networkInfo.height) || (loc[3] > networkInfo.width))
           continue;

        if((loc[2] < loc[0]) || (loc[3] < loc[1]))
            continue;

        if(((loc[3] - loc[1]) > networkInfo.height) || ((loc[2]-loc[0]) > networkInfo.width))
            continue;

        NvDsInferParseObjectInfo curObj{static_cast<unsigned int>(cls_id[0]),
                                        loc[0],loc[1],(loc[2]-loc[0]),
                                        (loc[3]-loc[1]), conf[0]};
        objectList.push_back(curObj);

    }

    return true;
}

/* Check that the custom function has been defined correctly */
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE(NvDsInferParseCustomYoloV4);
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE(NvDsInferParseCustomYoloV4Tiny);
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE(NvDsInferParseCustomYoloV3);
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE(NvDsInferParseCustomYoloV3Tiny);
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE(NvDsInferParseCustomYoloV2);
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE(NvDsInferParseCustomYoloV2Tiny);
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE(NvDsInferParseCustomYoloTLT);
