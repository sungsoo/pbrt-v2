
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

#ifndef PBRT_TEXTURES_CHECKERBOARD_H
#define PBRT_TEXTURES_CHECKERBOARD_H

// textures/checkerboard.h*
#include "pbrt.h"
#include "texture.h"
#include "paramset.h"
#include "montecarlo.h"
#include "shape.h"
#include "parallel.h"
#include "progressreporter.h"

// CheckerboardTexture Declarations
template <typename T> class Checkerboard2DTexture : public Texture<T> {
public:
    // Checkerboard2DTexture Public Methods
    Checkerboard2DTexture(TextureMapping2D *m, Reference<Texture<T> > c1,
                   Reference<Texture<T> > c2, const string &aa) {
        mapping = m;
        tex1 = c1;
        tex2 = c2;
        // Select antialiasing method for _Checkerboard2DTexture_
        if (aa == "none") aaMethod = NONE;
        else if (aa == "supersample") aaMethod = SUPERSAMPLE;
        else if (aa == "closedform") aaMethod = CLOSEDFORM;
        else {
            Warning("Anti-aliasing mode \"%s\" not understood "
                    "by Checkerboard2DTexture, defaulting"
                    "to \"supersample\"", aa.c_str());
            aaMethod = SUPERSAMPLE;
        }
        mutex = Mutex::Create();
    }
    ~Checkerboard2DTexture() {
        delete mapping;
        Mutex::Destroy(mutex);
    }
    T Evaluate(const DifferentialGeometry &dg) const {
        float s, t, dsdx, dtdx, dsdy, dtdy;
        mapping->Map(dg, &s, &t, &dsdx, &dtdx, &dsdy, &dtdy);
        if (aaMethod == CLOSEDFORM) {
            // Compute closed-form box-filtered _Checkerboard2DTexture_ value

            // Evaluate single check if filter is entirely inside one of them
            float ds = max(fabsf(dsdx), fabsf(dsdy));
            float dt = max(fabsf(dtdx), fabsf(dtdy));
            float s0 = s - ds, s1 = s + ds;
            float t0 = t - dt, t1 = t + dt;
            if (Floor2Int(s0) == Floor2Int(s1) && Floor2Int(t0) == Floor2Int(t1)) {
                // Point sample _Checkerboard2DTexture_
                if ((Floor2Int(s) + Floor2Int(t)) % 2 == 0)
                    return tex1->Evaluate(dg);
                return tex2->Evaluate(dg);
            }

            // Apply box filter to checkerboard region
#define BUMPINT(x) \
                (Floor2Int((x)/2) + \
                 2.f * max((x/2)-Floor2Int(x/2) - .5f, 0.f))
            float sint = (BUMPINT(s1) - BUMPINT(s0)) / (2.f * ds);
            float tint = (BUMPINT(t1) - BUMPINT(t0)) / (2.f * dt);
            float area2 = sint + tint - 2.f * sint * tint;
            if (ds > 1.f || dt > 1.f)
                area2 = .5f;
            return (1.f - area2) * tex1->Evaluate(dg) +
                   area2 * tex2->Evaluate(dg);
        }
        else if (aaMethod == SUPERSAMPLE) {
            // Supersample _Checkerboard2DTexture_
#define SQRT_SAMPLES 4
#define N_SAMPLES (SQRT_SAMPLES * SQRT_SAMPLES)
            float samples[2*N_SAMPLES];
            { MutexLock lock(*mutex);
            StratifiedSample2D(samples, SQRT_SAMPLES, SQRT_SAMPLES, rng); }
            T value = 0.;
            float filterSum = 0.;
            for (int i = 0; i < N_SAMPLES; ++i) {
                // Compute new differential geometry for supersample location
                float dx = samples[2*i]   - 0.5f;
                float dy = samples[2*i+1] - 0.5f;
                DifferentialGeometry dgs = dg;
                dgs.p += dx * dgs.dpdx + dy * dgs.dpdy;
                dgs.u += dx * dgs.dudx + dy * dgs.dudy;
                dgs.v += dx * dgs.dvdx + dy * dgs.dvdy;
                dgs.dudx /= N_SAMPLES;
                dgs.dudy /= N_SAMPLES;
                dgs.dvdx /= N_SAMPLES;
                dgs.dvdy /= N_SAMPLES;

                // Compute $(s,t)$ for supersample and evaluate subtexture
                float ss, ts, dsdxs, dtdxs, dsdys, dtdys;
                mapping->Map(dgs, &ss, &ts, &dsdxs, &dtdxs, &dsdys, &dtdys);
                float wt = expf(-2.f * (dx*dx + dy*dy));
                filterSum += wt;
                if ((Floor2Int(ss) + Floor2Int(ts)) % 2 == 0)
                    value += wt * tex1->Evaluate(dgs);
                else
                    value += wt * tex2->Evaluate(dgs);
            }
            return value / filterSum;
            #undef N_SAMPLES
        }
        // Point sample _Checkerboard2DTexture_
        if ((Floor2Int(s) + Floor2Int(t)) % 2 == 0)
            return tex1->Evaluate(dg);
        return tex2->Evaluate(dg);
    }
private:
    // Checkerboard2DTexture Private Data
    Reference<Texture<T> > tex1, tex2;
    TextureMapping2D *mapping;
    enum { NONE, SUPERSAMPLE, CLOSEDFORM } aaMethod;
    mutable RNG rng;
    mutable Mutex *mutex;
};


