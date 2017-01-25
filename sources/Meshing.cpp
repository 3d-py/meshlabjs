#include "mesh_def.h"
#include <vcg/complex/algorithms/local_optimization/tri_edge_collapse_quadric.h>
#include <vcg/complex/algorithms/clustering.h>
#include <vcg/complex/algorithms/convex_hull.h>
#include<vcg/complex/algorithms/point_sampling.h>
#include<vcg/complex/algorithms/voronoi_processing.h>
#include<vcg/complex/algorithms/crease_cut.h>
#include<vcg/complex/algorithms/curve_on_manifold.h>
#include<vcg/complex/algorithms/hole.h>


using namespace vcg;
using namespace std;


typedef	SimpleTempData<MyMesh::VertContainer, math::Quadric<double> > QuadricTemp;

class QHelper
{
public:
    QHelper(){}
    static void Init(){}
    static math::Quadric<double> &Qd(MyVertex &v) {return TD()[v];}
    static math::Quadric<double> &Qd(MyVertex *v) {return TD()[*v];}
    static MyVertex::ScalarType W(MyVertex * /*v*/) {return 1.0;}
    static MyVertex::ScalarType W(MyVertex & /*v*/) {return 1.0;}
    static void Merge(MyVertex & /*v_dest*/, MyVertex const & /*v_del*/){}
    static QuadricTemp* &TDp() {static QuadricTemp *td; return td;}
    static QuadricTemp &TD() {return *TDp();}
};

typedef tri::BasicVertexPair<MyVertex> VertexPair;

class MyTriEdgeCollapse: public vcg::tri::TriEdgeCollapseQuadric< MyMesh, VertexPair , MyTriEdgeCollapse, QHelper > {
public:
    typedef  vcg::tri::TriEdgeCollapseQuadric< MyMesh, VertexPair,  MyTriEdgeCollapse, QHelper> TECQ;
    inline MyTriEdgeCollapse(  const VertexPair &p, int i, BaseParameterClass *pp) :TECQ(p,i,pp){}
};

void ClusteringSimplification(uintptr_t _baseM, float threshold)
{
    MyMesh &m = *((MyMesh*) _baseM);
    tri::Clustering<MyMesh, vcg::tri::AverageColorCell<MyMesh> > ClusteringGrid;
    float cellSize = m.bbox.Diag() * threshold;
    printf("Clustering with a cell size of %6.2f = %4.2f * %6.2f\n",cellSize,threshold,m.bbox.Diag());

    ClusteringGrid.Init(m.bbox,100000,cellSize);
    if(m.FN() ==0)
        ClusteringGrid.AddPointSet(m);
    else
        ClusteringGrid.AddMesh(m);

    ClusteringGrid.ExtractMesh(m);
    printf("Completed Clustering Simplification\n");

    m.UpdateBoxAndNormals();
}

void QuadricSimplification(uintptr_t _baseM, float TargetFaceRatio, int exactFaceNum, 
                           bool topologyFlag, bool qualityQuadric)
{
    MyMesh &m = *((MyMesh*) _baseM);
    tri::UpdateTopology<MyMesh>::ClearFaceFace(m);
    math::Quadric<double> QZero;
    QZero.SetZero();
    QuadricTemp TD(m.vert,QZero);
    QHelper::TDp()=&TD;

    tri::TriEdgeCollapseQuadricParameter pp;
    pp.NormalCheck = true;
    pp.PreserveTopology = topologyFlag;
    if(pp.NormalCheck) pp.NormalThrRad = M_PI/4.0;
    if(qualityQuadric) pp.QualityQuadric=true;
    pp.QualityThr=0.3;

    vcg::LocalOptimization<MyMesh> DeciSession(m,&pp);
    DeciSession.Init<MyTriEdgeCollapse >();
    int TargetFaceNum;
    if(exactFaceNum==0) TargetFaceNum = m.fn * TargetFaceRatio;
    else TargetFaceNum = exactFaceNum;
    DeciSession.SetTargetSimplices(TargetFaceNum);
    DeciSession.SetTimeBudget(0.1f); // this allow to update the progress bar 10 time for sec...

    while( DeciSession.DoOptimization() && m.fn>TargetFaceNum );
    DeciSession.Finalize<MyTriEdgeCollapse >();

    int dfn = tri::Clean<MyMesh>::RemoveDegenerateFace(m);
    int uvn = tri::Clean<MyMesh>::RemoveUnreferencedVertex(m);
    if(dfn>0) printf("Removed %i degen faces\n",dfn);
    if(uvn>0) printf("Removed %i unref verts\n",uvn);

    m.UpdateBoxAndNormals();
    printf("Completed Simplification\n");
}

