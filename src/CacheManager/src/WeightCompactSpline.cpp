#include "CacheWeights.h"
#include "WeightBase.h"
#include "WeightCompactSpline.h"

#include <algorithm>
#include <iostream>
#include <exception>
#include <limits>
#include <cmath>

#include <hemi/hemi_error.h>
#include <hemi/launch.h>
#include <hemi/grid_stride_range.h>

#include "Logger.h"
LoggerInit([]{
  Logger::setUserHeaderStr("[Cache::Weight::CompactSpline]");
});

// The constructor
Cache::Weight::CompactSpline::CompactSpline(
    Cache::Weights::Results& weights,
    Cache::Parameters::Values& parameters,
    Cache::Parameters::Clamps& lowerClamps,
    Cache::Parameters::Clamps& upperClamps,
    std::size_t splines, std::size_t knots,
    std::string spaceOption)
    : Cache::Weight::Base("compactSpline",weights,parameters),
      fLowerClamp(lowerClamps), fUpperClamp(upperClamps),
      fSplinesReserved(splines), fSplinesUsed(0),
      fSplineSpaceReserved(knots), fSplineSpaceUsed(0) {

    LogInfo << "Reserved " << GetName() << " Splines: "
           << GetSplinesReserved() << std::endl;
    if (GetSplinesReserved() < 1) return;

    fTotalBytes += GetSplinesReserved()*sizeof(int);      // fSplineResult
    fTotalBytes += GetSplinesReserved()*sizeof(short);    // fSplineParameter
    fTotalBytes += (1+GetSplinesReserved())*sizeof(int);  // fSplineIndex

    if (spaceOption == "points") {
        fSplineSpaceReserved = 2*fSplinesReserved + fSplineSpaceReserved;
    }
    else {
        LogThrowIf(spaceOption != "space",
                   "Invalid space option for compact splines");
    }

#ifdef CACHE_MANAGER_SLOW_VALIDATION
#warning Using SLOW VALIDATION in Cache::Weight::CompactSpline::CompactSpline
        // Add validation code for the spline calculation.  This can be rather
        // slow, so do not use if it is not required.
    fTotalBytes += GetSplinesReserved()*sizeof(double);
#endif

    LogInfo << "Reserved " << GetName()
            << " Spline Knots: " << GetSplineSpaceReserved()
            << std::endl;
    fTotalBytes += GetSplineSpaceReserved()*sizeof(WEIGHT_BUFFER_FLOAT);  // fSpineKnots


    LogInfo << "Approximate Memory Size for " << GetName()
            << ": " << fTotalBytes/1E+9
            << " GB" << std::endl;

    try {
        // Get the CPU/GPU memory for the spline index tables.  These are
        // copied once during initialization so do not pin the CPU memory into
        // the page set.
        fSplineResult.reset(new hemi::Array<int>(GetSplinesReserved(),false));
        fSplineParameter.reset(
            new hemi::Array<short>(GetSplinesReserved(),false));
        fSplineIndex.reset(new hemi::Array<int>(1+GetSplinesReserved(),false));

#ifdef CACHE_MANAGER_SLOW_VALIDATION
#warning Using SLOW VALIDATION in Cache::Weight::CompactSpline::CompactSpline
        // Add validation code for the spline calculation.  This can be rather
        // slow, so do not use if it is not required.
        fSplineValue.reset(new hemi::Array<double>(GetSplinesReserved(),true));
#endif

        // Get the CPU/GPU memory for the spline knots.  This is copied once
        // during initialization so do not pin the CPU memory into the page
        // set.
        fSplineSpace.reset(
            new hemi::Array<WEIGHT_BUFFER_FLOAT>(GetSplineSpaceReserved(),false));
    }
    catch (std::bad_alloc&) {
        LogError << "Failed to allocate memory, so stopping" << std::endl;
        throw std::runtime_error("Not enough memory available");
    }

    // Initialize the caches.  Don't try to zero everything since the
    // caches can be huge.
    Reset();
    fSplineIndex->hostPtr()[0] = 0;
}

