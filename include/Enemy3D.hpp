#pragma once
#include <raylib.h>
#include "Config3D.hpp"
#include "Math3DUtil.hpp"
#include "Model3DWrapper.hpp"
#include "client/RenderFrame.hpp"
#include "sim/SimulationTypes.hpp"
#include <memory>
#include <cmath>
#include <algorithm>
#include <rlgl.h>

class Enemy3D {
private:
    Vector3 position;
    Vector3 velocity;
    Vector3 target;
    int waypointIndex;
    Vector3 ghostPosition;
    float health;
    float maxHealth;
    bool reachedTarget;
    float yaw;   // Horizontal rotation angle
    float pitch; // Vertical look angle (for manual control)
    float respawnTimer;
    static constexpr float RESPAWN_DELAY = 5.0f;

    // 3D Model
    std::shared_ptr<Model3DWrapper> model;
    Color baseColor;

public:
    Enemy3D(std::shared_ptr<Model3DWrapper> soldierModel, Vector3 startPos = Vector3{0.0f, 0.0f, -10.0f})
        : position(startPos),
          velocity{0.0f, 0.0f, 0.0f},
          target{10.0f, 0.0f, -10.0f},
          waypointIndex(0),
          ghostPosition(startPos),
          health(Config::ENEMY_MAX_HEALTH),
          maxHealth(Config::ENEMY_MAX_HEALTH),
          reachedTarget(false),
          yaw(0.0f),
          pitch(0.0f),
          respawnTimer(0.0f),
          model(soldierModel),
          baseColor(Color{255, 39, 104, 255}) {}

    // AI patrol logic
    void updateAI(float dt) {
        static const Vector3 waypoints[] = {
            Vector3{-20.0f, 0.0f, -20.0f},
            Vector3{20.0f, 0.0f, -20.0f},
            Vector3{20.0f, 0.0f, 20.0f},
            Vector3{-20.0f, 0.0f, 20.0f}
        };

        target = waypoints[waypointIndex % (sizeof(waypoints) / sizeof(Vector3))];

        Vector3 toTarget = Vector3Subtract(target, position);
        float distToTarget = Vector3Length(toTarget);

        if (distToTarget < Config::ENEMY_TARGET_THRESHOLD) {
            waypointIndex = (waypointIndex + 1) % (sizeof(waypoints) / sizeof(Vector3));
            target = waypoints[waypointIndex];
            toTarget = Vector3Subtract(target, position);
        }

        // Move toward target
        Vector3 direction = Vector3Normalize(toTarget);
        velocity = Vector3Scale(direction, Config::ENEMY_SPEED);
        position = Vector3Add(position, Vector3Scale(velocity, dt));

        // Update yaw to face movement direction
        if (Vector3Length(velocity) > 0.01f) {
            yaw = atan2f(velocity.x, velocity.z);
        }

        // Update animation - enemy is always walking
        if (model && model->hasAnimations()) {
            model->setAnimation(0); // Walking animation (MTF_Walking)
            model->updateAnimation(dt);
        }
    }

    // Manual control (when player controls enemy)
    void moveManual(Vector2 moveInput, Vector2 mouseDelta, float dt) {
        // Update yaw and pitch based on mouse (same as player)
        yaw += mouseDelta.x * Config::MOUSE_SENSITIVITY;
        pitch -= mouseDelta.y * Config::MOUSE_SENSITIVITY;
        pitch = Clamp(pitch, Config::MIN_PITCH, Config::MAX_PITCH);

        // Move based on input (use same math as player for consistent controls)
        if (Vector2Length(moveInput) > 0.0f) {
            Vector3 forward = Math3D::forwardFromYaw(yaw);
            Vector3 right = Math3D::rightFromYaw(yaw);

            Vector3 moveDir = Vector3Add(
                Vector3Scale(forward, moveInput.y),
                Vector3Scale(right, moveInput.x)
            );

            if (Vector3Length(moveDir) > 0.0f) {
                moveDir = Vector3Normalize(moveDir);
                position = Vector3Add(position, Vector3Scale(moveDir, Config::ENEMY_SPEED * dt));
            }
        }

        position.y = 0.0f; // Keep at ground level
    }