void RemoveUnreferencedVertices(uintptr_t _baseM)
{
    MyMesh &m = *((MyMesh*) _baseM);
    int rvn = tri::Clean<MyMesh>::RemoveUnreferencedVertex(m);
    m.UpdateBoxAndNormals();
    printf("Removed %i unreferenced vertices\n",rvn);
}

void InvertFaceOrientation(uintptr_t _baseM)
{
    MyMesh &m = *((MyMesh*) _baseM);
    tri::Clean<MyMesh>::FlipMesh(m);
}

void RemoveDuplicatedVertices(uintptr_t _baseM)
{
    MyMesh &m = *((MyMesh*) _baseM);
    int cnt = tri::Clean<MyMesh>::RemoveDuplicateVertex(m);
    printf("Removed %i duplicated vertices\n",cnt);
    m.UpdateBoxAndNormals();
}

void ConvexHullFilter(uintptr_t _baseM, uintptr_t _newM)
{
    MyMesh &m = *((MyMesh*) _baseM);
    MyMesh &ch = *((MyMesh*) _newM);
    ch.Clear();
    tri::ConvexHull<MyMesh,MyMesh>::ComputeConvexHull(m,ch);
    ch.UpdateBoxAndNormals();
} 

void VoronoiClustering(uintptr_t _baseM, uintptr_t _newM, float clusteringRatio, int iterNum, int relaxType, int postRelaxStep, int postRefineStep, bool colorizeMeshFlag)
{
    MyMesh &origMesh = *((MyMesh*) _baseM);
    MyMesh &clusteredMesh = *((MyMesh*) _newM);
    MyMesh baseMesh;
    tri::Append<MyMesh,MyMesh>::MeshCopy(baseMesh,origMesh);
    tri::Clean<MyMesh>::RemoveUnreferencedVertex(baseMesh);
    tri::Allocator<MyMesh>::CompactEveryVector(baseMesh);

    // if you ask too many seeds wrt to mesh size we must refine the base mesh.
    int seedNum=baseMesh.vn*clusteringRatio;
    while(baseMesh.fn / 30 < seedNum )
    {
        tri::TrivialMidPointRefine(baseMesh);
        printf("refined from %i to %i\n",origMesh.vn, baseMesh.vn);
    }

    clusteredMesh.Clear();
    printf("Starting Voronoi Clustering of a mesh of %i vert targeting %i seeds\n",baseMesh.vn,seedNum);
    tri::UpdateTopology<MyMesh>::VertexFace(baseMesh);
    tri::TrivialPointerSampler<MyMesh> cs;
    tri::SurfaceSampling<MyMesh, tri::TrivialPointerSampler<MyMesh> >::VertexUniform(baseMesh,cs,seedNum);
    tri::VoronoiProcessingParameter vpp;

    if(relaxType==0) vpp.geodesicRelaxFlag=false; // 0 -> Quadric
    else vpp.geodesicRelaxFlag=true; //              1 -> Geodesic

    vpp.deleteUnreachedRegionFlag=true;

    tri::EuclideanDistance<MyMesh> df;
    printf("Relaxing %i times\n",iterNum);
    tri::VoronoiProcessing<MyMesh>::VoronoiRelaxing(baseMesh, cs.sampleVec, iterNum, df, vpp);
    tri::VoronoiProcessing<MyMesh>::ConvertDelaunayTriangulationToMesh(baseMesh,clusteredMesh,cs.sampleVec,false);
    printf("Completed. Created Mesh of %i v and %i f\n",clusteredMesh.vn,clusteredMesh.fn);
    printf("Relaxing %i steps Refining %i steps\n",postRelaxStep,postRefineStep);
    tri::VoronoiProcessing<MyMesh>::RelaxRefineTriangulationSpring(baseMesh,clusteredMesh,postRelaxStep,postRefineStep);

    if(colorizeMeshFlag)
    {
        std::vector<Point3f> seedPVec;
        std::vector<MyVertex *>seedVVec;
        for(size_t i=0;i<cs.sampleVec.size();++i)
            seedPVec.push_back(cs.sampleVec[i]->P());
        tri::UpdateTopology<MyMesh>::VertexFace(origMesh);
        tri::VoronoiProcessing<MyMesh>::SeedToVertexConversion(origMesh,seedPVec,seedVVec);
        tri::VoronoiProcessing<MyMesh>::ComputePerVertexSources(origMesh,seedVVec,df);
        tri::VoronoiProcessing<MyMesh>::VoronoiColoring(origMesh,false);
    }
    clusteredMesh.UpdateBoxAndNormals();
}


