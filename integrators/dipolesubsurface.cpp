
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


// integrators/dipolesubsurface.cpp*
#include "integrators/dipolesubsurface.h"
#include "shapes/sphere.h"
#include "scene.h"
#include "montecarlo.h"
#include "sampler.h"
#include "octree.h"
#include "camera.h"
#include "probes.h"
#include "parallel.h"
#include "octree.h"
#include "progressreporter.h"
#include "intersection.h"
#include "paramset.h"
#include "reflection.h"
struct DiffusionReflectance;

// DipoleSubsurfaceIntegrator Local Declarations
class PoissonPointTask : public Task {
public:
    PoissonPointTask(const Scene *sc, const Point &org, float ti, int tn,
        float msd, int mf, RWMutex &m, int &rf, int &mrf,
        int &tpt, int &trt, int &npa, GeometricPrimitive &sph,
        Octree<IrradiancePoint> &oct, vector<IrradiancePoint> &ips,
        ProgressReporter &pr)
        : taskNum(tn), scene(sc), origin(org), time(ti),
          minSampleDist(msd), maxFails(mf), mutex(m),
          repeatedFails(rf), maxRepeatedFails(mrf), totalPathsTraced(tpt),
          totalRaysTraced(trt), numPointsAdded(npa), sphere(sph),
          octree(oct), irradiancePoints(ips), prog(pr) { }
    void Run();

    int taskNum;
    const Scene *scene;
    Point origin;
    float time;
    float minSampleDist;
    int maxFails;

    RWMutex &mutex;
    int &repeatedFails, &maxRepeatedFails;
    int &totalPathsTraced, &totalRaysTraced, &numPointsAdded;
    GeometricPrimitive &sphere;
    Octree<IrradiancePoint> &octree;
    vector<IrradiancePoint> &irradiancePoints;
    ProgressReporter &prog;
};


struct PoissonCheck {
    PoissonCheck(float md, const Point &pt)
        { maxDist2 = md * md; failed = false; p = pt; }
    float maxDist2;
    bool failed;
    Point p;
    bool operator()(const IrradiancePoint &ip) {
        if (DistanceSquared(ip.p, p) < maxDist2) {
            failed = true; return false;
        }
        return true;
    }
};


struct SubsurfaceOctreeNode {
    // SubsurfaceOctreeNode Methods
    SubsurfaceOctreeNode() {
        isLeaf = true;
        sumArea = 0.f;
        for (int i = 0; i < 8; ++i)
            ips[i] = NULL;
    }
    void Insert(const BBox &nodeBound, IrradiancePoint *ip,
            MemoryArena &arena) {
        Point pMid = .5f * nodeBound.pMin + .5f * nodeBound.pMax;
        if (isLeaf) {
            // Add _IrradiancePoint_ to leaf octree node
            for (int i = 0; i < 8; ++i) {
                if (!ips[i]) {
                    ips[i] = ip;
                    return;
                }
            }

            // Convert leaf node to interior node, redistribute points
            isLeaf = false;
            IrradiancePoint *localIps[8];
            for (int i = 0; i < 8; ++i) {
                localIps[i] = ips[i];
                children[i] = NULL;
            }
            for (int i = 0; i < 8; ++i)  {
                IrradiancePoint *ip = localIps[i];
                // Add _IrradiancePoint_ _ip_ to interior octree node
                int child = (ip->p.x > pMid.x ? 4 : 0) +
                    (ip->p.y > pMid.y ? 2 : 0) + (ip->p.z > pMid.z ? 1 : 0);
                if (!children[child])
                    children[child] = arena.Alloc<SubsurfaceOctreeNode>();
                BBox childBound = octreeChildBound(child, nodeBound, pMid);
                children[child]->Insert(childBound, ip, arena);
            }
            /* fall through to interior case to insert the new point... */
        }
        // Add _IrradiancePoint_ _ip_ to interior octree node
        int child = (ip->p.x > pMid.x ? 4 : 0) +
            (ip->p.y > pMid.y ? 2 : 0) + (ip->p.z > pMid.z ? 1 : 0);
        if (!children[child])
            children[child] = arena.Alloc<SubsurfaceOctreeNode>();
        BBox childBound = octreeChildBound(child, nodeBound, pMid);
        children[child]->Insert(childBound, ip, arena);
    }
    void InitHierarchy() {
        if (isLeaf) {
            // Init _SubsurfaceOctreeNode_ leaf from _IrradiancePoint_s
            float sumWt = 0.f;
            u_int i;
            for (i = 0; i < 8; ++i) {
                if (!ips[i]) break;
                float wt = ips[i]->E.y();
                E += ips[i]->E;
                p += wt * ips[i]->p;
                sumWt += wt;
                sumArea += ips[i]->area;
            }
            if (sumWt > 0.f) p /= sumWt;
            E /= i;
        }
        else {
            // Init interior _SubsurfaceOctreeNode_
            float sumWt = 0.f;
            u_int nChildren = 0;
            for (u_int i = 0; i < 8; ++i) {
                if (!children[i]) continue;
                ++nChildren;
                children[i]->InitHierarchy();
                float wt = children[i]->E.y();
                E += children[i]->E;
                p += wt * children[i]->p;
                sumWt += wt;
                sumArea += children[i]->sumArea;
            }
            if (sumWt > 0.f) p /= sumWt;
            E /= nChildren;
        }
    }
    Spectrum Mo(const BBox &nodeBound, const Point &p, const DiffusionReflectance &Rd,
        float maxError);

