
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


// materials/measured.cpp*
#include "materials/measured.h"
#include "paramset.h"
#include "floatfile.h"

// MeasuredMaterial Method Definitions
#define BRDF_SAMPLING_RES_THETA_H       90
#define BRDF_SAMPLING_RES_THETA_D       90
#define BRDF_SAMPLING_RES_PHI_D         360

static map<string, float *> loaded;
static map<string, KdTree<ThetaPhiSample> *> loadedThetaPhi;
MeasuredMaterial::MeasuredMaterial(const string &filename,
      Reference<Texture<float> > bump) {
    bumpMap = bump;
    const char *suffix = strrchr(filename.c_str(), '.');
    data = NULL;
    thetaPhiData = NULL;
    if (!suffix)
        Error("No suffix in measured BRDF filename \"%s\".  "
              "Can't determine file type (.brdf / .merl)", filename.c_str());
    if (!strcmp(suffix, ".brdf") || !strcmp(suffix, ".BRDF")) {
        // Load $(\theta, \phi)$ measured BRDF data
        if (loadedThetaPhi.find(filename) != loadedThetaPhi.end()) {
            thetaPhiData = loadedThetaPhi[filename];
            return;
        }
        
        vector<float> values;
        if (!ReadFloatFile(filename.c_str(), &values))
            Error("Unable to read BRDF data from file \"%s\"", filename.c_str());
        
        u_int pos = 0;
        int numWls = int(values[pos++]);
        if ((values.size() - 1 - numWls) % (4 + numWls) != 0)
            Error("Excess or insufficient data in theta, phi BRDF file \"%s\"",
                  filename.c_str());
        vector<float> wls;
        for (int i = 0; i < numWls; ++i)
            wls.push_back(values[pos++]);
        
        BBox bbox;
        vector<ThetaPhiSample> samples;
        while (pos < values.size()) {
            float thetai = values[pos++];
            float phii = values[pos++];
            float thetao = values[pos++];
            float phio = values[pos++];
            Vector wo = SphericalDirection(sinf(thetao), cosf(thetao), phio);
            Vector wi = SphericalDirection(sinf(thetai), cosf(thetai), phii);
            Spectrum s = Spectrum::FromSampled(&wls[0], &values[pos], numWls);
            pos += numWls;
            Point p = BRDFRemap(wo, wi);
            samples.push_back(ThetaPhiSample(p, s));
        bbox = Union(bbox, p);
        }
        //fprintf(stderr, "bbox (%f,%f,%f) - (%f,%f,%f)\n", bbox.pMin.x, bbox.pMin.y, bbox.pMin.z, bbox.pMax.x, bbox.pMax.y, bbox.pMax.z);
        loadedThetaPhi[filename] = thetaPhiData = new KdTree<ThetaPhiSample>(samples);
    }
    else {
        // Load MERL BRDF Data
        if (loaded.find(filename) != loaded.end()) {
            data = loaded[filename];
            return;
        }
        
        FILE *f = fopen(filename.c_str(), "rb");
        if (!f)
            Error("Unable to open material file");
        
        int dims[3];
        if (fread(dims, sizeof(int), 3, f) != 3)
            Error("Premature end-of-file in measured BRDF data file \"%s\"",
                  filename.c_str());
        u_int n = dims[0] * dims[1] * dims[2];
        if (n != BRDF_SAMPLING_RES_THETA_H *
             BRDF_SAMPLING_RES_THETA_D *
             BRDF_SAMPLING_RES_PHI_D / 2)  {
            Error("Dimensions don't match\n");
            fclose(f);
        }
        
        data = new float[3*n];
        const u_int chunkSize = BRDF_SAMPLING_RES_PHI_D;
        double tmp[chunkSize];
        u_int nChunks = n / chunkSize;
        Assert((n % chunkSize) == 0);
        float scales[3] = { 1.f/1500.f, 1.15f/1500.f, 1.66f/1500.f };
        for (int c = 0; c < 3; ++c) {
            int offset = 0;
            for (u_int i = 0; i < nChunks; ++i) {
                if (fread(tmp, sizeof(double), chunkSize, f) != chunkSize)
                    Error("Premature end-of-file in measured BRDF data file \"%s\"",
                          filename.c_str());
                for (u_int j = 0; j < chunkSize; ++j)
                    data[3 * offset++ + c] = tmp[j] * scales[c];
            }
        }
        
        loaded[filename] = data;
        fclose(f);
    }
}


BSDF *MeasuredMaterial::GetBSDF(const DifferentialGeometry &dgGeom,
        const DifferentialGeometry &dgShading,
        MemoryArena &arena) const {
    // Allocate _BSDF_, possibly doing bump mapping with _bumpMap_
    DifferentialGeometry dgs;
    if (bumpMap)
        Bump(bumpMap, dgGeom, dgShading, &dgs);
    else
        dgs = dgShading;
    BSDF *bsdf = BSDF_ALLOC(arena, BSDF)(dgs, dgGeom.nn);
    if (data)
        bsdf->Add(BSDF_ALLOC(arena, MERLMeasuredBRDF)(data));
    else
        bsdf->Add(BSDF_ALLOC(arena, ThetaPhiMeasuredBRDF)(thetaPhiData));
    return bsdf;
}


MeasuredMaterial *CreateMeasuredMaterial(const Transform &xform,
        const TextureParams &mp) {
    Reference<Texture<float> > bumpMap = mp.GetFloatTexture("bumpmap", 0.f);
    return new MeasuredMaterial(mp.FindString("filename"), bumpMap);
}


