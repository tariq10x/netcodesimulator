#pragma once
#include <raylib.h>
#include <raymath.h>
#include "Config3D.hpp"

class LaserBeam3D {
private:
    Vector3 start;
    Vector3 end;
    float lifetime;
    float maxLifetime;
    Color color;
    float thickness;
    bool ghost;
    bool canDamage;

public:
    LaserBeam3D(Vector3 startPos, Vector3 endPos, Color col = RED, float duration = 0.15f, float thick = 0.05f, bool ghostBeam = false, bool canDamageEnemy = true)
        : start(startPos),
          end(endPos),
          lifetime(0.0f),
          maxLifetime(duration),
          color(col),
          thickness(thick),
          ghost(ghostBeam),
          canDamage(canDamageEnemy) {}

    void update(float dt) {
        lifetime += dt;
    }

    bool isExpired() const {
        return lifetime >= maxLifetime;
    }

    float getAlpha() const {
        return 1.0f - (lifetime / maxLifetime);
    }

    void render() const {
        Color col = color;
        col.a = (unsigned char)(getAlpha() * 255);

        // Thick laser beam - red outer layer
        float radius = thickness;
        DrawCylinderEx(start, end, radius, radius, 12, col);

        if (!ghost) {
            // Yellow inner core - smaller and brighter
            Color yellowCore = YELLOW;
            yellowCore.a = (unsigned char)(getAlpha() * 255);
            DrawCylinderEx(start, end, radius * 0.4f, radius * 0.4f, 8, yellowCore);

            // Draw spheres along the beam for visibility from all angles
            Vector3 direction = Vector3Subtract(end, start);
            float length = Vector3Length(direction);
            Vector3 normalized = Vector3Normalize(direction);

            // Draw spheres every 5 units along the beam (less frequent for cleaner look)
            const int sphereSteps = static_cast<int>(length / 5.0f);
            for (int step = 0; step <= sphereSteps; ++step) {
                const float dist = static_cast<float>(step) * 5.0f;
                Vector3 spherePos = Vector3Add(start, Vector3Scale(normalized, dist));
                DrawSphere(spherePos, radius, col);              // Red outer
                DrawSphere(spherePos, radius * 0.4f, yellowCore); // Yellow core
            }
        }
    }

    Vector3 getStart() const { return start; }
    Vector3 getEnd() const { return end; }
    float getLifetime() const { return lifetime; }
    float getMaxLifetime() const { return maxLifetime; }
    float getThickness() const { return thickness; }
    Color getColor() const { return color; }
    bool isGhostBeam() const { return ghost; }
    bool canDamageEnemy() const { return canDamage; }
};