    // Combat
    void takeDamage(float amount) {
        health -= amount;
        if (health < 0.0f) health = 0.0f;
    }

    void updateRespawn(float dt, Vector3 spawnPos) {
        if (health <= 0.0f) {
            respawnTimer += dt;
            if (respawnTimer >= RESPAWN_DELAY) {
                respawn(spawnPos);
            }
        } else {
            respawnTimer = 0.0f;
        }
    }

    bool isAlive() const {
        return health > 0.0f;
    }

    void respawn(Vector3 spawnPos = Vector3{0.0f, 0.0f, -10.0f}) {
        position = spawnPos;
        health = maxHealth;
        velocity = Vector3{0.0f, 0.0f, 0.0f};
        reachedTarget = false;
        target = Vector3{10.0f, 0.0f, -10.0f};
        respawnTimer = 0.0f;
    }

    sim::RemoteActorState getSimState() const {
        sim::RemoteActorState state;
        state.position = toSim(position);
        state.velocity = toSim(velocity);
        state.yaw = yaw;
        state.pitch = pitch;
        state.health = health;
        state.radius = Config::ENEMY_RADIUS;
        state.alive = isAlive();
        return state;
    }

    void setSimState(const sim::RemoteActorState& state) {
        position = fromSim(state.position);
        velocity = fromSim(state.velocity);
        yaw = state.yaw;
        pitch = Clamp(state.pitch, Config::MIN_PITCH, Config::MAX_PITCH);
        health = std::clamp(state.health, 0.0f, maxHealth);
    }

    // Getters
    Vector3 getPosition() const { return position; }
    Vector3 getVelocity() const { return velocity; }
    Vector3 getGhostPosition() const { return ghostPosition; }
    float getHealth() const { return health; }
    float getMaxHealth() const { return maxHealth; }
    float getHealthPercent() const { return health / maxHealth; }
    float getYaw() const { return yaw; }
    float getPitch() const { return pitch; }
    void setColor(Color c) { baseColor = c; }

    // Setters
    void setPosition(Vector3 pos) { position = pos; }
    void setGhostPosition(Vector3 pos) { ghostPosition = pos; }
    void setYawPitch(float newYaw, float newPitch) {
        yaw = newYaw;
        pitch = Clamp(newPitch, Config::MIN_PITCH, Config::MAX_PITCH);
    }
    void setHealth(float amount) {
        health = std::clamp(amount, 0.0f, maxHealth);
    }

    // Rendering
    void render(Vector3 displayPos) const {
        render(displayPos, yaw, isAlive() ? baseColor : Color{120, 120, 120, 180});
    }

    void render(Vector3 displayPos, float yawRadians, Color enemyColor) const {
        if (model && model->isLoaded()) {
            // Render model at ground level, rotated to face direction
            // Note: Model appears static (no animation) as we're only translating/rotating the mesh
            Vector3 groundPos = displayPos;
            groundPos.y = 0.0f;

            model->drawWiresEx(groundPos, {0, 1, 0}, yawRadians * RAD2DEG, {0.001f, 0.001f, 0.001f}, enemyColor);
        } else {
            // Solid colored primitives
            DrawCylinder(displayPos, Config::ENEMY_BODY_RADIUS,
                        Config::ENEMY_BODY_RADIUS, Config::ENEMY_BODY_HEIGHT, 16, enemyColor);

            Vector3 headPos = {displayPos.x, displayPos.y + Config::ENEMY_HEAD_OFFSET, displayPos.z};
            DrawSphere(headPos, Config::ENEMY_HEAD_RADIUS, enemyColor);
        }
    }

    void render(const client::RemoteEnemyRenderItem& item) const {
        render(item.displayPosition, item.yawRadians, item.tint);
    }

