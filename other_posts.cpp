Hello uc! yall good? im fine, today im releasing a source ive been working on for some time, its self explanatory, the only thing is you will need to figure out vpk parsing and collision checks yourself (this isnt a pasta pasta ).

I have been working on one with bounce detection and calculation, its pretty inaccurate and i think impossible as of now, i think i need to do smth with my normal vector method or smth.

If you like this post consider doing +rep to me

Code:
 
struct Sphere {
    Vec3 center{};
    float radius{};
};
 
Sphere grenadecalc;
std::mutex predmtx;
 
inline Vec3 AngleToDirection(const QAngle& angles)
{
	Vec3 direction;
 
	// Transform degrees into radians
	float pitch = angles.pitch * (M_PI / 180.0f);
	float yaw = angles.yaw * (M_PI / 180.0f);
 
	float cosPitch = cosf(pitch);
	direction.x = cosf(yaw) * cosPitch;
	direction.y = sinf(yaw) * cosPitch;
	direction.z = -sinf(pitch);  // Negative becos pitch in inverted in source/source2 games
 
	return direction;
}
 
// This entire [removed] took around 2.5 weeks to figure out LMFAOO
Vec3 CalculateThrowVelocity(const QAngle& viewAngles, float throwStrength, const Vec3& playerVelocity) {
    QAngle adjustedAngles = viewAngles;
 
    // Normalize pitch angle to avoid excessive throw angles (yes, i did this because ive had problems with it)
    if (adjustedAngles.pitch < -89.f) {
        adjustedAngles.pitch += 360.f;
    }
    else if (adjustedAngles.pitch > 89.f) {
        adjustedAngles.pitch -= 360.f;
    }
 
    // Adjust pitch angle for adjusted arc, 
    // because if not the direction will not be the actual direction where the grenade is thrown
    adjustedAngles.pitch -= (90.f - std::fabsf(adjustedAngles.pitch)) * 10.f / 90.f;
 
    // Convert angle to direction vector
    Vec3 direction = AngleToDirection(adjustedAngles);
 
    // This was adjusted from a internal grenade prediction from CSGO and changed A LOT from the original
    Vec3 throwVelocity = direction * (throwStrength * 0.7f + 0.3f) * 1090.0f; // The 1090 constant was obtained by testing for 3 hours straight lmfao
 
    // This will give you the throw velocity with the players velocity so jumpthrow / runthrow
    throwVelocity += playerVelocity * 1.25f;
 
    return throwVelocity;
}
 
// This is basic oblique throw, 
std::vector<Vec3> SimulateGrenadeTrajectory(const Vec3& startPos, const Vec3& velocity) {
    Vec3 currentPos = startPos;
    Vec3 currentVel = velocity;
 
    std::vector<Vec3> path;
    path.reserve(100);  // Reserve memory for efficiency
 
    grenadecalc.radius = 5.0f; // radius of collision
 
    constexpr int INTERPOLATION_STEPS = 10; // amount of interp positions between collision
 
    for (int step = 0; step < MAX_SIMULATION_STEPS; ++step) {
        path.emplace_back(currentPos); // emplace current pos
 
        // Apply gravity (For people who are think of minecraft coordinates which paste all the time z is y axis)
        currentVel.z -= CS_GRAVITY * CS_TICK_INTERVAL;
 
        // Calculate next position
        Vec3 nextPos = currentPos + (currentVel * CS_TICK_INTERVAL);
 
        // Interpolate to detect missed collisions
        for (int i = 1; i <= INTERPOLATION_STEPS; ++i) {
            float t = static_cast<float>(i) / INTERPOLATION_STEPS;
            Vec3 interpolatedPos = currentPos + (nextPos - currentPos) * t; // Interpolate positions to get a accurate point of collision
 
            grenadecalc.center = interpolatedPos;
 
            if (map.tracehullsphere(grenadecalc)) { // vpk parsing, figure out yourself
                path.emplace_back(interpolatedPos);  // Save collision pos
                return path;  // Stop if collision
            }
        }
 
        // Move to next pos if no collision occurred
        currentPos = nextPos;
    }
 
    return path;
}
 