// The destructor
Cache::Weight::CompactSpline::~CompactSpline() {}

int Cache::Weight::CompactSpline::FindPoints(const TSpline3* s) {
    return s->GetNp();
}

void Cache::Weight::CompactSpline::AddSpline(int resIndex,
                                             int parIndex,
                                             const std::vector<double>& splineData) {
    if (resIndex < 0) {
        LogError << "Invalid result index"
               << std::endl;
        throw std::runtime_error("Negative result index");
    }
    if (fWeights.size() <= resIndex) {
        LogError << "Invalid result index"
               << std::endl;
        throw std::runtime_error("Result index out of bounds");
    }
    if (parIndex < 0) {
        LogError << "Invalid parameter index"
               << std::endl;
        throw std::runtime_error("Negative parameter index");
    }
    if (fParameters.size() <= parIndex) {
        LogError << "Invalid parameter index"
               << std::endl;
        throw std::runtime_error("Parameter index out of bounds");
    }
    if (splineData.size() < 4) {
        LogError << "Insufficient points in spline: " << splineData.size()
               << std::endl;
        throw std::runtime_error("Invalid number of spline points");
    }
    int newIndex = fSplinesUsed++;
    if (fSplinesUsed > fSplinesReserved) {
        LogError << "Not enough space reserved for splines"
                 << " Reserved: " << fSplinesReserved
                 << " Used: " << fSplinesUsed
                 << std::endl;
        throw std::runtime_error("Not enough space reserved for splines");
    }
    fSplineResult->hostPtr()[newIndex] = resIndex;
    fSplineParameter->hostPtr()[newIndex] = parIndex;
    if (fSplineIndex->hostPtr()[newIndex] != fSplineSpaceUsed) {
        LogError << "Last spline knot index should be at old end of splines"
                  << std::endl;
        throw std::runtime_error("Problem with control indices");
    }
    int knotIndex = fSplineSpaceUsed;
    fSplineSpaceUsed += splineData.size();
    if (fSplineSpaceUsed > fSplineSpaceReserved) {
        LogError << "Not enough space reserved for spline knots"
                 << " Reserved: " << fSplineSpaceReserved
                 << " Used: " << fSplineSpaceUsed
                 << std::endl;
        throw std::runtime_error("Not enough space reserved for spline knots");
    }
    fSplineIndex->hostPtr()[newIndex+1] = fSplineSpaceUsed;
    for (std::size_t i = 0; i<splineData.size(); ++i) {
        fSplineSpace->hostPtr()[knotIndex+i] = splineData.at(i);
    }

}

void Cache::Weight::CompactSpline::SetSplineKnot(
    int sIndex, int kIndex, double value) {
    if (sIndex < 0) {
        LogError << "Requested spline index is negative"
                  << std::endl;
        std::runtime_error("Invalid control point being set");
    }
    if (GetSplinesUsed() <= sIndex) {
        LogError << "Requested spline index is to large"
                  << std::endl;
        std::runtime_error("Invalid control point being set");
    }
    if (kIndex < 0) {
        LogError << "Requested control point index is negative"
                  << std::endl;
        std::runtime_error("Invalid control point being set");
    }
    int knotIndex = fSplineIndex->hostPtr()[sIndex] + 2 + kIndex;
    if (fSplineIndex->hostPtr()[sIndex+1] <= knotIndex) {
        LogError << "Requested control point index is two large"
                  << std::endl;
        std::runtime_error("Invalid control point being set");
    }
    fSplineSpace->hostPtr()[knotIndex] = value;
}

int Cache::Weight::CompactSpline::GetSplineParameterIndex(int sIndex) {
    if (sIndex < 0) {
        throw std::runtime_error("Spline index invalid");
    }
    if (GetSplinesUsed() <= sIndex) {
        throw std::runtime_error("Spline index invalid");
    }
    return fSplineParameter->hostPtr()[sIndex];
}

