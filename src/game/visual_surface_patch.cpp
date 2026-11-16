#include "game/visual_surface_patch.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace hailstone::game {
namespace {
using Point=glm::dvec2;
using Polygon=std::vector<Point>;
constexpr std::size_t kMaximumPieces=2048,kMaximumCorners=32;
struct Plane {
    double x=0,y=0,z=0;
    double at(Point p) const{return x*p.x+y*p.y+z;}
};
bool finite(glm::vec3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
double cross(Point a,Point b){return a.x*b.y-a.y*b.x;}
double area(const Polygon& p){
    double twice=0;
    // Convex polygons: a fan about their own first corner avoids subtracting large almost-equal world-space
    // products for a very narrow intersection near the edge of a large producer triangle.
    for(std::size_t n=1;n+1<p.size();++n)twice+=cross(p[n]-p[0],p[n+1]-p[0]);
    return twice*.5;
}
Point center(const Polygon& p){Point sum(0);for(const auto v:p)sum+=v;return sum/static_cast<double>(p.size());}
Plane through(const std::array<glm::vec3,3>& vertices,Point origin){
    const auto a=glm::dvec3(vertices[0]),b=glm::dvec3(vertices[1]),c=glm::dvec3(vertices[2]);
    const auto ab=b-a,ac=c-a;
    const double determinant=ab.x*ac.y-ab.y*ac.x;
    Plane out;
    out.x=(ab.z*ac.y-ab.y*ac.z)/determinant;
    out.y=(ab.x*ac.z-ab.z*ac.x)/determinant;
    out.z=a.z-out.x*(a.x-origin.x)-out.y*(a.y-origin.y);
    return out;
}
class Check {
public:
    Check(std::uint32_t& work):work_(work){}
    bool spend(){if(work_==0){exhausted_=true;return false;}--work_;return true;}
    void uncertain(){exhausted_=true;}
    bool failed() const{return exhausted_;}
    // Keep line.x*x+line.y*y+line.z >= 0. Translation to the query origin avoids cancellation at large world XY.
    Polygon clip(const Polygon& source,Plane line){
        if(source.empty()||!spend())return {};
        Polygon out;
        auto previous=source.back();double old=line.at(previous);
        for(const auto current:source){
            const double now=line.at(current);
            if((old>=0)!=(now>=0)){
                const double t=std::clamp(old/(old-now),0.0,1.0);
                auto intersection=previous+(current-previous)*t;
                // Keep a boundary contact on that exact constraint. Interpolation alone can leave it a few
                // double ULPs across a horizontal stair edge and invent a positive-area overlap with its riser.
                if(std::abs(line.x)>std::abs(line.y))intersection.x=(-line.y*intersection.y-line.z)/line.x;
                else if(line.y!=0)intersection.y=(-line.x*intersection.x-line.z)/line.y;
                out.push_back(intersection);
            }
            if(now>=0)out.push_back(current);
            previous=current;old=now;
        }
        out.erase(std::unique(out.begin(),out.end(),[](Point a,Point b){return glm::dot(a-b,a-b)<1e-24;}),out.end());
        if(out.size()>1&&glm::dot(out.front()-out.back(),out.front()-out.back())<1e-24)out.pop_back();
        if(out.size()>kMaximumCorners){exhausted_=true;return {};}
        if(out.size()<3||area(out)==0)return {};
        // Preserve every positive-area intermediate, including tiny pieces at shared face edges. Later faces
        // can cover them exactly; an uncovered piece becomes a double REGION hint, never a false planar proof.
        return out;
    }
    Polygon intersection(Polygon source,const Polygon& convex){
        for(std::size_t n=0;n<convex.size()&&!source.empty()&&!failed();++n){
            const auto a=convex[n],edge=convex[(n+1)%convex.size()]-a;
            source=clip(source,{-edge.y,edge.x,edge.y*a.x-edge.x*a.y});
        }
        return source;
    }
    bool subtract(std::vector<Polygon>& remaining,const Polygon& covered){
        std::vector<Polygon> next;
        for(auto inside:remaining){
            for(std::size_t n=0;n<covered.size()&&!inside.empty();++n){
                const auto a=covered[n],edge=covered[(n+1)%covered.size()]-a;
                const Plane line{-edge.y,edge.x,edge.y*a.x-edge.x*a.y};
                auto outside=clip(inside,{-line.x,-line.y,-line.z});
                if(!outside.empty())next.push_back(std::move(outside));
                inside=clip(inside,line);
                if(failed()||next.size()>kMaximumPieces){exhausted_=true;return false;}
            }
        }
        remaining=std::move(next);return true;
    }
private:
    std::uint32_t& work_;
    bool exhausted_=false;
};
} // namespace

VisualSurfacePatchResult certify_visual_surface_patch(const VisualSurfacePatch& query,const VisualTriangleSource& source,
                                                       const VisualFloorProbe& floor,std::uint32_t& work){
    VisualSurfacePatchResult out;
    const auto deferred=[](const char* reason){VisualSurfacePatchResult result;result.diagnostic=reason;return result;};
    if(!source||!floor||!std::isfinite(query.maximum_rise)||query.maximum_rise<0||
       !std::isfinite(query.tolerance)||query.tolerance<0)return out;
    for(const auto v:query.vertices)if(!finite(v))return out;
    const Point origin(query.vertices[0]);
    Polygon triangle;
    for(const auto v:query.vertices)triangle.push_back(Point(v)-origin);
    if(std::abs(area(triangle))*2<=1e-8)return out; // same twice-area contract as the SDK admission check
    if(area(triangle)<0)std::reverse(triangle.begin(),triangle.end());
    const auto reference=through(query.vertices,origin);
    std::vector<Polygon> remaining{triangle};
    Check check(work);
    std::optional<Point> witness;
    Polygon witness_region;
    glm::vec2 low(query.vertices[0]),high=low;
    for(const auto v:query.vertices){low=glm::min(low,glm::vec2(v));high=glm::max(high,glm::vec2(v));}
    const bool completed=source(low,high,[&](const CollisionTriangle& candidate){
        if(!check.spend())return false;
        if(!(candidate.normal.z>0)||
           (candidate.flags&(kTriangleTransparent|kTriangleInvisible|kTriangleNoVisualSurface|kTriangleLadderSlab|0x10)))return true;
        const std::array<glm::vec3,3> vertices{candidate.v0,candidate.v1,candidate.v2};
        for(const auto v:vertices)if(!finite(v)){check.uncertain();return false;}
        Polygon footprint;
        for(const auto v:vertices)footprint.push_back(Point(v)-origin);
        if(std::abs(area(footprint))<1e-16){check.uncertain();return false;}
        if(area(footprint)<0)std::reverse(footprint.begin(),footprint.end());
        auto overlap=check.intersection(footprint,triangle);
        if(check.failed())return false;
        if(overlap.empty())return true;
        const auto plane=through(vertices,origin);
        const Plane difference{plane.x-reference.x,plane.y-reference.y,plane.z-reference.z};
        overlap=check.clip(overlap,{-difference.x,-difference.y,query.maximum_rise-difference.z});
        if(check.failed())return false;
        if(overlap.empty())return true; // distant overhead surface is outside the requested layer
        const auto above=check.clip(overlap,{difference.x,difference.y,difference.z-query.tolerance});
        if(check.failed())return false;
        if(!above.empty()){
            const auto at=center(above);
            // A boundary-only contact is coplanar within tolerance, not an interior mismatch.
            if(difference.at(at)>query.tolerance){witness=at;witness_region=above;return false;}
        }
        // Subtract the original source footprint, not the query-clipped intersection polygon. Reusing the latter
        // reconstructs the producer's own edges with rounded intersections and can invent an uncovered edge sliver.
        auto covered=check.clip(footprint,{-difference.x,-difference.y,query.maximum_rise-difference.z});
        covered=check.clip(covered,{-difference.x,-difference.y,query.tolerance-difference.z});
        covered=check.clip(covered,{difference.x,difference.y,difference.z+query.tolerance});
        if(check.failed())return false;
        if(!covered.empty()&&!remaining.empty()&&!check.subtract(remaining,covered))return false;
        return true;
    },work);
    if(check.failed()||(!completed&&!witness))return deferred("first coverage clipping incomplete");
    if(!witness&&remaining.empty()){out.status=VisualSurfacePatchStatus::Planar;return out;}
    if(!witness){witness_region=remaining.front();witness=center(witness_region);}
    if(!check.spend())return out;
    const Point region_origin=witness_region.front()+origin;
    Point relative_sum(0);
    for(const auto point:witness_region)relative_sum+=(point+origin)-region_origin;
    const Point xy=region_origin+relative_sum/static_cast<double>(witness_region.size());
    *witness=xy-origin; // same origin-relative centroid arithmetic as SDK hint validation
    out.witness={static_cast<float>(xy.x),static_cast<float>(xy.y),0};
    // A thin region's double centroid can round onto a different float layer. Resolve the highest actual face at
    // the proven double location; float witness XY is only its approximation when a REGION hint is supplied.
    std::optional<double> highest;
    const double ceiling=reference.at(*witness)+query.maximum_rise;
    const glm::vec2 low_point{std::nextafter(out.witness.x,-std::numeric_limits<float>::infinity()),
                              std::nextafter(out.witness.y,-std::numeric_limits<float>::infinity())};
    const glm::vec2 high_point{std::nextafter(out.witness.x,std::numeric_limits<float>::infinity()),
                               std::nextafter(out.witness.y,std::numeric_limits<float>::infinity())};
    const bool found_all=source(low_point,high_point,[&](const CollisionTriangle& face){
            if(!check.spend())return false;
            if(!(face.normal.z>0)||
               (face.flags&(kTriangleTransparent|kTriangleInvisible|kTriangleNoVisualSurface|kTriangleLadderSlab|0x10)))return true;
            const Point a(face.v0),b(face.v1),c(face.v2);
            const double d=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);
            if(!std::isfinite(d)||std::abs(d)<1e-12)return true;
            const double u=((b.y-c.y)*(xy.x-c.x)+(c.x-b.x)*(xy.y-c.y))/d;
            const double v=((c.y-a.y)*(xy.x-c.x)+(a.x-c.x)*(xy.y-c.y))/d;
            const double w=1-u-v;
            if(!(u>=0&&v>=0&&w>=0))return true;
            const double z=u*face.v0.z+v*face.v1.z+w*face.v2.z;
            if(std::isfinite(z)&&z<=ceiling&&(!highest||z>*highest)){
                highest=z;out.source_face={face.v0,face.v1,face.v2};
            }
            return true;
    },work);
    if(!found_all||check.failed())return deferred("highest-face resolution incomplete");
    if(highest){
        out.status=VisualSurfacePatchStatus::Mismatch;out.witness.z=static_cast<float>(*highest);out.height_known=true;
        out.source_face_known=true;
        // Refine a whole actual highest-face domain, rather than repeatedly cutting a tolerance contour through
        // a sloping candidate plane. Remove every higher bounded face so a lower floor never overwrites a prop.
        const auto selected=through(out.source_face,origin);
        Polygon footprint;for(const auto vertex:out.source_face)footprint.push_back(Point(vertex)-origin);
        if(area(footprint)<0)std::reverse(footprint.begin(),footprint.end());
        auto domain_polygon=check.intersection(std::move(footprint),triangle);
        domain_polygon=check.clip(domain_polygon,{reference.x-selected.x,reference.y-selected.y,
                                                  reference.z+query.maximum_rise-selected.z});
        if(check.failed()||domain_polygon.empty())return deferred("selected domain empty or uncertain");
        std::vector<Polygon> domain{std::move(domain_polygon)};
        const double hint_tolerance=std::max(1e-10,static_cast<double>(query.tolerance)*.05);
        const bool domain_complete=source(low,high,[&](const CollisionTriangle& face){
            if(!check.spend())return false;
            if(!(face.normal.z>0)||
               (face.flags&(kTriangleTransparent|kTriangleInvisible|kTriangleNoVisualSurface|kTriangleLadderSlab|0x10)))return true;
            const std::array<glm::vec3,3> vertices{face.v0,face.v1,face.v2};
            const auto plane=through(vertices,origin);
            const Plane above_selected{plane.x-selected.x,plane.y-selected.y,plane.z-selected.z-hint_tolerance};
            Polygon cover;for(const auto vertex:vertices)cover.push_back(Point(vertex)-origin);
            if(area(cover)<0)std::reverse(cover.begin(),cover.end());
            if(std::none_of(cover.begin(),cover.end(),[&](Point point){return above_selected.at(point)>0;}))return true;
            cover=check.clip(cover,{reference.x-plane.x,reference.y-plane.y,reference.z+query.maximum_rise-plane.z});
            cover=check.clip(cover,above_selected);
            if(check.failed())return false;
            return cover.empty()||check.subtract(domain,cover);
        },work);
        if(!domain_complete||check.failed())return deferred("higher-face subtraction incomplete");
        const auto contains=[&](const Polygon& polygon){
            for(std::size_t n=0;n<polygon.size();++n)
                if(cross(polygon[(n+1)%polygon.size()]-polygon[n],*witness-polygon[n])<-1e-10)return false;
            return true;
        };
        const auto piece=std::find_if(domain.begin(),domain.end(),contains);
        if(piece==domain.end())return deferred("witness outside selected-face domain");
        witness_region=*piece;
        const auto first=witness_region.front()+origin;
        Point sum(0);for(const auto point:witness_region)sum+=(point+origin)-first;
        const auto location=first+sum/static_cast<double>(witness_region.size());
        out.witness={static_cast<float>(location.x),static_cast<float>(location.y),
                     static_cast<float>(selected.at(location-origin))};
    }else out.status=VisualSurfacePatchStatus::Missing;
    if(witness_region.size()>out.region.size())return {};
    out.region_count=static_cast<std::uint32_t>(witness_region.size());
    for(std::size_t n=0;n<witness_region.size();++n)out.region[n]=witness_region[n]+origin;
    return out;
}
} // namespace hailstone::game