    // SubsurfaceOctreeNode Public Data
    Point p;
    bool isLeaf;
    Spectrum E;
    float sumArea;
    union {
        SubsurfaceOctreeNode *children[8];
        IrradiancePoint *ips[8];
    };
};


struct DiffusionReflectance {
    // DiffusionReflectance Public Methods
    DiffusionReflectance(const Spectrum &sigma_a,
            const Spectrum &sigmap_s, float eta) {
        A = (1.f + Fdr(eta)) / (1.f - Fdr(eta));
        sigmap_t = sigma_a + sigmap_s;
        sigma_tr = Sqrt(3.f * sigma_a * sigmap_t);
        alphap = sigmap_s / sigmap_t;
        zpos = Spectrum(1.f) / sigmap_t;
        zneg = zpos * (1.f + (4.f/3.f) * A);
    }
    Spectrum operator()(float d2) const {
        Spectrum dpos = Sqrt(Spectrum(d2) + zpos * zpos);
        Spectrum dneg = Sqrt(Spectrum(d2) + zneg * zneg);
        Spectrum Rd = (1.f / (4.f * M_PI)) *
            ((zpos * (dpos * sigma_tr + Spectrum(1.f)) *
              Exp(-sigma_tr * dpos)) / (dpos * dpos * dpos) -
             (zneg * (dneg * sigma_tr + Spectrum(1.f)) *
              Exp(-sigma_tr * dneg)) / (dneg * dneg * dneg));
        return Rd.Clamp();
    }

    // DiffusionReflectance Data
    Spectrum zpos, zneg, sigmap_t, sigma_tr, alphap;
    float A;
};



// DipoleSubsurfaceIntegrator Method Definitions
DipoleSubsurfaceIntegrator::~DipoleSubsurfaceIntegrator() {
    delete[] lightSampleOffsets;
    delete[] bsdfSampleOffsets;
}


void DipoleSubsurfaceIntegrator::RequestSamples(Sampler *sampler, Sample *sample,
        const Scene *scene) {
    // Allocate and request samples for sampling all lights
    u_int nLights = scene->lights.size();
    lightSampleOffsets = new LightSampleOffsets[nLights];
    bsdfSampleOffsets = new BSDFSampleOffsets[nLights];
    for (u_int i = 0; i < nLights; ++i) {
        const Light *light = scene->lights[i];
        int nSamples = light->nSamples;
        if (sampler) nSamples = sampler->RoundSize(nSamples);
        lightSampleOffsets[i] = LightSampleOffsets(nSamples, sample);
        bsdfSampleOffsets[i] = BSDFSampleOffsets(nSamples, sample);
    }
}