    void renderGhost() const {
        Color enemyColor = isAlive() ? baseColor : Color{180, 180, 180, 120};
        if (model && model->isLoaded()) {
            // Render ghost model
            Vector3 ghostGroundPos = ghostPosition;
            ghostGroundPos.y = 0.0f;

            model->drawWiresEx(ghostGroundPos, {0, 1, 0}, yaw * RAD2DEG, {0.0009f, 0.0009f, 0.0009f}, Fade(enemyColor, 0.4f));
        } else {
            // Solid colored transparent ghost - same size as main enemy
            Color ghostColor = Fade(enemyColor, 0.4f); // 40% opacity

            DrawCylinder(ghostPosition, Config::ENEMY_BODY_RADIUS,
                        Config::ENEMY_BODY_RADIUS, Config::ENEMY_BODY_HEIGHT, 16, ghostColor);

            Vector3 ghostHeadPos = {ghostPosition.x, ghostPosition.y + Config::ENEMY_HEAD_OFFSET, ghostPosition.z};
            DrawSphere(ghostHeadPos, Config::ENEMY_HEAD_RADIUS, ghostColor);
        }
    }

    void renderHealthBar(Vector3 displayPos, const Camera3D& camera, float alpha = 1.0f) const {
        renderHealthBar(displayPos, getHealthPercent(), camera, alpha);
    }

    void renderHealthBar(Vector3 displayPos,
                         float healthPercent,
                         const Camera3D& camera,
                         float alpha = 1.0f) const {
        Vector3 center = {displayPos.x, displayPos.y + 2.6f, displayPos.z};
        float barWidth = 0.975f;
        float barHeight = 0.18f;
        Vector3 toCamera = Vector3Subtract(camera.position, center);
        toCamera.y = 0.0f;

        if (Vector3LengthSqr(toCamera) < 0.0001f) {
            toCamera = {0.0f, 0.0f, 1.0f};
        } else {
            toCamera = Vector3Normalize(toCamera);
        }

        Vector3 right = {toCamera.z, 0.0f, -toCamera.x};
        if (Vector3LengthSqr(right) < 0.0001f) {
            right = {1.0f, 0.0f, 0.0f};
        }

        Vector3 up = {0.0f, 1.0f, 0.0f};
        Vector3 drawCenter = Vector3Add(center, Vector3Scale(toCamera, 0.05f));

        auto drawQuad = [&](Vector3 quadCenter, float width, float height, Color color) {
            Vector3 halfRight = Vector3Scale(right, width * 0.5f);
            Vector3 halfUp = Vector3Scale(up, height * 0.5f);

            Vector3 topLeft = Vector3Add(Vector3Subtract(quadCenter, halfRight), halfUp);
            Vector3 topRight = Vector3Add(Vector3Add(quadCenter, halfRight), halfUp);
            Vector3 bottomRight = Vector3Subtract(Vector3Add(quadCenter, halfRight), halfUp);
            Vector3 bottomLeft = Vector3Subtract(Vector3Subtract(quadCenter, halfRight), halfUp);

            rlCheckRenderBatchLimit(4);
            rlBegin(RL_QUADS);
            rlColor4ub(color.r, color.g, color.b, color.a);
            rlVertex3f(topLeft.x, topLeft.y, topLeft.z);
            rlVertex3f(bottomLeft.x, bottomLeft.y, bottomLeft.z);
            rlVertex3f(bottomRight.x, bottomRight.y, bottomRight.z);
            rlVertex3f(topRight.x, topRight.y, topRight.z);
            rlEnd();
            rlColor4ub(255, 255, 255, 255);
        };

        rlDisableDepthTest();

        healthPercent = std::clamp(healthPercent, 0.0f, 1.0f);
        float currentWidth = barWidth * healthPercent;
        if (currentWidth > 0.001f) {
            Vector3 fillCenter = Vector3Subtract(drawCenter, Vector3Scale(right, (barWidth - currentWidth) * 0.5f));
            float alphaClamped = std::clamp(alpha, 0.0f, 1.0f);
            Color fillColor = Color{255, 60, 110, static_cast<unsigned char>(alphaClamped * 255.0f)};
            drawQuad(fillCenter, currentWidth, barHeight * 0.7f, fillColor);
        }
        rlEnableDepthTest();
    }

    void renderHealthBar(const client::RemoteEnemyRenderItem& item,
                         const Camera3D& camera,
                         float alpha = 1.0f) const {
        renderHealthBar(item.displayPosition, item.healthPercent, camera, alpha);
    }

private:
    static sim::Vec3 toSim(Vector3 value) {
        return sim::Vec3{value.x, value.y, value.z};
    }

    static Vector3 fromSim(const sim::Vec3& value) {
        return Vector3{value.x, value.y, value.z};
    }
};