template <typename T> class Checkerboard3DTexture : public Texture<T> {
public:
    // Checkerboard3DTexture Public Methods
    Checkerboard3DTexture(TextureMapping3D *m,
                   Reference<Texture<T> > c1,
                   Reference<Texture<T> > c2) {
        mapping = m;
        tex1 = c1;
        tex2 = c2;
        mutex = Mutex::Create(); // FIXME LEAK
    }
    T Evaluate(const DifferentialGeometry &dg) const {
        // Supersample _Checkerboard3DTexture_
#define N_SAMPLES 4
        float samples[2*N_SAMPLES*N_SAMPLES];
        { MutexLock lock(*mutex);
        StratifiedSample2D(samples, N_SAMPLES, N_SAMPLES, rng); }
        T value = 0.;
        float filterSum = 0.;
        for (int i = 0; i < N_SAMPLES*N_SAMPLES; ++i) {
            // Compute new differential geometry for supersample location
            float dx = samples[2*i]   - 0.5f;
            float dy = samples[2*i+1] - 0.5f;
            DifferentialGeometry dgs = dg;
            dgs.p += dx * dgs.dpdx + dy * dgs.dpdy;
            dgs.u += dx * dgs.dudx + dy * dgs.dudy;
            dgs.v += dx * dgs.dvdx + dy * dgs.dvdy;
            dgs.dudx /= N_SAMPLES;
            dgs.dudy /= N_SAMPLES;
            dgs.dvdx /= N_SAMPLES;
            dgs.dvdy /= N_SAMPLES;

            // Compute 3D supersample position and evaluate sub-texture
            Vector dPPdx, dPPdy;
            Point PP = mapping->Map(dgs, &dPPdx, &dPPdy);
            float wt = expf(-2.f * (dx*dx + dy*dy));
            filterSum += wt;
            if ((Floor2Int(PP.x) + Floor2Int(PP.y) + Floor2Int(PP.z)) % 2 == 0)
                value += wt * tex1->Evaluate(dgs);
            else
                value += wt * tex2->Evaluate(dgs);
        }
        return value / filterSum;
    }
private:
    // Checkerboard3DTexture Private Data
    Reference<Texture<T> > tex1, tex2;
    TextureMapping3D *mapping;
    mutable RNG rng;
    mutable Mutex *mutex;
};


Texture<float> *CreateCheckerboardFloatTexture(const Transform &tex2world,
        const TextureParams &tp);
Texture<Spectrum> *CreateCheckerboardSpectrumTexture(const Transform &tex2world,
        const TextureParams &tp);

#endif // PBRT_TEXTURES_CHECKERBOARD_H
