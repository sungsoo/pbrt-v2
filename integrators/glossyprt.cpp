
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


// integrators/glossyprt.cpp*
#include "integrators/glossyprt.h"
#include "sh.h"
#include "light.h"
#include "scene.h"
#include "camera.h"
#include "intersection.h"
#include "paramset.h"

// GlossyPRTIntegrator Method Definitions
GlossyPRTIntegrator::GlossyPRTIntegrator(const Spectrum &kd,
        const Spectrum &ks, float rough, int lm, int ns, bool dt) {
    Kd = kd;
    Ks = ks;
    roughness = rough;
    lmax = lm;
    nSamples = RoundUpPow2(ns);
    doTransfer = dt;
    c_in = B = NULL;
}


GlossyPRTIntegrator::~GlossyPRTIntegrator() {
    delete[] c_in;
    delete[] B;
}


void GlossyPRTIntegrator::Preprocess(const Scene *scene,
        const Camera *camera, const Renderer *renderer) {
    // Project direct lighting into SH for _GlossyPRTIntegrator_
    BBox bbox = scene->WorldBound();
    Point p = .5f * bbox.pMin + .5f * bbox.pMax;
    RNG rng;
    MemoryArena arena;
    c_in = new Spectrum[SHTerms(lmax)];
    SHProjectIncidentDirectRadiance(p, 0.f, camera->ShutterOpen, arena,
        scene, false, lmax, rng, c_in);

    // Compute glossy BSDF matrix for PRT
    B = new Spectrum[SHTerms(lmax)*SHTerms(lmax)];
    SHComputeBSDFMatrix(Kd, Ks, roughness, rng, 1024, lmax, B);
}


void GlossyPRTIntegrator::RequestSamples(Sampler *sampler, Sample *sample, const Scene *scene) {
}


Spectrum GlossyPRTIntegrator::Li(const Scene *scene, const Renderer *,
            const RayDifferential &ray, const Intersection &isect,
            const Sample *sample, MemoryArena &arena) const {
    Spectrum L = 0.f;
    Vector wo = -ray.d;
    // Compute emitted light if ray hit an area light source
    L += isect.Le(wo);

    // Evaluate BSDF at hit point
    BSDF *bsdf = isect.GetBSDF(ray, arena);
    const Point &p = bsdf->dgShading.p;
    // Compute reflected radiance with glossy PRT at point

    // Compute SH radiance transfer matrix at point and SH coefficients
    Spectrum *c_t = arena.Alloc<Spectrum>(SHTerms(lmax));
    if (doTransfer) {
        Spectrum *T = arena.Alloc<Spectrum>(SHTerms(lmax)*SHTerms(lmax));
        SHComputeTransferMatrix(p, isect.RayEpsilon, scene, *sample->rng,
                                nSamples, lmax, T);
        SHMatrixVectorMultiply(T, c_in, c_t, lmax);
    }
    else {
        for (int i = 0; i < SHTerms(lmax); ++i)
            c_t[i] = c_in[i];
    }

    // Rotate incident SH lighting to local coordinate frame
    Vector r1 = bsdf->LocalToWorld(Vector(1,0,0));
    Vector r2 = bsdf->LocalToWorld(Vector(0,1,0));
    Normal nl = Normal(bsdf->LocalToWorld(Vector(0,0,1)));
    Matrix4x4 rot(r1.x, r2.x, nl.x, 0,
                  r1.y, r2.y, nl.y, 0,
                  r1.z, r2.z, nl.z, 0,
                     0,    0,    0, 1);
    Spectrum *c_l = arena.Alloc<Spectrum>(SHTerms(lmax));
    SHRotate(c_t, c_l, rot, lmax, arena);
    #if 0

    // Sample BSDF and integrate against direct SH coefficients
    float *Ylm = ALLOCA(float, SHTerms(lmax));
    int ns = 1024;
    for (int i = 0; i < ns; ++i) {
        Vector wi;
        float pdf;
        Spectrum f = bsdf->Sample_f(wo, &wi, BSDFSample(*sample->rng), &pdf);
        if (pdf > 0.f && !f.IsBlack() && !scene->IntersectP(Ray(p, wi))) {
            f *= fabsf(Dot(wi, n)) / (pdf * ns);
            SHEvaluate(bsdf->WorldToLocal(wi), lmax, Ylm);
    
            Spectrum Li = 0.f;
            for (int j = 0; j < SHTerms(lmax); ++j)
                Li += Ylm[j] * c_l[j] * f;
            L += Li.Clamp();
        }
    }
    #else

    // Compute final coefficients _c\_o_ using BSDF matrix
    Spectrum *c_o = arena.Alloc<Spectrum>(SHTerms(lmax));
    SHMatrixVectorMultiply(B, c_l, c_o, lmax);

    // Evaluate outgong radiance function for $\wo$ and add to _L_
    Vector woLocal = bsdf->WorldToLocal(wo);
    float *Ylm = ALLOCA(float, SHTerms(lmax));
    SHEvaluate(woLocal, lmax, Ylm);
    Spectrum Li = 0.f;
    for (int i = 0; i < SHTerms(lmax); ++i)
        Li += Ylm[i] * c_o[i];
    L += Li.Clamp();
    #endif
    return L;
}


GlossyPRTIntegrator *CreateGlossyPRTIntegratorSurfaceIntegrator(const ParamSet &params) {
    int lmax = params.FindOneInt("lmax", 4);
    int ns = params.FindOneInt("nsamples", 4096);
    bool dt = params.FindOneBool("dotransfer", true);
    Spectrum Kd = params.FindOneSpectrum("Kd", Spectrum(0.5f));
    Spectrum Ks = params.FindOneSpectrum("Ks", Spectrum(0.25f));
    float roughness = params.FindOneFloat("roughness", 10.f);
    return new GlossyPRTIntegrator(Kd, Ks, roughness, lmax, ns, dt);
}