void CutAlongCreaseFilter(uintptr_t _baseM, float angleDeg)
{
    MyMesh &m = *((MyMesh*) _baseM);
    tri::UpdateTopology<MyMesh>::FaceFace(m);
    printf("Crease angle %f\n",angleDeg);
    tri::CreaseCut<MyMesh>(m, math::ToRad(angleDeg));

    m.UpdateBoxAndNormals();
}

void CutTopologicalFilter(uintptr_t _baseM)
{
    MyMesh &m = *((MyMesh*) _baseM);
    MyMesh poly;
    tri::CoM<MyMesh> cc(m);
    cc.BuildVisitTree(poly);
    while(cc.OptimizeTree(poly));
    cc.MarkFauxEdgeWithPolyLine(m,poly);
    tri::UpdateTopology<MyMesh>::FaceFace(m);
    tri::CutMeshAlongNonFauxEdges<MyMesh>(m);
    m.UpdateBoxAndNormals();
}

void ReorientFaceCoherently(uintptr_t _baseM)
{
    MyMesh &m = *((MyMesh*) _baseM);
    bool _isOriented,_isOrientable;
    tri::UpdateTopology<MyMesh>::FaceFace(m);
    tri::Clean<MyMesh>::OrientCoherentlyMesh(m,_isOriented,_isOrientable);
}

void HoleFilling(uintptr_t _baseM, int maxHoleEdgeNum)
{
    MyMesh &m = *((MyMesh*) _baseM);
    tri::UpdateTopology<MyMesh>::FaceFace(m);
    tri::Hole<MyMesh>::EarCuttingFill< tri::TrivialEar<MyMesh> > (m, maxHoleEdgeNum);
}



#include<vcg/complex/algorithms/update/color.h>
#include<vcg/complex/algorithms/update/quality.h>
#include<vcg/complex/algorithms/update/curvature.h>
#include<vcg/complex/algorithms/update/normal.h>
#include<vcg/complex/algorithms/refine.h>
#include<vcg/complex/algorithms/stat.h>
#include<vcg/complex/algorithms/smooth.h>
#include<vcg/complex/algorithms/local_optimization/tri_edge_collapse.h>
#include<vcg/space/index/spatial_hashing.h>


typedef  face::Pos<MyFace> MyPos;
typedef  BasicVertexPair<MyVertex> MyPair;
typedef  EdgeCollapser<MyMesh, BasicVertexPair<MyVertex>> MyCollapser;
typedef  GridStaticPtr<MyFace, float> MyGrid;


float lerp (float a, float b, float lambda)
{
    math::Clamp(lambda, 0.f, 1.f);
    return a * lambda + (1-lambda) * b;
}