// What do you expect.
void DrawPredictedTrajectory(const std::vector<Vec3>& path) {
    auto* drawList = ImGui::GetBackgroundDrawList();
 
    for (size_t i = 1; i < path.size(); ++i) {
        Vec2 screenPos1, screenPos2;
 
        if (w2s(path[i - 1], screenPos1, current__vm, SCW, SCH) &&
            w2s(path[i], screenPos2, current__vm, SCW, SCH)) {
 
            drawList->AddLine(
                ImVec2(screenPos1.x, screenPos1.y),
                ImVec2(screenPos2.x, screenPos2.y),
                IM_COL32(0, 255, 0, 255)  // Green for trajectory
            );
        }
    }
}
 
traj1 traj;
 
// Main grenade prediction loop
void GrenadePred() {
    while (true) {
        if (lp.currweapid == WEAPON_HEGRENADE ||
            lp.currweapid == WEAPON_MOLOTOV ||
            lp.currweapid == WEAPON_INCGRENADE ||
            lp.currweapid == WEAPON_FLASHBANG ||
            lp.currweapid == WEAPON_SMOKEGRENADE) // check if have grenade
        {
            // Check if you are pulling the pin
            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) || ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) && (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) /*driver->RPM<float>(lp.currweapid + cs2_dumper::schemas::client_dll::C_BaseCSGrenade::m_fThrowTime) == 0.f*/)
            {
 
                float throw_strength = driver->RPM<float>(
                    lp.weaponptr + cs2_dumper::schemas::client_dll::C_BaseCSGrenade::m_flThrowStrength); // Get throw strength
                // weaponptr is m_pClippingWeapon
 
                std::lock_guard<std::mutex> lock(predmtx);
 
                // Calculate initial velocity and position
                Vec3 playerVelocity = driver->RPM<Vec3>(lp.Pawn + cs2_dumper::schemas::client_dll::C_BaseEntity::m_vecVelocity); // Get player vel
                traj.m_vInitialVelocity = CalculateThrowVelocity(lp.ViewAngles, throw_strength, playerVelocity); // Calc vel
                traj.m_vInitialPosition = lp.Origin.pos + driver->RPM<Vec3>(lp.Pawn + cs2_dumper::schemas::client_dll::C_BaseModelEntity::m_vecViewOffset); // set initial pos
 
                // Predict the trajectory
                traj.predictedt = SimulateGrenadeTrajectory(traj.m_vInitialPosition, traj.m_vInitialVelocity); // Get predicted traj
            }
        }
 
        sleepMicroseconds(1000LL);
    }
}
 
// this two things go in your esp loop
std::lock_guard<std::mutex> lock(predmtx);
 
DrawPredictedTrajectory(traj.predictedt);



//------------------------------------------------------------------------



The only thing i know for know is until i hit a wall (if you want a hint on the collision i will give the source at the end of the text) Still trying to figure it out. Even tried reversing the own's game prediction and grabbing the cs source game's prediction already reversed to see constant and [removed], the only things ive learned are you need to know the angle of incidence and see exactly in which timestep it collides with a geometry of the map, grab that triangle and get the relative angle of incidence ( the angle between the direction of motion of the wave and a line drawn perpendicular to the reflecting boundary ) To the triangle, based on that and the coeficient of restitution ( amount of elasticity applied to the object in a bounce, any bounce ).

Bounce detection code (Uses a KD-Tree for opts, fix it for yourself):
Code:
struct BoundingBox {
    Vec3 min, max;
 