void DipoleSubsurfaceIntegrator::Preprocess(const Scene *scene,
        const Camera *camera, const Renderer *renderer) {
    if (scene->lights.size() == 0) return;
    // Declare shared variables for Poisson point generation

    // Create scene bounding sphere to catch rays that leave the scene
    Point sceneCenter;
    float sceneRadius;
    scene->WorldBound().BoundingSphere(&sceneCenter, &sceneRadius);
    Transform ObjectToWorld(Translate(sceneCenter - Point(0,0,0)));
    Transform WorldToObject(Inverse(ObjectToWorld));
    Reference<Shape> sph = new Sphere(&ObjectToWorld, &WorldToObject,
        true, sceneRadius, -sceneRadius, sceneRadius, 360.f);
    Reference<Material> nullMaterial = Reference<Material>(NULL);
    GeometricPrimitive sphere(sph, nullMaterial, NULL);
    int maxFails = 2000, repeatedFails = 0, maxRepeatedFails = 0;
    if (getenv("PBRT_QUICK_RENDER")) maxFails = max(10, maxFails / 10);
    int totalPathsTraced = 0, totalRaysTraced = 0, numPointsAdded = 0;
    BBox octBounds = scene->WorldBound();
    octBounds.Expand(.001f * powf(octBounds.Volume(), 1.f/3.f));
    Octree<IrradiancePoint> pointOctree(octBounds);
    ProgressReporter prog(maxFails, "Depositing samples");
    // Launch tasks to trace rays to find Poisson points
    PBRT_SUBSURFACE_STARTED_RAYS_FOR_POINTS();
    vector<Task *> tasks;
    RWMutex *mutex = RWMutex::Create();
    int nTasks = NumSystemCores();
    Point origin = camera->CameraToWorld(camera->ShutterOpen,
                                         Point(0, 0, 0));
    for (int i = 0; i < nTasks; ++i)
        tasks.push_back(new PoissonPointTask(scene, origin, camera->ShutterOpen, i,
            minSampleDist, maxFails, *mutex, repeatedFails, maxRepeatedFails,
            totalPathsTraced, totalRaysTraced, numPointsAdded, sphere, pointOctree,
            irradiancePoints, prog));
    EnqueueTasks(tasks);
    WaitForAllTasks();
    for (u_int i = 0; i < tasks.size(); ++i)
        delete tasks[i];
    RWMutex::Destroy(mutex);
    prog.Done();
    PBRT_SUBSURFACE_FINISHED_RAYS_FOR_POINTS(totalRaysTraced, numPointsAdded);
    // Compute irradiance values at sample points
    RNG rng;
    MemoryArena arena;
    PBRT_SUBSURFACE_STARTED_COMPUTING_IRRADIANCE_VALUES();
    ProgressReporter progress(irradiancePoints.size(), "Computing Irradiances");
    for (u_int i = 0; i < irradiancePoints.size(); ++i) {
        IrradiancePoint &ip = irradiancePoints[i];
        for (u_int j = 0; j < scene->lights.size(); ++j) {
            // Add irradiance from light at point
            const Light *light = scene->lights[j];
            Spectrum Elight = 0.f;
            int nSamples = RoundUpPow2(light->nSamples);
            u_int scramble[2] = { rng.RandomUInt(), rng.RandomUInt() };
            u_int compScramble = rng.RandomUInt();
            for (int s = 0; s < nSamples; ++s) {
                float lpos[2];
                Sample02(s, scramble, lpos);
                float lcomp = VanDerCorput(s, compScramble);
                LightSample ls(lpos[0], lpos[1], lcomp);
                Vector wi;
                float lightPdf;
                VisibilityTester visibility;
                Spectrum Li = light->Sample_L(ip.p, ip.rayEpsilon,
                    ls, &wi, &lightPdf, &visibility);
                if (Dot(wi, ip.n) <= 0.) continue;
                if (Li.IsBlack() || lightPdf == 0.f) continue;
                Li *= visibility.Transmittance(scene, renderer, camera->ShutterOpen, NULL, &rng, arena);
                if (visibility.Unoccluded(scene, camera->ShutterOpen))
                    Elight += Li * Dot(wi, ip.n) / lightPdf;
            }
            ip.E += Elight / nSamples;
        }
        PBRT_SUBSURFACE_COMPUTED_IRRADIANCE_AT_POINT(&ip);
        arena.FreeAll();
        progress.Update();
    }
    progress.Done();
    PBRT_SUBSURFACE_FINISHED_COMPUTING_IRRADIANCE_VALUES();

    // Create octree of clustered irradiance samples
    octree = octreeArena.Alloc<SubsurfaceOctreeNode>();
    for (u_int i = 0; i < irradiancePoints.size(); ++i)
        octreeBounds = Union(octreeBounds, irradiancePoints[i].p);
    for (u_int i = 0; i < irradiancePoints.size(); ++i)
        if (irradiancePoints[i].E.y() > 0.f)
            octree->Insert(octreeBounds, &irradiancePoints[i], octreeArena);
    octree->InitHierarchy();
}