void MapErrorColor(MyMesh &m)
{
    static float minQ=0, maxQ=0;
    if(minQ==maxQ)
        tri::Stat<MyMesh>::ComputePerFaceQualityMinMax(m,minQ,maxQ);
    tri::UpdateColor<MyMesh>::PerFaceQualityRamp(m,minQ,maxQ);
}

void MapCreaseColor(MyMesh &m)
{
    tri::UpdateTopology<MyMesh>::VertexFace(m);
    tri::UpdateTopology<MyMesh>::FaceFace(m);
    tri::UpdateFlags<MyMesh>::FaceClearV(m);
    tri::UpdateFlags<MyMesh>::VertexClearV(m);
    tri::UpdateFlags<MyMesh>::FaceFauxCrease(m,math::ToRad(15.f));
    for(auto fi=m.face.begin(); fi!=m.face.end(); ++fi)
        if(!(*fi).IsD())
        {

            for(auto i=0; i<3; ++i)
            {
                MyPos pi(&*fi, i);
                if(!pi.F()->IsF(i))
                {
                    pi.V()->C() = vcg::Color4b::Red;
                    pi.V()->SetV();
                }
                else
                {
                    if(!pi.V()->IsV())
                        pi.V()->C() = vcg::Color4b::White;
                    pi.V()->SetV();
                }
            }
        }
    tri::UpdateColor<MyMesh>::PerFaceFromVertex(m);
}
void computeCrease(MyMesh &m, float crease)
{
    for(auto fi=m.face.begin(); fi!=m.face.end(); ++fi)
        if(!(*fi).IsD())
        {
            for(auto i=0; i<3; ++i)
            {
                MyPos pi(&*fi, i);
                Point3f n0,n1;

                n0 = NormalizedTriangleNormal(*(pi.F()));
                n1 = NormalizedTriangleNormal(*(pi.FFlip()));

                float angle = math::ToDeg(AngleN(n0, n1));
                if(angle >= crease || angle <= -crease)
                {
                    pi.F()->SetCrease(i);
                    pi.FlipF();
                    pi.FFlip()->SetCrease(pi.E());

                }
            }
        }
}




class EdgeSplitPred
{
public:
    float length, min, max;
    bool adapt;
    bool operator()(MyPos &ep) const
    {
        float mult = (adapt)? lerp(1.f/10.f, 10.f, (ep.V()->Q()/(max-min))) : 1.f;
        return vcg::Distance(ep.V()->P(), ep.VFlip()->P()) > mult*length;
    }
};

int ComputeValence(MyPos &p)
{
    return p.NumberOfIncidentVertices();
}

/*
 Computes the ideal valence for the vertex in pos p:
 4 for border vertices
 6 for internal vertices
*/
int idealValence(MyPos p)
{
    if(p.IsBorder()) return 4;
    return 6;
}

float computeMeanValence(MyMesh &m)
{
    tri::UpdateFlags<MyMesh>::VertexClearV(m);
    int total = 0, count = 0, totalOff = 0;

    for(auto fi=m.face.begin(); fi!=m.face.end(); ++fi)
        if(!(*fi).IsD())
        {
            for(int i=0; i<3; ++i)
            {
                MyPos p(&*fi, i);
                if(!p.V()->IsV())
                {
                    total += p.NumberOfIncidentVertices();
                    totalOff += abs(idealValence(p)-ComputeValence(p));
                    p.V()->SetV();
                    ++count;
                }
            }
        }
    printf("counted: %d vertices\n", count);
    printf("MEAN OFFSET: %.15f\n", ((float)totalOff/(float)count));
    return (float) total / (float) count;
}
/*
    Edge Swap Step:
    This method optimizes the valence of each vertex.
    oldDist is the sum of the absolute distance of each vertex from its ideal valence
    newDist is the sum of the absolute distance of each vertex from its ideal valence after
    the edge swap.
    If the swap decreases the total absolute distance, then it's applied, preserving the triangle
    quality.                        +1
            v1                    v1
           / \                   /|\
          /   \                 / | \
         /     \               /  |  \
        /    _*p\           -1/   |   \ -1
      v2--------v0 ========> v2   |   v0
       \        /             \   |   /
        \      /               \  |  /
         \    /                 \ | /
          \  /                   \|/ +1
           v3                     v3
        Before Swap             After Swap
*/