    bool intersect(const Vec3& ray_origin, const Vec3& ray_end) const { //Slabs method
        Vec3 dir = ray_end - ray_origin;
        dir = dir.Normalize(); 
 
        float t1 = (min.x - ray_origin.x) / dir.x;
        float t2 = (max.x - ray_origin.x) / dir.x;
        float t3 = (min.y - ray_origin.y) / dir.y;
        float t4 = (max.y - ray_origin.y) / dir.y;
        float t5 = (min.z - ray_origin.z) / dir.z;
        float t6 = (max.z - ray_origin.z) / dir.z;
 
        float tmin1 = (t1 < t2) ? t1 : t2;
        float tmin2 = (t3 < t4) ? t3 : t4;
        float tmin3 = (t5 < t6) ? t5 : t6;
 
        float tmax1 = (t1 > t2) ? t1 : t2;
        float tmax2 = (t3 > t4) ? t3 : t4;
        float tmax3 = (t5 > t6) ? t5 : t6;
 
        float tmin = (tmin1 > tmin2) ? ((tmin1 > tmin3) ? tmin1 : tmin3) : ((tmin2 > tmin3) ? tmin2 : tmin3);
        float tmax = (tmax1 < tmax2) ? ((tmax1 < tmax3) ? tmax1 : tmax3) : ((tmax2 < tmax3) ? tmax2 : tmax3);
 
        
        if (tmax < 0) {
            return false;
        }
 
     
        if (tmin > tmax) {
            return false;
        }
 
        return true;
    }
 
    // New method to check if a sphere intersects the bounding box
    bool intersectsSphere(const Vec3& sphereCenter, float sphereRadius) const {
        // Find the closest point on the bounding box to the sphere center
        float closestX = (((min.x) > ((((sphereCenter.x) < (max.x)) ? (sphereCenter.x) : (max.x)))) ? (min.x) : ((((sphereCenter.x) < (max.x)) ? (sphereCenter.x) : (max.x))));
        float closestY = (((min.y) > ((((sphereCenter.y) < (max.y)) ? (sphereCenter.y) : (max.y)))) ? (min.y) : ((((sphereCenter.y) < (max.y)) ? (sphereCenter.y) : (max.y))));
        float closestZ = (((min.z) > ((((sphereCenter.z) < (max.z)) ? (sphereCenter.z) : (max.z)))) ? (min.z) : ((((sphereCenter.z) < (max.z)) ? (sphereCenter.z) : (max.z))));
 
        // Compute the distance between the sphere's center and the closest point
        Vec3 closestPoint(closestX, closestY, closestZ);
        Vec3 diff = closestPoint - sphereCenter;
        float distSq = diff.LengthSq();  // Use squared distance to avoid unnecessary sqrt
 
        // If the distance is less than or equal to the radius, the sphere intersects the box
        return distSq <= sphereRadius * sphereRadius;
    }
};
 
struct Triangle {
    Vec3 p1, p2, p3;
 
    bool intersect(Vec3 ray_origin, Vec3 ray_end) const {
        const float EPSILON = 0.0000001f;
        Vec3 edge1, edge2, h, s, q;
        float a, f, u, v, t;
        edge1 = p2 - p1;
        edge2 = p3 - p1;
        h = CrossProduct(ray_end - ray_origin, edge2);
        a = edge1.Dot(h);
 
        if (a > -EPSILON && a < EPSILON)
            return false;
 
        f = 1.0 / a;
        s = ray_origin - p1;
        u = f * s.Dot(h);
 
        if (u < 0.0 || u > 1.0)
            return false;
 
        q = CrossProduct(s, edge1);
        v = f * (ray_end - ray_origin).Dot(q);
 
        if (v < 0.0 || u + v > 1.0)
            return false;
 
 
        t = f * edge2.Dot(q);
 
        if (t > EPSILON && t < 1.0)
            return true;
 
        return false;
    }
 
    bool intersectwithreturn(const Vec3& ray_origin, const Vec3& ray_end, Vec3& currentIntersection) const {
        const float EPSILON = 0.0000001f;
        Vec3 edge1, edge2, h, s, q;
        float a, f, u, v, t;
 
        edge1 = p2 - p1;
        edge2 = p3 - p1;
        h = CrossProduct(ray_end - ray_origin, edge2);
        a = edge1.Dot(h);
 
        if (a > -EPSILON && a < EPSILON) {
            return false; // Ray is parallel to the triangle
        }
 
        f = 1.0 / a;
        s = ray_origin - p1;
        u = f * s.Dot(h);
 
        if (u < 0.0 || u > 1.0) {
            return false; // Intersection is outside the triangle
        }
 
        q = CrossProduct(s, edge1);
        v = f * (ray_end - ray_origin).Dot(q);
 
        if (v < 0.0 || u + v > 1.0) {
            return false; // Intersection is outside the triangle
        }
 
        t = f * edge2.Dot(q);
 
        if (t > EPSILON && t < 1.0) {
            // Calculate the intersection point
            currentIntersection = ray_origin + (ray_end - ray_origin) * t; // Compute the intersection point, this is wrong, dont use it
            return true; // Intersection occurred
        }
 
        return false; // No intersection
    }
};
 
