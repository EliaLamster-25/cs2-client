struct traj1
{
    Vec3 IntialPos{};
    Vec3 m_vInitialPosition;
    Vec3 m_vInitialVelocity;
 
    std::vector<Vec3> predictedt;
};
 
// Sphere structure
struct Sphere {
    Vec3 center{};
    float radius{};
};
 
 
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
 
 
    inline bool intersectsSphere(const Vec3& sphereCenter, float sphereRadius) const {
 
        float closestX = (((min.x) > ((((sphereCenter.x) < (max.x)) ? (sphereCenter.x) : (max.x)))) ? (min.x) : ((((sphereCenter.x) < (max.x)) ? (sphereCenter.x) : (max.x))));
        float closestY = (((min.y) > ((((sphereCenter.y) < (max.y)) ? (sphereCenter.y) : (max.y)))) ? (min.y) : ((((sphereCenter.y) < (max.y)) ? (sphereCenter.y) : (max.y))));
        float closestZ = (((min.z) > ((((sphereCenter.z) < (max.z)) ? (sphereCenter.z) : (max.z)))) ? (min.z) : ((((sphereCenter.z) < (max.z)) ? (sphereCenter.z) : (max.z))));
 
        Vec3 closestPoint(closestX, closestY, closestZ);
        Vec3 diff = closestPoint - sphereCenter;
        float distSq = diff.LengthSq();  
 
        return distSq <= sphereRadius * sphereRadius;
    }
 