// su trim star non fa scomparire proprio tutti i problemi sulle punte, probabilmente anche perché
// hanno angoli tali da risultare di crease...su meshlabjs non sono sicurissimo di averci visto bene
bool testSwap(MyPos p)
{
    //if border or feature, do not swap
    if(p.IsBorder() || !p.IsFaux()) return false;
    //gathering the vertices interested by this swap
    int oldDist = 0, newDist = 0, idealV, actualV;
    MyPos tp=p;
    //starting pos v0: here we decrease the edge count by 1
    MyVertex *v0=p.V();
    idealV  = idealValence(tp); actualV = ComputeValence(tp);
    oldDist += abs(idealV - actualV);
    newDist += abs(idealV - (actualV - 1));
    // pos v1: here we increment the edge count by 1
    tp.FlipF();tp.FlipE();tp.FlipV();
    MyVertex *v1=tp.V();
    idealV  = idealValence(tp); actualV = ComputeValence(tp);
    oldDist += abs(idealV - actualV);
    newDist += abs(idealV - (actualV + 1));
    // pos v2: here we decrease the edge count by 1
    tp.FlipE();tp.FlipV();tp.FlipE();
    MyVertex *v2=tp.V();
    idealV  = idealValence(tp); actualV = ComputeValence(tp);
    oldDist += abs(idealV - actualV);
    newDist += abs(idealV - (actualV - 1));
    // pos v3: here we increment the edge count by 1
    tp.FlipF();tp.FlipE();tp.FlipV();
    MyVertex *v3=tp.V();
    idealV  = idealValence(tp); actualV = ComputeValence(tp);
    oldDist += abs(idealV - actualV);
    newDist += abs(idealV - (actualV + 1));

    float qOld = std::min(Quality(v0->P(),v2->P(),v3->P()),Quality(v0->P(),v1->P(),v2->P()));
    float qNew = std::min(Quality(v0->P(),v1->P(),v3->P()),Quality(v2->P(),v3->P(),v1->P()));

    return (newDist < oldDist && qNew >= qOld * 0.50f) ||
            (newDist == oldDist && qNew >= qOld * 1.1f) || qNew > 1.5f * qOld;
}

void ImproveValence(MyMesh &m, float crease)
{
    tri::UpdateTopology<MyMesh>::FaceFace(m);
    tri::UpdateFlags<MyMesh>::FaceClearV(m);
    //feature conservative
    tri::UpdateFlags<MyMesh>::FaceFauxCrease(m, math::ToRad(crease));

    int swapCnt=0;
    for(auto fi=m.face.begin();fi!=m.face.end();++fi)
        if(!(*fi).IsD())
        {
            for(int i=0;i<3;++i)
            {
                MyPos pi(&*fi,i);

                if(!pi.FFlip()->IsV())
                    if(testSwap(pi) &&
                            face::CheckFlipEdgeNormal(*fi, i, math::ToRad(crease)) &&
                            face::CheckFlipEdge(*fi,i) )
                    {
                        face::FlipEdge(*fi,i);
                        swapCnt++;
                    }
            }
            fi->SetV();
        }
    printf("Performed %i swaps\n",swapCnt);
}

void SplitLongEdges(MyMesh &m, bool adapt, float length)
{
    tri::UpdateTopology<MyMesh>::FaceFace(m);

    Distribution<float> distr;
    tri::Stat<MyMesh>::ComputePerVertexQualityDistribution(m,distr);
    float min,max;
    max = distr.Percentile(0.9f);
    min = distr.Percentile(0.1f);

    //float length = m.bbox.Diag()/1000.f;

    tri::MidPoint<MyMesh> midFunctor(&m);
    EdgeSplitPred ep;
    ep.min = min;
    ep.max = max;
    ep.adapt = adapt;
    ep.length =(4.0f/3.0f)*length;

    //RefineE updates FF topology after doing the refine (not needed in collapse then)
    tri::RefineE(m,midFunctor,ep);
}