// Sphere structure
struct Sphere {
    Vec3 center;
    float radius;
};
 
 
struct KDNode {
    BoundingBox bbox;
    std::vector<Triangle> triangle;
    KDNode* left, * right = nullptr;
    int axis;
 
    void deleteKDTree(KDNode* node) {
        if (node == nullptr) return;
 
        
        deleteKDTree(node->left);
        deleteKDTree(node->right);
 
        
        delete node;
    }
};
 
// Helper function to find the closest point on an edge (line segment) to a given point
void updateClosestPoint(const Vec3& point, const Vec3& a, const Vec3& b, Vec3& closestPoint, float& minDistSq) {
    // Vector from a to b (edge)
    Vec3 ab = b - a;
 
    // Project point onto the line segment (a, b) and clamp the result between 0 and 1
    float t = (((0.0f) > ((((1.0f) < ((point - a).Dot(ab) / ab.LengthSq())) ? (1.0f) : ((point - a).Dot(ab) / ab.LengthSq())))) ? (0.0f) : ((((1.0f) < ((point - a).Dot(ab) / ab.LengthSq())) ? (1.0f) : ((point - a).Dot(ab) / ab.LengthSq()))));
 
    // Projection point on the edge
    Vec3 projection = a + ab * t;
 
    // Calculate the squared distance from point to the projection
    float distSq = (point - projection).LengthSq();
 
    // Update closest point and minimum distance if this projection is closer
    if (distSq < minDistSq) {
        minDistSq = distSq;
        closestPoint = projection;
    }
}
 
// Corrected function to find the closest point on a triangle to a given point
Vec3 closestPointOnTriangle(const Vec3& point, const Triangle& triangle, float& u, float& v, float& w) {
    Vec3 edge1 = triangle.p2 - triangle.p1;
    Vec3 edge2 = triangle.p3 - triangle.p1;
 
    // Calculate normal of the triangle plane
    Vec3 normal = CrossProduct(edge1, edge2).Normalize();
 
    // Calculate distance of the point to the triangle plane
    Vec3 toPoint = point - triangle.p1;
    float planeDistance = toPoint.Dot(normal);
 
    // Project the point onto the triangle plane
    Vec3 projectedPoint = point - normal * planeDistance;
 
    // Calculate the barycentric coordinates of the projected point
    Vec3 v0 = triangle.p2 - triangle.p1;
    Vec3 v1 = triangle.p3 - triangle.p1;
    Vec3 v2 = projectedPoint - triangle.p1;
 
    float d00 = v0.Dot(v0);
    float d01 = v0.Dot(v1);
    float d11 = v1.Dot(v1);
    float d20 = v2.Dot(v0);
    float d21 = v2.Dot(v1);
 
    float denom = d00 * d11 - d01 * d01;
    if (std::fabs(denom) < 1e-8) {
        // Degenerate triangle, fallback
        return triangle.p1;
    }
 
    v = (d11 * d20 - d01 * d21) / denom;
    w = (d00 * d21 - d01 * d20) / denom;
    u = 1.0f - v - w;
 
    // Ensure the point is within the triangle
    if (u >= 0.0f && v >= 0.0f && w >= 0.0f) {
        return triangle.p1 * u + triangle.p2 * v + triangle.p3 * w;
    }
 
    // If outside the triangle, find the closest point on the edges
    Vec3 closestPoint = triangle.p1;
    float minDistSq = (point - triangle.p1).LengthSq();
 
    // Correct usage: passing the point, triangle vertices, and references to closestPoint and minDistSq
    updateClosestPoint(point, triangle.p1, triangle.p2, closestPoint, minDistSq);
    updateClosestPoint(point, triangle.p2, triangle.p3, closestPoint, minDistSq);
    updateClosestPoint(point, triangle.p3, triangle.p1, closestPoint, minDistSq);
 
    return closestPoint;
}
 