void PoissonPointTask::Run() {
    // Declare common variables for _PoissonPointTask::Run()_
    RNG rng(37 * taskNum);
    MemoryArena arena;
    vector<IrradiancePoint> candidates;
    while (true) {
        int pathsTraced, raysTraced = 0;
        for (pathsTraced = 0; pathsTraced < 20000; ++pathsTraced) {
            // Follow ray path and attempt to deposit candidate sample points
            Vector dir = UniformSampleSphere(rng.RandomFloat(), rng.RandomFloat());
            Ray ray(origin, dir, 0.f, INFINITY, time);
            while (ray.depth < 30) {
                // Find ray intersection with scene geometry or bounding sphere
                ++raysTraced;
                Intersection isect;
                bool hitOnSphere = false;
                if (!scene->Intersect(ray, &isect)) {
                    if (!sphere.Intersect(ray, &isect))
                        break;
                    hitOnSphere = true;
                }
                DifferentialGeometry &hitGeometry = isect.dg;
                hitGeometry.nn = Faceforward(hitGeometry.nn, -ray.d);

                // Store candidate sample point at ray intersection if appropriate
                if (!hitOnSphere && ray.depth >= 3 &&
                    isect.GetBSSRDF(RayDifferential(ray), arena) != NULL) {
                    IrradiancePoint ip;
                    ip.p = hitGeometry.p;
                    ip.n = hitGeometry.nn;
                    ip.area = M_PI * minSampleDist * minSampleDist;
                    ip.rayEpsilon = isect.RayEpsilon;
                    candidates.push_back(ip);
                }

                // Generate random ray from intersection point
                Vector dir = UniformSampleSphere(rng.RandomFloat(), rng.RandomFloat());
                dir = Faceforward(dir, hitGeometry.nn);
                ray = Ray(hitGeometry.p, dir, ray, isect.RayEpsilon);
            }
            arena.FreeAll();
        }
        // Make first pass through candidate points with reader lock
        vector<bool> candidateRejected;
        candidateRejected.reserve(candidates.size());
        RWMutexLock lock(mutex, READ);
        for (u_int i = 0; i < candidates.size(); ++i) {
            IrradiancePoint &ip = candidates[i];
            PoissonCheck check(minSampleDist, ip.p);
            octree.Lookup(ip.p, check);
            candidateRejected.push_back(check.failed);
        }

        // Make second pass through points with writer lock and update octree
        lock.UpgradeToWrite();
        if (repeatedFails >= maxFails)
            return;
        totalPathsTraced += pathsTraced;
        totalRaysTraced += raysTraced;
        int oldMaxRepeatedFails = maxRepeatedFails;
        for (u_int i = 0; i < candidates.size(); ++i) {
            if (candidateRejected[i]) {
                // Update for rejected candidate point
                maxRepeatedFails = max(maxRepeatedFails, ++repeatedFails);
                if (repeatedFails >= maxFails)
                    return;
            }
            else {
                // Recheck candidate point and possibly add to octree
                IrradiancePoint &ip = candidates[i];
                PoissonCheck check(minSampleDist, ip.p);
                octree.Lookup(ip.p, check);
                if (check.failed) {
                    // Update for rejected candidate point
                    maxRepeatedFails = max(maxRepeatedFails, ++repeatedFails);
                    if (repeatedFails >= maxFails)
                        return;
                }
                else {
                    ++numPointsAdded;
                    repeatedFails = 0;
                    Vector delta(minSampleDist, minSampleDist, minSampleDist);
                    octree.Add(ip, BBox(ip.p-delta, ip.p+delta));
                    PBRT_SUBSURFACE_ADDED_POINT_TO_OCTREE(&ip, minSampleDist);
                    irradiancePoints.push_back(ip);
                }
            }
        }

        // Stop following paths if not finding new points
        if (repeatedFails > oldMaxRepeatedFails) {
            int delta = repeatedFails - oldMaxRepeatedFails;
            prog.Update(delta);
        }
        if (totalPathsTraced > 50000 && numPointsAdded == 0) {
            Warning("There don't seem to be any objects with BSSRDFs "
                    "in this scene.  Giving up.");
            return;
        }
        candidates.erase(candidates.begin(), candidates.end());
    }
}