bool testCollapse(MyPair &p, MyCollapser &eg, float min, float max, float length, bool adapt)
{
    float mult = (adapt) ? lerp(1.f/10.f, 10.f, (((p.V(0)->Q()+p.V(1)->Q())/2.f)/(max-min))) : 1.f;
    float dist = Distance(p.V(0)->P(), p.V(1)->P());
    return  dist < mult*(4.0f/5.0f)*length &&
            !p.V(0)->IsB() && !p.V(1)->IsB() &&
            eg.LinkConditions(p);
}

void CollapseShortEdges(MyMesh &m, bool adapt, float crease, float length)
{
    tri::UpdateTopology<MyMesh>::VertexFace(m);
    //tri::UpdateTopology<MyMesh>::FaceFace(m); //FF is updated by the preceding RefineE
    tri::UpdateFlags<MyMesh>::FaceClearV(m);

    tri::UpdateFlags<MyMesh>::FaceFauxCrease(m, math::ToRad(crease));

    Distribution<float> distr;
    tri::Stat<MyMesh>::ComputePerVertexQualityDistribution(m,distr);

    float min,max;
    max = distr.Percentile(0.9f);
    min = distr.Percentile(0.1f);

    MyCollapser eg;
    for(auto fi=m.face.begin(); fi!=m.face.end(); ++fi)
        if(!(*fi).IsD())
        {
            for(auto i=0; i<3; ++i)
            {
                MyPos pi(&*fi, i);
                if(!pi.F()->IsAnyF() && !pi.FFlip()->IsAnyF() && // se collassarmi non sposta un crease vertex
                        !pi.IsBorder() &&
                        !pi.FFlip()->IsV())
                {
                    MyPair bp(pi.V(), pi.VFlip());
                    Point3f mp=(bp.V(0)->P()+bp.V(1)->P())/2.0f;
                    if(testCollapse(bp, eg, min, max, length, adapt))
                    {
                        eg.Do(m, bp, mp);
                        pi.V()->SetV();
                        break;
                    }
                }
            }
            fi->SetV();
        }
    //compact vectors, since we killed vertices
    Allocator<MyMesh>::CompactEveryVector(m);
}

int selectVertexFromCrease(MyMesh &m)
{
    int count = 0;
    for(auto fi=m.face.begin(); fi!=m.face.end(); ++fi)
        if(!(*fi).IsD())
            for(int i=0; i<3; ++i)
                if(!(*fi).IsF(i))
                {
                    MyPos p(&*fi, i);
                    p.V()->SetS();
                    p.VFlip()->SetS();
                    ++count;
                }
    return count; //count is not accurate atm
}

/*
    Simple Laplacian Smoothing step
*/
void ImproveByLaplacian(MyMesh &m)
{
    //#########PROVA CON VERTEX##################
        tri::UpdateSelection<MyMesh>::VertexFromFaceStrict(m);
        tri::UpdateFlags<MyMesh>::VertexBorderFromFaceAdj(m);
        int i1 = tri::UpdateSelection<MyMesh>::VertexFromBorderFlag(m);
        printf("sel on border %d\n", i1);
        int i2 = selectVertexFromCrease(m);
        printf("sel on crease %d\n", i2);
        int i = tri::UpdateSelection<MyMesh>::VertexInvert(m);
        tri::Smooth<MyMesh>::VertexCoordPlanarLaplacian(m,1,math::ToRad(15.f),true);
        printf("Laplacian done (selected %d)\n", i);
        tri::UpdateSelection<MyMesh>::Clear(m);
}