// Function to check if a sphere intersects a triangle
bool sphereIntersectsTriangle(KDNode* node, const Sphere& sphere, const Triangle& triangle) {
    // Check if the sphere's center is inside the triangle's plane
    Vec3 edge1 = triangle.p2 - triangle.p1;
    Vec3 edge2 = triangle.p3 - triangle.p1;
    Vec3 normal = CrossProduct(edge1, edge2).Normalize();
    float distanceToPlane = (sphere.center - triangle.p1).Dot(normal);
 
    if (std::fabs(distanceToPlane) > sphere.radius) {
        return false; // Sphere center is too far from the triangle plane
    }
 
    // Project sphere center onto the triangle plane
    Vec3 projectedCenter = sphere.center - normal * distanceToPlane;
 
    // Find the closest point on the triangle to the projected sphere center
    float u, v, w;
    Vec3 closestPoint = closestPointOnTriangle(projectedCenter, triangle, u, v, w);
 
    // Check if the closest point is within the sphere's radius
    float distanceToClosestPoint = (closestPoint - sphere.center).Length();
    return distanceToClosestPoint <= sphere.radius;
}
 
// Updated function for KD-Tree intersection with sphere
bool sphereIntersectsKDTree(KDNode* node, const Sphere& sphere) {
    if (node == nullptr) return false;
 
    // Check if the sphere intersects with the bounding box of the node
    if (!node->bbox.intersectsSphere(sphere.center, sphere.radius)) {
        return false;
    }
 
    // Check intersection with triangles in the node
    for (const auto& tri : node->triangle) {
        if (sphereIntersectsTriangle(node, sphere, tri)) {
            return true;
        }
    }
 
    // Recur for left and right children
    bool hitLeft = sphereIntersectsKDTree(node->left, sphere);
    bool hitRight = sphereIntersectsKDTree(node->right, sphere);
 
    return hitLeft || hitRight;
}
 
bool tracehullsphere(Sphere sphere) 
{
    return sphereIntersectsKDTree(kd_tree, sphere);
}



//------------------------------------------------------------------------


Will use boxes then, no problem, i do have the vpks of each map parsed, i have each triangle and collision of the map, i think BSPs no longer exist in cs2, i'll take a look at
CBaseCSGrenadeProjectile::ResolveFlyCollisionCustom in a bit.

By the way, where is that class in the cstrike15 src?

Found something interesting, found friction constant and bbox defs

Code:
Base Handling for all the player's grenades
 
*/
#include "cbase.h"
#include "grenadethrown.h"
#include "ammodef.h"
#include "vstdlib/random.h"
 
// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"
 
// Precaches a grenade and ensures clients know of it's "ammo"
void UTIL_PrecacheOtherGrenade( const char *szClassname )
{
	CBaseEntity *pEntity = CreateEntityByName( szClassname );
	if ( !pEntity )
	{
		Msg( "NULL Ent in UTIL_PrecacheOtherGrenade\n" );
		return;
	}
	
	CThrownGrenade *pGrenade = dynamic_cast<CThrownGrenade *>( pEntity );
 
	if (pGrenade)
	{
		pGrenade->Precache( );
	}
 
	UTIL_Remove( pEntity );
}
 
