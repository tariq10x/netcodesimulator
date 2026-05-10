#pragma once
#include <raylib.h>
#include <raymath.h>
#include <cmath>

namespace Math3D {
    // Linear interpolation
    inline Vector3 lerp(Vector3 a, Vector3 b, float t) {
        return Vector3Lerp(a, b, t);
    }

    // Distance in 2D (XZ plane, ignoring Y)
    inline float distance2D(Vector3 a, Vector3 b) {
        float dx = b.x - a.x;
        float dz = b.z - a.z;
        return sqrtf(dx * dx + dz * dz);
    }

    // Move towards target with max distance delta
    inline Vector3 moveTowards(Vector3 current, Vector3 target, float maxDelta) {
        Vector3 dir = Vector3Subtract(target, current);
        float dist = Vector3Length(dir);
        if (dist <= maxDelta || dist < 0.001f) {
            return target;
        }
        return Vector3Add(current, Vector3Scale(Vector3Normalize(dir), maxDelta));
    }

    // Clamp vector to bounds
    inline Vector3 clampXZ(Vector3 v, float minX, float maxX, float minZ, float maxZ) {
        v.x = Clamp(v.x, minX, maxX);
        v.z = Clamp(v.z, minZ, maxZ);
        return v;
    }

    // Check if position is within bounds
    inline bool isWithinBounds(Vector3 pos, float bounds) {
        return fabs(pos.x) <= bounds && fabs(pos.z) <= bounds;
    }

    // Forward vector from yaw (XZ plane)
    inline Vector3 forwardFromYaw(float yaw) {
        return Vector3{sinf(yaw), 0.0f, -cosf(yaw)};
    }

    // Right vector from yaw (XZ plane)
    inline Vector3 rightFromYaw(float yaw) {
        return Vector3{cosf(yaw), 0.0f, sinf(yaw)};
    }

    // Look direction from yaw and pitch
    inline Vector3 lookDirection(float yaw, float pitch) {
        float cosYaw = cosf(yaw);
        float sinYaw = sinf(yaw);
        float cosPitch = cosf(pitch);
        float sinPitch = sinf(pitch);

        return Vector3{
            sinYaw * cosPitch,
            sinPitch,
            -cosYaw * cosPitch
        };
    }
}