/*
    Reprojection step, this method reprojects each vertex on the original surface
    sampling the nearest point onto it using a uniform grid MyGrid t
*/
void ProjectToSurface(MyMesh &m, MyGrid t, FaceTmark<MyMesh> mark)
{
    face::PointDistanceBaseFunctor<float> distFunct;
    float maxDist = 100.f, minDist = 0.f;
    int cnt = 0;
    for(auto vi=m.vert.begin();vi!=m.vert.end();++vi)
        if(!(*vi).IsD())
        {
            Point3f newP;
            t.GetClosest(distFunct, mark, vi->P(), maxDist, minDist, newP);
            vi->P() = newP;
            ++cnt;
        }
    printf("projected %d\n", cnt);

}
/*
 * guarda bene come calcolare la qualità per remesh adattivo
 *
 * CONTROLLA BENE QUANDO SONO DAVVERO NEC LE CHIAMATE DI UPDATE DELLA TOPOLOGIA E FLAG
 * dai statistiche prima dopo di:
 *  valenza media     (direi media e media della distanza da ideale)
 *  qualità triangoli (dovrebbe esserci già in meshlabjs)
 * magari fallo anche come filtro a se
 *
 * aggiungi la possibilità di escludere refine (split e collapse)  edgeswap [done]
 *
 *
 * Seems good:
 *  il valence offset (reale-ideale) diminuisce monotonicamente (con soli swap)
 *  la qualità sembra aumentare (si polarizza anche un po' di più tra brutti e belli)
 *
 * Da discutere con prof:
 *  Sul bunny 5k.off con solo 1 swap succede cosa brutta brutta sotto
 *  Su laurana con 1 solo swap la mesh diventa non manifold
 *  Prima ipotesi era che avessi problemi su mesh non watertight, ma su semi1/2.ply funziona bene
 *  È un problema mio (più probabile :( ) (o delle mesh di cui sopra?)
 */
void CoarseIsotropicRemeshing(uintptr_t _baseM, int iter, bool adapt, bool refine, bool swap, float crease)
{
    MyMesh &original = *((MyMesh*) _baseM), m;

    /* Mesh cleaning (needed to support formats like .stl) */
    int dup      = tri::Clean<MyMesh>::RemoveDuplicateVertex(original);
    int unref    = tri::Clean<MyMesh>::RemoveUnreferencedVertex(original);
    int zeroArea = tri::Clean<MyMesh>::RemoveZeroAreaFace(original);

    /* Updating box before constructing the grid, otherwise we get weird results */
    original.UpdateBoxAndNormals();

    /*
     * Build a uniform grid with the orignal mesh.
     * Needed to apply the reprojection step.
     *
     */
    vcg::tri::Append<MyMesh,MyMesh>::MeshCopy(m,original);

    MyGrid t;
    t.Set(original.face.begin(), original.face.end());
    tri::FaceTmark<MyMesh> mark;
    mark.SetMesh(&original);


    // Update required topology and quality
    tri::UpdateTopology<MyMesh>::FaceFace(m);
    tri::UpdateTopology<MyMesh>::VertexFace(m);


    /* Manifold(ness) check*/
    if(tri::Clean<MyMesh>::CountNonManifoldEdgeFF(m) != 0 ||
            tri::Clean<MyMesh>::CountNonManifoldVertexFF(m) != 0)
    {
        printf("Input mesh is non-manifold, manifoldness is required!\nInterrupting filter");
        return;
    }

    // here it crashes if executed after simplification (cluster or quadric)
    tri::UpdateCurvature<MyMesh>::MeanAndGaussian(m);
    tri::UpdateQuality<MyMesh>::VertexFromAbsoluteCurvature(m);
    tri::UpdateQuality<MyMesh>::VertexSaturate(m); //do you really want this?

    //was used to debug
    //computeCrease(m);
    //MapErrorColor(m);
    //MapCreaseColor(m);

    /* Computing mean edge length (it needs the edge vector) */
    tri::UpdateTopology<MyMesh>::AllocateEdge(m);
    float length = tri::Stat<MyMesh>::ComputeEdgeLengthAverage(m);

    printf("Avg edge size: %f\n", length);
    printf("BBox dep size: %f\n", m.bbox.Diag() / 1000.f);

    printf("Initial Mean Valence: %.15f \n", computeMeanValence(m));
    for(int i=0; i < iter; ++i)
    {
        printf("iter %d \n", i+1);

        if(refine) {
            SplitLongEdges(m, adapt, length);
            CollapseShortEdges(m, adapt, crease, length);
        }

        if(swap)
            ImproveValence(m, crease);

        ImproveByLaplacian(m);
        ProjectToSurface(m, t, mark);
    }
    printf("Final Mean Valence: %.15f \n", computeMeanValence(m));

    m.UpdateBoxAndNormals();
    vcg::tri::Append<MyMesh,MyMesh>::MeshCopy(original,m);
}