//-----------------------------------------------------------------------------
// Purpose: Setup basic values for Thrown grens
//-----------------------------------------------------------------------------
void CThrownGrenade::Spawn( void )
{
	// point sized, solid, bouncing
	SetMoveType( MOVETYPE_FLYGRAVITY, MOVECOLLIDE_FLY_BOUNCE );
	SetSolid( SOLID_BBOX );
	UTIL_SetSize(this, vec3_origin, vec3_origin);
 
	// Movement
	SetGravity( UTIL_ScaleForGravity( 648 ) );
	SetFriction(0.6);
	QAngle angles;
	VectorAngles( GetAbsVelocity(), angles );
	SetLocalAngles( angles );
	QAngle vecAngVel( random->RandomFloat ( -100, -500 ), 0, 0 );
	SetLocalAngularVelocity( vecAngVel );
	
	SetTouch( &CThrownGrenade::BounceTouch );
}
 
//-----------------------------------------------------------------------------
// Purpose: Throw the grenade.
// Input  : vecOrigin - Starting position
//			vecVelocity - Starting velocity
//			flExplodeTime - Time at which to detonate
//-----------------------------------------------------------------------------
void CThrownGrenade::Thrown( Vector vecOrigin, Vector vecVelocity, float flExplodeTime )
{
	// Throw
	SetLocalOrigin( vecOrigin );
	SetAbsVelocity( vecVelocity );
 
	// Explode in 3 seconds
	SetThink( &CThrownGrenade::Detonate );
	SetNextThink( gpGlobals->curtime + flExplodeTime );
}


//------------------------------------------------------------------------


This signature is from a server.dll from around a month back so it might not work on the newest one
Code:
server.dll @ 48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC 40 48 8B 6C 24
Here is a very lightly reversed dissam for you as well:
Code:
180227e10  struct CHEGrenadeProjectile* CHEGrenadeProjectile::Create(struct CHEGrenadeProjectile* pProjectile_1, int32_t* arg2, int64_t* arg3, int64_t* arg4, int16_t arg5)
 