Spectrum DipoleSubsurfaceIntegrator::Li(const Scene *scene, const Renderer *renderer,
        const RayDifferential &ray, const Intersection &isect,
        const Sample *sample, MemoryArena &arena) const {
    Spectrum L(0.);
    Vector wo = -ray.d;
    // Compute emitted light if ray hit an area light source
    L += isect.Le(wo);

    // Evaluate BSDF at hit point
    BSDF *bsdf = isect.GetBSDF(ray, arena);
    const Point &p = bsdf->dgShading.p;
    const Normal &n = bsdf->dgShading.nn;
    // Evaluate BSSRDF and possibly compute subsurface scattering
    BSSRDF *bssrdf = isect.GetBSSRDF(ray, arena);
    if (bssrdf) {
        Spectrum sigma_a  = bssrdf->sigma_a();
        Spectrum sigmap_s = bssrdf->sigma_prime_s();
        Spectrum sigmap_t = sigmap_s + sigma_a;
        if (!sigmap_t.IsBlack()) {
            // Compute direct transmittance in scattering medium

            // Compute single scattering in scattering medium

            // Use hierarchical integration to evaluate reflection from dipole model
            PBRT_SUBSURFACE_STARTED_OCTREE_LOOKUP(const_cast<Point *>(&p));
            DiffusionReflectance Rd(sigma_a, sigmap_s, bssrdf->eta);
            Spectrum Mo = octree->Mo(octreeBounds, p, Rd, maxError);
            FresnelDielectric fresnel(1.f, bssrdf->eta);
            Spectrum Ft = Spectrum(1.f) - fresnel.Evaluate(AbsDot(wo, n));
            float Fdt = 1.f - Fdr(bssrdf->eta);
            L += (INV_PI * Ft) * (Fdt * Mo);
            PBRT_SUBSURFACE_FINISHED_OCTREE_LOOKUP();
        }
    }
    L += UniformSampleAllLights(scene, renderer, arena, p, n,
        wo, isect.RayEpsilon, bsdf, sample, lightSampleOffsets, bsdfSampleOffsets);
    if (ray.depth < maxSpecularDepth) {
        // Trace rays for specular reflection and refraction
        L += SpecularReflect(ray, bsdf, *sample->rng, isect, renderer,
                             scene, sample, arena);
        L += SpecularTransmit(ray, bsdf, *sample->rng, isect, renderer,
                              scene, sample, arena);
    }
    return L;
}


Spectrum SubsurfaceOctreeNode::Mo(const BBox &nodeBound, const Point &pt,
        const DiffusionReflectance &Rd, float maxError) {
    // Compute $M_\roman{o}$ at node if error is low enough
    float dw = sumArea / DistanceSquared(pt, p);
    if (dw < maxError && !nodeBound.Inside(pt))
    {
        PBRT_SUBSURFACE_ADDED_INTERIOR_CONTRIBUTION(const_cast<SubsurfaceOctreeNode *>(this));
        return Rd(DistanceSquared(pt, p)) * E * sumArea;
    }

    // Otherwise comupte $M_\roman{o}$ from points in leaf or recursively visit children
    Spectrum Mo = 0.f;
    if (isLeaf) {
        // Accumulate $M_\roman{o}$ from leaf node
        for (int i = 0; i < 8; ++i) {
            if (!ips[i]) break;
            PBRT_SUBSURFACE_ADDED_POINT_CONTRIBUTION(const_cast<IrradiancePoint *>(ips[i]));
            Mo += Rd(DistanceSquared(pt, ips[i]->p)) * ips[i]->E * ips[i]->area;
        }
    }
    else {
        // Recursively visit children nodes to compute $M_\roman{o}$
        Point pMid = .5f * nodeBound.pMin + .5f * nodeBound.pMax;
        for (int child = 0; child < 8; ++child) {
            if (!children[child]) continue;
            BBox childBound = octreeChildBound(child, nodeBound, pMid);
            Mo += children[child]->Mo(childBound, pt, Rd, maxError);
        }
    }
    return Mo;
}


DipoleSubsurfaceIntegrator *CreateDipoleSubsurfaceIntegrator(const ParamSet &params) {
    int maxDepth = params.FindOneInt("maxdepth", 5);
    float maxError = params.FindOneFloat("maxerror", .05f);
    float minDist = params.FindOneFloat("minsampledistance", .25f);
    if (getenv("PBRT_QUICK_RENDER")) { maxError *= 4.f; minDist *= 4.f; }
    return new DipoleSubsurfaceIntegrator(maxDepth, maxError, minDist);
}