void MeshingPluginTEST()
{
    printf("Meshing Plugin Test\n");
    MyMesh platonic;
    Dodecahedron(platonic);
    CutAlongCreaseFilter(uintptr_t(&platonic),40);
    assert(platonic.vn==72 && platonic.fn==60);

    Hexahedron(platonic);
    CutAlongCreaseFilter(uintptr_t(&platonic),40);
    assert(platonic.vn==24 && platonic.fn==12);
    
    Torus(platonic,3,1);
    CutTopologicalFilter(uintptr_t(&platonic));
    
    Torus(platonic,3,1);
    tri::UpdateTopology<MyMesh>::FaceFace(platonic);
    for(int i=0;i<5;++i)
    {
        int index = rand()%platonic.FN();
        face::SwapEdge(platonic.face[index],rand()%3);
    }
    
    ReorientFaceCoherently(uintptr_t(&platonic));
    
    tri::SphericalCap(platonic, 120, 1);
    HoleFilling(uintptr_t(&platonic),30);
    

    for(int i=1;i<5;++i)
    {
        MyMesh mq,mc,mv,ch;
        Torus(mq,10*i,5*i);
        Torus(mc,10*i,5*i);
        int t0=clock();
        QuadricSimplification(uintptr_t(&mq),0.5f,0,true,false);
        int t1=clock();
        printf("Quadric    simplification in  %6.3f sec\n",float(t1-t0)/CLOCKS_PER_SEC);
        ClusteringSimplification(uintptr_t(&mc),0.01f);
        int t2=clock();
        printf("Clustering simplification in  %6.3f sec\n",float(t2-t1)/CLOCKS_PER_SEC);
        ConvexHullFilter(uintptr_t(&mc),uintptr_t(&ch));
        int t3=clock();
        printf("Computed Convex Hull %i %i -> %i %i  in  %6.3f sec\n",mc.vn,mc.fn,ch.vn,ch.fn,float(t3-t2)/CLOCKS_PER_SEC);
        VoronoiClustering(uintptr_t(&mc),uintptr_t(&ch),0.01*i, i%2, 3,0,i,i);
        int t4=clock();
        printf("Voronoi Clustering %i %i -> %i %i  in  %6.3f sec\n",mc.vn,mc.fn,ch.vn,ch.fn,float(t4-t3)/CLOCKS_PER_SEC);
    }
}


#ifdef __EMSCRIPTEN__
//Binding code
EMSCRIPTEN_BINDINGS(MLMeshingPlugin) {
    emscripten::function("ConvexHullFilter",           &ConvexHullFilter);
    emscripten::function("QuadricSimplification",      &QuadricSimplification);
    emscripten::function("ClusteringSimplification",   &ClusteringSimplification);
    emscripten::function("RemoveUnreferencedVertices", &RemoveUnreferencedVertices);
    emscripten::function("RemoveDuplicatedVertices",   &RemoveDuplicatedVertices);
    emscripten::function("InvertFaceOrientation",      &InvertFaceOrientation);
    emscripten::function("ReorientFaceCoherently",     &ReorientFaceCoherently);
    emscripten::function("VoronoiClustering",          &VoronoiClustering);
    emscripten::function("CutAlongCreaseFilter",       &CutAlongCreaseFilter);
    emscripten::function("CutTopologicalFilter",       &CutTopologicalFilter);
    emscripten::function("HoleFilling",                &HoleFilling);
    emscripten::function("CoarseIsotropicRemeshing",   &CoarseIsotropicRemeshing);
}
#endif