180227e10  {
180227e24      int64_t* rbp = arg4;
180227e44      struct CHEGrenadeProjectile* pGrenade;
180227e44      int32_t zmm0;
180227e44      int128_t zmm6;
180227e44      pGrenade = CBaseEntity::Create("hegrenade_projectile", pProjectile_1, arg2, rbp);
180227e57      CBaseCSGrenadeProjectile::SetDetonateTimerLength(pGrenade, 1.5, zmm0, zmm6);
180227e62      int128_t zmm6_1 = sub_18023f810(pGrenade, arg3);
180227e70      int512_t zmm0_1 = sub_180572af0(pGrenade, arg3, pProjectile_1);
180227e7b      sub_180534770(pGrenade, rbp);
180227e92      pGrenade->field_288 = sub_180227bf4;
180227e99      CBaseCSGrenadeProjectile::SetGravity(pGrenade, 0.400000006, zmm0_1, zmm6_1);
180227ea9      uint128_t zmm6_2 = CBaseCSGrenadeProjectile::SetFriction(pGrenade, 0.200000003, zmm0_1);
180227ea9      
180227ebd      if (!(0.449999988f == pGrenade->m_flElasticity))
180227ebd      {
180227ebf          pGrenade->m_pVmt->field_c0(pGrenade, 0x3d8, 0xffffffff, 0xffffffff);
180227edc          pGrenade->m_flElasticity = 0.449999988;
180227ebd      }
180227ebd      
180227eec      uint64_t rdx_4;
180227eec      
180227eec      if (rbp == 0)
180227ef7          rdx_4 = 0;
180227eec      else
180227eee          rdx_4 = ((uint64_t)*(uint8_t*)((char*)rbp + 0x314));
180227eee      
180227ee6      pGrenade->m_pVmt->field_310(pGrenade, rdx_4);
180227f1f      int32_t zmm0_2 = sub_180242a50(pGrenade, sub_180227be8, 0, 0, zmm6_2, 0);
180227f30      sub_18050c190(&arg4, *(uint32_t*)(pGrenade->field_10 + 0x38), zmm0_2);
180227f3f      sub_180240990(pGrenade, arg4, 0);
180227f49      pGrenade->field_a5e = arg5;
180227f69      void* rax_4 = sub_180569de0(sub_180972500(&data_181564620, arg5, 0, 0));
180227f79      uint128_t zmm6_3 = _mm_cvtepi32_ps(((uint128_t)*(uint32_t*)((char*)rax_4 + 0xd48)));
180227f79      
180227f83      if (!(zmm6_3 == pGrenade->field_9e0))
180227f83      {
180227f85          pGrenade->m_pVmt->field_c0(pGrenade, 0x9e0, 0xffffffff, 0xffffffff);
180227fa3          pGrenade->field_9e0 = zmm6_3;
180227f83      }
180227f83      
180227fab      zmm6_3 = *(uint32_t*)((char*)rax_4 + 0xd58);
180227fab      
180227fba      if (!(zmm6_3 == pGrenade->field_9d4))
180227fba      {
180227fbc          pGrenade->m_pVmt->field_c0(pGrenade, 0x9d4, 0xffffffff, 0xffffffff);
180227fd9          pGrenade->field_9d4 = zmm6_3;
180227fba      }
180227fba      
180227fed      uint128_t* rax_6 = sub_180d201e0(&data_1814a0578, 0xffffffff);
180227fed      
180227ff5      if (rax_6 == 0)
180227ffe          rax_6 = *(uint64_t*)(data_1814a0580 + 8);
180227ffe      
180228002      int32_t zmm0_3 = pGrenade->field_9e0;
18022800e      zmm6_3 = (*(uint32_t*)rax_6 * zmm0_3);
18022800e      
180228015      if (!(zmm0_3 == zmm6_3))
180228015      {
180228017          pGrenade->m_pVmt->field_c0(pGrenade, 0x9e0, 0xffffffff, 0xffffffff);
180228034          pGrenade->field_9e0 = zmm6_3;
180228015      }
180228015      
180228048      uint128_t* rax_9 = sub_180d201e0(&data_1814a0588, 0xffffffff);
180228048      
180228050      if (rax_9 == 0)
180228059          rax_9 = *(uint64_t*)(data_1814a0590 + 8);
180228059      
18022805d      zmm0_3 = pGrenade->field_9d4;
180228069      zmm6_3 = (*(uint32_t*)rax_9 * zmm0_3);
180228069      
180228070      if (!(zmm0_3 == zmm6_3))
180228070      {
180228072          pGrenade->m_pVmt->field_c0(pGrenade, 0x9d4, 0xffffffff, 0xffffffff);
18022808f          pGrenade->field_9d4 = zmm6_3;
180228070      }
180228070      
1802280a3      sub_1807ba910(&pGrenade->field_580, 0x10);
1802280c4      return pGrenade;
180227e10  }
As you can see, they set elasticity to 0.45f ( displayed as 0.449999988f )



//------------------------------------------------------------------------



External grenade prediction problems
Oh boy, this is definetly a hard one.

"If you tried to do this externally (like i did), with map geometry it predicts at 90% accuracy, the only pain is, surface normals, as triangles in maps are pretty big, surface normals sometimes cannot be found for no apparent reason, so there's no bounces (or that's what i've figured out from all the algos ive tried for it) so if someone could help figuring this out it would be really helpful, one thing im thinking of is interpolating triangles in the same plane as that big one, just one, in the hit point, so the algo has to do less work. If someone has tried or has had success in this it would be really helpful."

"I haven't had the issue with surface normals not being found wdym? Surely sometimes they're not accurate as the triangles are so big but non exististing means the triangle itself doesnt exist. I've been brainstorming some cool ideas for this but don't have time to implement it right now. So here's a cool idea for you if you want that 100% accuracy. Make a internal load all the data you parse just like you do in the external. For every triangle split it into like 3-4 triangles or how many you think so you don't have 2GB files. You need to trace those 3-4 triangles to get the actual points of the triangles because when you will split them they won't be "attached" to the wall. Now save all ur data and you have a more accurate way of doing it. Bonus points if you do this you can actually also save wall data and make a perfect autowall."

"Hey, actually it isn't that now that i've realized im having microbounces when i hit the wall in a certain spot, most spots do that, its a problem with probably how im interpolating between each position i have available, or it could be im not using regular hulls, i was using spheres, rn im refactoring it to use normal hulls with min -2 and max 2 which is what the game uses from the center of the grenade like a hitbox."