inline void updateClosestPoint(const Vec3& point, const Vec3& a, const Vec3& b, Vec3& closestPoint, float& minDistSq) {
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
 
inline Vec3 closestPointOnTriangle(const Vec3& point, const Triangle& triangle, float& u, float& v, float& w) {
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
 
    
    updateClosestPoint(point, triangle.p1, triangle.p2, closestPoint, minDistSq);
    updateClosestPoint(point, triangle.p2, triangle.p3, closestPoint, minDistSq);
    updateClosestPoint(point, triangle.p3, triangle.p1, closestPoint, minDistSq);
 
    return closestPoint;
}
 
inline bool sphereIntersectsTriangle(KDNode * node, const Sphere & sphere, const Triangle & triangle) {
 
    Vec3 edge1 = triangle.p2 - triangle.p1;
    Vec3 edge2 = triangle.p3 - triangle.p1;
    Vec3 normal = CrossProduct(edge1, edge2).Normalize();
    float distanceToPlane = (sphere.center - triangle.p1).Dot(normal);
 
    if (std::fabs(distanceToPlane) > sphere.radius) {
        return false; 
    }
 
    Vec3 projectedCenter = sphere.center - normal * distanceToPlane;
 
    float u, v, w;
    Vec3 closestPoint = closestPointOnTriangle(projectedCenter, triangle, u, v, w);
 
    float distanceToClosestPoint = (closestPoint - sphere.center).Length();
    return distanceToClosestPoint <= sphere.radius;
}
 
 
inline bool sphereIntersectsKDTree(KDNode* node, const Sphere& sphere) {
    if (node == nullptr) return false;
 
    if (!node->bbox.intersectsSphere(sphere.center, sphere.radius)) {
        return false;
    }
 
    for (const auto& tri : node->triangle) {
        if (sphereIntersectsTriangle(node, sphere, tri)) {
            return true;
        }
    }
 
    bool hitLeft = sphereIntersectsKDTree(node->left, sphere);
    bool hitRight = sphereIntersectsKDTree(node->right, sphere);
 
    return hitLeft || hitRight;
}
 
inline bool tracehullsphere(Sphere sphere) {
    return sphereIntersectsKDTree(kd_tree, sphere);
}
 
inline void traverseKDTree(KDNode* node, const Vec3& pos, std::vector<Triangle>& intersectedTriangles) {
    if (node == nullptr) {
        return;
    }
 
    // Use a small radius to define the bounding sphere around the interpolated position
    constexpr float SEARCH_RADIUS = 10.0f;
 
    Sphere sp;
 
    sp.radius = 10.0f;
    sp.center = pos;
 
    // Lambda to check if the point lies within the node's bounding box (using a bounding sphere)
    auto intersectsBoundingBox = [&](const BoundingBox& bbox) {
        return bbox.intersectsSphere(pos, SEARCH_RADIUS);
        };
 
    // Early exit if the bounding box of the current node doesn't intersect the sphere
    if (!intersectsBoundingBox(node->bbox)) {
        return;
    }
 
    // Check for intersections with triangles in the current node
    for (const auto& triangle : node->triangle) {
        if (sphereIntersectsTriangle(node, sp, triangle)) {  // If point is inside or near this triangle
            intersectedTriangles.push_back(triangle);
        }
    }
 
    // Use a stack to traverse the KD-tree iteratively (to avoid recursion depth issues)
    std::stack<KDNode*> nodeStack;
    nodeStack.push(node->left);
    nodeStack.push(node->right);
 
    while (!nodeStack.empty()) {
        KDNode* currentNode = nodeStack.top();
        nodeStack.pop();
 
        // Continue traversing if the current node is valid and intersects with the bounding sphere
        if (currentNode != nullptr && intersectsBoundingBox(currentNode->bbox)) {
            // Check for intersections with triangles in the current node
            for (const auto& triangle : currentNode->triangle) {
                if (sphereIntersectsTriangle(currentNode, sp, triangle)) {
                    intersectedTriangles.push_back(triangle);
                }
            }
 
            // Push child nodes onto the stack for further traversal
            nodeStack.push(currentNode->left);
            nodeStack.push(currentNode->right);
        }
    }
}
 
 
inline bool GetSurfaceNormal(const Vec3& interpolatedPos, Vec3& collisionNormal) {
    std::vector<Triangle> intersectedTriangles;
    intersectedTriangles.reserve(100);
    traverseKDTree(kd_tree, interpolatedPos, intersectedTriangles);
 
    if (intersectedTriangles.empty()) {
        return false;
    }
 
    Vec3 accumulatedNormal(0.0f, 0.0f, 0.0f);
    int validTriangleCount = 0;
 
    const Triangle& referenceTriangle = intersectedTriangles[0];
    Vec3 refEdge1 = referenceTriangle.p2 - referenceTriangle.p1;
    Vec3 refEdge2 = referenceTriangle.p3 - referenceTriangle.p1;
    Vec3 referenceNormal = CrossProduct(refEdge1, refEdge2);
    if (referenceNormal.LengthSq() < 1e-6f) {
        return false;
    }
    referenceNormal = referenceNormal.Normalize();
 
    const float DOT_THRESHOLD = 1.0f;
    for (const auto& triangle : intersectedTriangles) {
        Vec3 edge1 = triangle.p2 - triangle.p1;
        Vec3 edge2 = triangle.p3 - triangle.p1;
        Vec3 normal = CrossProduct(edge1, edge2);
 
        if (normal.LengthSq() < 1e-6f) {
            continue;
        }
        normal = normal.Normalize();
 
        if (normal.Dot(referenceNormal) >= DOT_THRESHOLD) {
            accumulatedNormal += normal;
            ++validTriangleCount;
        }
    }
 
    if (validTriangleCount == 0) {
        collisionNormal = referenceNormal;
    }
    else {
        collisionNormal = accumulatedNormal.Normalize();
    }
 
    return true;
}
 
 
 
 
 
 
 
Sphere grenadecalc;
std::mutex predmtx;
 
Vec3 CalculateThrowVelocity(const QAngle& viewAngles, float throwStrength, const Vec3& playerVelocity) {
 
    QAngle adjustedAngles = viewAngles;
 
    if (adjustedAngles.pitch < -89.f) {
        adjustedAngles.pitch += 360.f;
    }
    else if (adjustedAngles.pitch > 89.f) {
        adjustedAngles.pitch -= 360.f;
    }
 
    adjustedAngles.pitch -= (90.0f - std::fabsf(adjustedAngles.pitch)) * 10.0f / 90.0f;
    
 
    Vec3 direction = AngleToDirection(adjustedAngles);
    
 
    Vec3 throwVelocity = direction * (throwStrength * 0.7f + 0.3f) * 1115.0f;
 
 
    throwVelocity += playerVelocity * 1.25f;
    
 
    return throwVelocity;
}
 
std::vector<Vec3> SimulateGrenadeTrajectory(
    const Vec3& startPos,
    const Vec3& velocity,
    float CoR = 0.45f,
    float friction = 0.4f
) {
 
    Vec3 currentPos = startPos;
    Vec3 currentVel = velocity;
    std::vector<Vec3> path;
    path.reserve(1000);
 
    grenadecalc.radius = 5.0f;
    constexpr int INTERPOLATION_STEPS = 10;
 
    int bounceCount = 0;
 
    for (int step = 0; step < MAX_SIMULATION_STEPS && bounceCount < 10; ++step) {
        
        path.emplace_back(currentPos);
 
        currentVel.z -= CS_GRAVITY * CS_TICK_INTERVAL;
        Vec3 nextPos = currentPos + (currentVel * CS_TICK_INTERVAL);
        bool collided = false;
 
        for (int i = 1; i <= INTERPOLATION_STEPS; ++i) {
            float t = static_cast<float>(i) / INTERPOLATION_STEPS;
            Vec3 interpolatedPos = currentPos + (nextPos - currentPos) * t;
            grenadecalc.center = interpolatedPos;
 
            if (map.tracehullsphere(grenadecalc)) {
                path.emplace_back(interpolatedPos);
 
                Vec3 surfaceNormal;
                if (!map.GetSurfaceNormal(interpolatedPos, surfaceNormal))
                    break;
                surfaceNormal = surfaceNormal.Normalized();
                float normalVelocity = currentVel.Dot(surfaceNormal);
                Vec3 velocityNormal = surfaceNormal * normalVelocity;
                Vec3 velocityTangent = currentVel - velocityNormal;
                velocityNormal = -velocityNormal * CoR;
                velocityTangent *= friction;
                currentVel = velocityNormal + velocityTangent;
                currentPos = interpolatedPos + surfaceNormal;
                collided = true;
                bounceCount++;
                break;
            }
        }
 
        if (!collided) {
            currentPos = nextPos;
        }
 
        if (std::abs(currentVel.x) < 0.1f && std::abs(currentVel.y) < 0.1f && std::abs(currentVel.z) < 0.1f) {
            break;
        }
    }
 
    return path;
}
 
void DrawPredictedTrajectory(const std::vector<Vec3>& path) {
    auto* drawList = ImGui::GetBackgroundDrawList();
 
    for (size_t i = 1; i < path.size(); ++i) {
        Vec2 screenPos1, screenPos2;
 
        if (w2s(path[i - 1], screenPos1, current__vm, SCW, SCH) &&
            w2s(path[i], screenPos2, current__vm, SCW, SCH)) {
 
            drawList->AddLine(
                ImVec2(screenPos1.x, screenPos1.y),
                ImVec2(screenPos2.x, screenPos2.y),
                IM_COL32(0, 255, 0, 255) // TODO: Add slider for color
            );
        }
    }
}
 
traj1 traj;
 
// Main grenade prediction loop
inline void GrenadePred() {
    while (true) {
 
        if (lp.currweapid == WEAPON_HEGRENADE ||
            lp.currweapid == WEAPON_MOLOTOV ||
            lp.currweapid == WEAPON_INCGRENADE ||
            lp.currweapid == WEAPON_FLASHBANG ||
            lp.currweapid == WEAPON_SMOKEGRENADE) // If we have a grenade in hand only then check for holding mouse, could be changed to pull pin or smth, idc though.
        {
            
            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) || ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) && (GetAsyncKeyState(VK_RBUTTON) & 0x8000))) //driver->RPM<float>(lp.currweapid + cs2_dumper::schemas::client_dll::C_BaseCSGrenade::m_fThrowTime) == 0.f)
            {
 
                
 
                float throw_strength = driver->RPM<float>(
                    lp.weaponptr + cs2_dumper::schemas::client_dll::C_BaseCSGrenade::m_flThrowStrength); 
                
 
                std::lock_guard<std::mutex> lock(predmtx);
 
                
                Vec3 playerVelocity = driver->RPM<Vec3>(lp.Pawn + cs2_dumper::schemas::client_dll::C_BaseEntity::m_vecVelocity); 
                traj.m_vInitialVelocity = CalculateThrowVelocity(lp.ViewAngles, throw_strength, playerVelocity); 
                traj.m_vInitialPosition = lp.Origin.pos + driver->RPM<Vec3>(lp.Pawn + cs2_dumper::schemas::client_dll::C_BaseModelEntity::m_vecViewOffset); // set initial pos
 
                
                traj.predictedt = SimulateGrenadeTrajectory(traj.m_vInitialPosition, traj.m_vInitialVelocity); 
            }
        }
 
        sleepMicroseconds(1000LL);
    }
}
 
// this goes on a drawing loop. 
 
 
        std::lock_guard<std::mutex> lock(predmtx);
 
        DrawPredictedTrajectory(traj.predictedt);