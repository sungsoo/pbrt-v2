
/*
    pbrt source code Copyright(c) 1998-2009 Matt Pharr and Greg Humphreys.

    This file is part of pbrt.

    pbrt is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.  Note that the text contents of
    the book "Physically Based Rendering" are *not* licensed under the
    GNU GPL.

    pbrt is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */


// core/light.cpp*
#include "light.h"
#include "scene.h"
#include "montecarlo.h"
#include "paramset.h"
#include "sh.h"

// Light Method Definitions
Light::~Light() {
}


bool VisibilityTester::Unoccluded(const Scene *scene, float time) const {
    Ray rr = r;
    rr.time = time;
    return !scene->IntersectP(rr);
}


Spectrum
VisibilityTester::Transmittance(const Scene *scene, const Renderer *renderer, float time, const Sample *sample, RNG *rng, MemoryArena &arena) const {
    Ray rr = r;
    rr.time = time;
    return renderer->Transmittance(scene, RayDifferential(rr), sample, arena, rng);
}


Spectrum Light::Le(const RayDifferential &) const {
    return Spectrum(0.);
}


LightSampleOffsets::LightSampleOffsets(int count, Sample *sample) {
    nSamples = count;
    posOffset = sample->Add2D(nSamples);
    componentOffset = sample->Add1D(nSamples);
}


LightSample::LightSample(const Sample *sample,
        const LightSampleOffsets &offsets, u_int num) {
    Assert(num < sample->n2D[offsets.posOffset]);
    Assert(num < sample->n1D[offsets.componentOffset]);
    uPos[0] = sample->twoD[offsets.posOffset][2*num];
    uPos[1] = sample->twoD[offsets.posOffset][2*num+1];
    uComponent = sample->oneD[offsets.componentOffset][num];
}


void Light::SHProject(const Point &p, float pEpsilon, int lmax,
        const Scene *scene, bool computeLightVisibility, float time,
        RNG &rng, Spectrum *coeffs) const {
    for (int i = 0; i < SHTerms(lmax); ++i)
        coeffs[i] = 0.f;
    u_int ns = RoundUpPow2(nSamples);
    u_int scramble1D = rng.RandomUInt();
    u_int scramble2D[2] = { rng.RandomUInt(), rng.RandomUInt() };
    float *Ylm = ALLOCA(float, SHTerms(lmax));
    for (u_int i = 0; i < ns; ++i) {
        // Compute incident radiance sample from _light_, update SH _coeffs_
        float u[2], pdf;
        Sample02(i, scramble2D, u);
        LightSample lightSample(u[0], u[1], VanDerCorput(i, scramble1D));
        Vector wi;
        VisibilityTester vis;
        Spectrum Li = Sample_L(p, pEpsilon, lightSample, &wi, &pdf, &vis);
        if (!Li.IsBlack() && pdf > 0.f &&
            (!computeLightVisibility || vis.Unoccluded(scene, time))) {
            // Add light sample contribution to MC estimate of SH coefficients
            SHEvaluate(wi, lmax, Ylm);
            for (int j = 0; j < SHTerms(lmax); ++j)
                coeffs[j] += Li * Ylm[j] / (pdf * ns);
        }
    }
}



// ShapeSet Method Definitions
ShapeSet::ShapeSet(const Reference<Shape> &s) {
    vector<Reference<Shape> > todo;
    todo.push_back(s);
    while (todo.size()) {
        Reference<Shape> sh = todo.back();
        todo.pop_back();
        if (sh->CanIntersect())
            shapes.push_back(sh);
        else
            sh->Refine(todo);
    }
    if (shapes.size() > 64)
        Warning("Area light geometry turned into %d shapes; "
            "may be very inefficient.", (int)shapes.size());

    // Compute total area of shapes in _ShapeSet_ and area CDF
    sumArea = 0.f;
    for (u_int i = 0; i < shapes.size(); ++i) {
        float a = shapes[i]->Area();
        areas.push_back(a);
        sumArea += a;
    }
    areaDistribution = new Distribution1D(&areas[0], areas.size());
}


ShapeSet::~ShapeSet() {
    delete areaDistribution;
}


Point ShapeSet::Sample(const Point &p, const LightSample &ls,
        Normal *Ns) const {
    int sn = areaDistribution->SampleDiscrete(ls.uComponent, NULL);
    return shapes[sn]->Sample(p, ls.uPos[0], ls.uPos[1], Ns);
}


Point ShapeSet::Sample(const LightSample &ls, Normal *Ns) const {
    int sn = areaDistribution->SampleDiscrete(ls.uComponent, NULL);
    return shapes[sn]->Sample(ls.uPos[0], ls.uPos[1], Ns);
}


float ShapeSet::Pdf(const Point &p, const Vector &wi) const {
    float pdf = 0.f;
    for (u_int i = 0; i < shapes.size(); ++i)
        pdf += areas[i] * shapes[i]->Pdf(p, wi);
    return pdf / sumArea;
}


float ShapeSet::Pdf(const Point &p) const {
    float pdf = 0.f;
    for (u_int i = 0; i < shapes.size(); ++i)
        pdf += areas[i] * shapes[i]->Pdf(p);
    return pdf / sumArea;
}