double Cache::Weight::CompactSpline::GetSplineParameter(int sIndex) {
    int i = GetSplineParameterIndex(sIndex);
    if (i<0) {
        throw std::runtime_error("Spine parameter index out of bounds");
    }
    if (fParameters.size() <= i) {
        throw std::runtime_error("Spine parameter index out of bounds");
    }
    return fParameters.hostPtr()[i];
}

int Cache::Weight::CompactSpline::GetSplineKnotCount(int sIndex) {
    if (sIndex < 0) {
        throw std::runtime_error("Spline index invalid");
    }
    if (GetSplinesUsed() <= sIndex) {
        throw std::runtime_error("Spline index invalid");
    }
    return fSplineIndex->hostPtr()[sIndex+1]-fSplineIndex->hostPtr()[sIndex]-2;
}

double Cache::Weight::CompactSpline::GetSplineLowerBound(int sIndex) {
    if (sIndex < 0) {
        throw std::runtime_error("Spline index invalid");
    }
    if (GetSplinesUsed() <= sIndex) {
        throw std::runtime_error("Spline index invalid");
    }
    int knotsIndex = fSplineIndex->hostPtr()[sIndex];
    return fSplineSpace->hostPtr()[knotsIndex];
}

double Cache::Weight::CompactSpline::GetSplineUpperBound(int sIndex) {
    if (sIndex < 0) {
        throw std::runtime_error("Spline index invalid");
    }
    if (GetSplinesUsed() <= sIndex) {
        throw std::runtime_error("Spline index invalid");
    }
    int knotCount = GetSplineKnotCount(sIndex);
    double lower = GetSplineLowerBound(sIndex);
    int knotsIndex = fSplineIndex->hostPtr()[sIndex];
    double step = fSplineSpace->hostPtr()[knotsIndex+1];
    return lower + (knotCount-1)/step;
}

double Cache::Weight::CompactSpline::GetSplineLowerClamp(int sIndex) {
    int i = GetSplineParameterIndex(sIndex);
    if (i<0) {
        throw std::runtime_error("Spine lower clamp index out of bounds");
    }
    if (fLowerClamp.size() <= i) {
        throw std::runtime_error("Spine lower clamp index out of bounds");
    }
    return fLowerClamp.hostPtr()[i];
}

double Cache::Weight::CompactSpline::GetSplineUpperClamp(int sIndex) {
    int i = GetSplineParameterIndex(sIndex);
    if (i<0) {
        throw std::runtime_error("Spine upper clamp index out of bounds");
    }
    if (fUpperClamp.size() <= i) {
        throw std::runtime_error("Spine upper clamp index out of bounds");
    }
    return fUpperClamp.hostPtr()[i];
}

double Cache::Weight::CompactSpline::GetSplineKnot(int sIndex, int knot) {
    if (sIndex < 0) {
        throw std::runtime_error("Spline index invalid");
    }
    if (GetSplinesUsed() <= sIndex) {
        throw std::runtime_error("Spline index invalid");
    }
    int knotsIndex = fSplineIndex->hostPtr()[sIndex];
    int count = GetSplineKnotCount(sIndex);
    if (knot < 0) {
        throw std::runtime_error("Knot index invalid");
    }
    if (count <= knot) {
        throw std::runtime_error("Knot index invalid");
    }
    return fSplineSpace->hostPtr()[knotsIndex+2+knot];
}

////////////////////////////////////////////////////////////////////
// This section is for the validation methods.  They should mostly be
// NOOPs and should mostly not be called.

#ifdef CACHE_MANAGER_SLOW_VALIDATION
#warning Using SLOW VALIDATION in Cache::Weight::CompactSpline::GetSplineValue
// Get the intermediate spline result that is used to calculate an event
// weight.  This can trigger a copy from the GPU to CPU, and must only be
// enabled during validation.  Using this validation code also significantly
// increases the amount of GPU memory required.  In a short sentence, "Do not
// use this method."
double* Cache::Weight::CompactSpline::GetCachePointer(int sIndex) {
    if (sIndex < 0) {
        throw std::runtime_error("GetSplineValue: Spline index invalid");
    }
    if (GetSplinesUsed() <= sIndex) {
        throw std::runtime_error("GetSplineValue: Spline index invalid");
    }
    // This can trigger a *slow* copy of the spline values from the GPU to the
    // CPU.
    return fSplineValue->hostPtr() + sIndex;
}
#endif

#include "CacheAtomicMult.h"
#include "CalculateCompactSpline.h"

// Define CACHE_DEBUG to get lots of output from the host
#undef CACHE_DEBUG
#define PRINT_STEP 3

namespace {
    // A function to be used as the kernel on either the CPU or GPU.  This
    // must be valid CUDA coda.
    HEMI_KERNEL_FUNCTION(HEMISplinesKernel,
                         double* results,
#ifdef CACHE_MANAGER_SLOW_VALIDATION
#warning Using SLOW VALIDATION in Cache::Weight::CompactSpline::HEMISplinesKernel
                         // inputs/output for validation
                         double* splineValues,
#endif
                         const double* params,
                         const double* lowerClamp,
                         const double* upperClamp,
                         const WEIGHT_BUFFER_FLOAT* knots,
                         const int* rIndex,
                         const short* pIndex,
                         const int* sIndex,
                         const int NP) {
        for (int i : hemi::grid_stride_range(0,NP)) {
            const int id0 = sIndex[i];
            const int id1 = sIndex[i+1];
            const int dim = id1-id0-2;
            const double x = params[pIndex[i]];
            const double lowBound = knots[id0];
            const double step = 1.0/knots[id0+1];
            const double lClamp = lowerClamp[pIndex[i]];
            const double uClamp = upperClamp[pIndex[i]];

            double v = CalculateCompactSpline(x, lClamp,uClamp,
                                                &knots[id0],dim);

#ifdef CACHE_MANAGER_SLOW_VALIDATION
#warning Using SLOW VALIDATION in Cache::Weight::CompactSpline::HEMISplinesKernel
            splineValues[i] = v;
#endif
            CacheAtomicMult(&results[rIndex[i]], v);
#ifndef HEMI_DEV_CODE
#ifdef CACHE_DEBUG
            if (rIndex[i] < PRINT_STEP) {
                std::cout << "Splines kernel " << i
                       << " iEvt " << rIndex[i]
                       << " iPar " << pIndex[i]
                       << " = " << params[pIndex[i]]
                       << " --> " << x
                       << std::endl;
            }
#endif
#endif
        }
    }
}

void Cache::Weight::CompactSpline::Reset() {
    // Use the parent reset.
    Cache::Weight::Base::Reset();
    // Reset this class
    fSplinesUsed = 0;
    fSplineSpaceUsed = 0;

}

bool Cache::Weight::CompactSpline::Apply() {
    if (GetSplinesUsed() < 1) return false;

    HEMISplinesKernel splinesKernel;
    hemi::launch(splinesKernel,
                 fWeights.writeOnlyPtr(),
#ifdef CACHE_MANAGER_SLOW_VALIDATION
#warning Using SLOW VALIDATION in Cache::Weight::CompactSpline::Apply
                 fSplineValue->writeOnlyPtr(),
#endif
                 fParameters.readOnlyPtr(),
                 fLowerClamp.readOnlyPtr(),
                 fUpperClamp.readOnlyPtr(),
                 fSplineSpace->readOnlyPtr(),
                 fSplineResult->readOnlyPtr(),
                 fSplineParameter->readOnlyPtr(),
                 fSplineIndex->readOnlyPtr(),
                 GetSplinesUsed()
        );

#ifdef CACHE_MANAGER_SLOW_VALIDATION
    // This MUST be done for slow validation.
#warning Using SLOW VALIDATION and copying spine values
    fSplineValue->hostPtr();
#endif

    return true;
}

// An MIT Style License

// Copyright (c) 2022 Clark McGrew

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Local Variables:
// mode:c++
// c-basic-offset:4
// compile-command:"$(git rev-parse --show-toplevel)/cmake/gundam-build.sh"
// End:
