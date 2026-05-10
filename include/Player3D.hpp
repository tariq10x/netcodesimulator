#pragma once
#include <raylib.h>
#include "Arena3D.hpp"
#include "Config3D.hpp"
#include "Math3DUtil.hpp"
#include "Model3DWrapper.hpp"
#include "client/RenderFrame.hpp"
#include "sim/SimulationTypes.hpp"
#include <memory>
#include <algorithm>
#include <cmath>
#include <rlgl.h>

class Player3D {
private:
    Vector3 position;
    Vector3 ghostPosition;
    float yaw;
    float pitch;
    float speed;
    float health;
    float maxHealth;
    float verticalVelocity{0.0f};
    int jumpsUsed{0};

    // Shooting
    bool canShoot;
    float shootCooldown;
    float shootCooldownTimer;

    // 3D Model
    std::shared_ptr<Model3DWrapper> model;

public:
    Player3D(std::shared_ptr<Model3DWrapper> soldierModel, Vector3 startPos = Vector3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f})
        : position(startPos),
          ghostPosition(startPos),
          yaw(0.0f),
          pitch(0.0f),
          speed(Config::PLAYER_SPEED),
          health(Config::PLAYER_MAX_HEALTH),
          maxHealth(Config::PLAYER_MAX_HEALTH),
          canShoot(true),
          shootCooldown(Config::SHOOT_COOLDOWN),
          shootCooldownTimer(0.0f),
          model(soldierModel) {}

    // Movement
    void move(Vector2 moveInput, bool jumpPressed, float dt) {
        bool isMoving = Vector2Length(moveInput) > 0.0f;

        // Note: Animation switching disabled for player ghost to avoid conflicts
        // (player and enemy share the same model instance)

        if (isMoving) {
            Vector3 forward = Math3D::forwardFromYaw(yaw);
            Vector3 right = Math3D::rightFromYaw(yaw);

            Vector3 moveDir = Vector3Add(
                Vector3Scale(forward, moveInput.y),
                Vector3Scale(right, moveInput.x)
            );

            if (Vector3Length(moveDir) > 0.0f) {
                moveDir = Vector3Normalize(moveDir);
                position = Vector3Add(position, Vector3Scale(moveDir, speed * dt));
            }
        }

        // Jump handling (allow up to triple jump)
        bool grounded = position.y <= Config::PLAYER_EYE_HEIGHT + 0.001f;
        if (grounded) {
            position.y = Config::PLAYER_EYE_HEIGHT;
            verticalVelocity = std::max(verticalVelocity, 0.0f);
            jumpsUsed = 0;
        }
        if (jumpPressed && jumpsUsed < Config::PLAYER_MAX_JUMPS) {
            verticalVelocity = Config::PLAYER_JUMP_VELOCITY;
            jumpsUsed++;
        }

        // Gravity
        verticalVelocity += Config::PLAYER_GRAVITY * dt;
        position.y += verticalVelocity * dt;

        // Ground clamp
        if (position.y < Config::PLAYER_EYE_HEIGHT) {
            position.y = Config::PLAYER_EYE_HEIGHT;
            verticalVelocity = 0.0f;
            jumpsUsed = 0;
        }
    }

    // Look
    void look(Vector2 mouseDelta) {
        yaw += mouseDelta.x * Config::MOUSE_SENSITIVITY;
        pitch -= mouseDelta.y * Config::MOUSE_SENSITIVITY;
        pitch = Clamp(pitch, Config::MIN_PITCH, Config::MAX_PITCH);
    }

    // Bounds
    void clampToBounds(float bounds) {
        position = Math3D::clampXZ(position, -bounds, bounds, -bounds, bounds);
    }

    // Ghost - updated from network simulator
    void setGhostPosition(Vector3 pos) {
        ghostPosition = pos;
    }

    // Shooting
    void updateCooldown(float dt) {
        if (!canShoot) {
            shootCooldownTimer += dt;
            if (shootCooldownTimer >= shootCooldown) {
                canShoot = true;
            }
        }
    }

    // Animation
    void updateAnimation(float dt) {
        if (model && model->hasAnimations()) {
            model->updateAnimation(dt);
        }
    }

    void shoot() {
        if (canShoot) {
            canShoot = false;
            shootCooldownTimer = 0.0f;
        }
    }

    sim::PlayerState getSimState() const {
        sim::PlayerState state;
        state.position = toSim(position);
        state.velocity = sim::Vec3{0.0f, verticalVelocity, 0.0f};
        state.yaw = yaw;
        state.pitch = pitch;
        state.health = health;
        state.maxHealth = maxHealth;
        state.weaponCooldownRemaining = canShoot ? 0.0f : std::max(shootCooldown - shootCooldownTimer, 0.0f);
        state.jumpsUsed = jumpsUsed;
        state.grounded = position.y <= Config::PLAYER_EYE_HEIGHT + 0.001f;
        return state;
    }

    void setSimState(const sim::PlayerState& state) {
        position = fromSim(state.position);
        yaw = state.yaw;
        pitch = Clamp(state.pitch, Config::MIN_PITCH, Config::MAX_PITCH);
        health = std::clamp(state.health, 0.0f, state.maxHealth);
        maxHealth = std::max(0.0f, state.maxHealth);
        verticalVelocity = state.velocity.y;
        jumpsUsed = state.jumpsUsed;

        const float cooldownRemaining = std::max(state.weaponCooldownRemaining, 0.0f);
        canShoot = cooldownRemaining <= 0.0f;
        shootCooldownTimer = canShoot ? shootCooldown : std::max(shootCooldown - cooldownRemaining, 0.0f);
    }

    // Getters
    Vector3 getPosition() const { return position; }
    Vector3 getGhostPosition() const { return ghostPosition; }
    float getYaw() const { return yaw; }
    float getPitch() const { return pitch; }
    Vector3 getForwardVector() const { return Math3D::forwardFromYaw(yaw); }
    Vector3 getRightVector() const { return Math3D::rightFromYaw(yaw); }
    Vector3 getLookDirection() const { return Math3D::lookDirection(yaw, pitch); }
    bool getCanShoot() const { return canShoot; }
    float getHealth() const { return health; }
    float getMaxHealth() const { return maxHealth; }
    float getHealthPercent() const { return maxHealth > 0.0f ? health / maxHealth : 0.0f; }
    float getVerticalVelocity() const { return verticalVelocity; }

    // Setters
    void setPosition(Vector3 pos) { position = pos; }
    void setYawPitch(float newYaw, float newPitch) {
        yaw = newYaw;
        pitch = Clamp(newPitch, Config::MIN_PITCH, Config::MAX_PITCH);
    }
    void setHealth(float amount) { health = std::clamp(amount, 0.0f, maxHealth); }
    void resetJumpState() { verticalVelocity = 0.0f; jumpsUsed = 0; }

    static Vector3 renderRootFromEyePosition(const sim::Vec3& eyePosition, const Arena3D& arena) {
        return arena.playerRenderRootFromEyePosition(eyePosition);
    }

    static Vector3 renderRootFromSimState(const sim::PlayerState& state, const Arena3D& arena) {
        return renderRootFromEyePosition(state.position, arena);
    }

    static Vector3 healthBarRootFromEyePosition(Vector3 eyePosition) {
        eyePosition.y -= Config::PLAYER_EYE_HEIGHT;
        return eyePosition;
    }

    static Vector3 headCenterFromRoot(Vector3 rootPosition) {
        return Vector3{rootPosition.x, rootPosition.y + Config::ENEMY_HEAD_OFFSET, rootPosition.z};
    }

    static Vector3 healthBarCenterFromRoot(Vector3 rootPosition) {
        return Vector3{
            rootPosition.x,
            rootPosition.y + Config::HEALTH_BAR_VERTICAL_OFFSET,
            rootPosition.z
        };
    }

    // Rendering
    void render() const {
        Vector3 groundPos = position;
        groundPos.y = 0.0f;
        renderAtRoot(groundPos);
    }

    void renderAtRoot(Vector3 rootPos, Color tint = Color{46, 107, 255, 255}) const {
        renderAtRoot(rootPos, yaw, tint);
    }

    void renderAtRoot(Vector3 rootPos, float yawRadians, Color tint) const {
        if (model && model->isLoaded()) {
            model->drawWiresEx(rootPos, {0, 1, 0}, yawRadians * RAD2DEG, {0.001f, 0.001f, 0.001f}, tint);
        } else {
            DrawCylinder(rootPos, Config::ENEMY_BODY_RADIUS,
                         Config::ENEMY_BODY_RADIUS, Config::ENEMY_BODY_HEIGHT, 16, tint);

            Vector3 headPos = headCenterFromRoot(rootPos);
            DrawSphere(headPos, Config::ENEMY_HEAD_RADIUS, tint);
        }
    }

    void renderGhostAtRoot(Vector3 rootPos, float /*yawRadians*/, Color tint) const {
        Vector3 drawRoot = rootPos;

        Color fillTint = tint;
        fillTint.a = static_cast<unsigned char>(std::clamp<int>(fillTint.a, 36, 110));

        Color outlineTint = tint;
        outlineTint.a =
            static_cast<unsigned char>(std::clamp<int>(fillTint.a + 45, 70, 155));

        constexpr float kGhostBodyScale = 0.95f;
        constexpr float kGhostBodyHeightScale = 0.95f;
        constexpr float kGhostHeadScale = 0.95f;
        constexpr float kGhostHeadLowerOffset = 0.02f;
        const float bodyRadius = Config::ENEMY_BODY_RADIUS * kGhostBodyScale;
        const float bodyHeight = Config::ENEMY_BODY_HEIGHT * kGhostBodyHeightScale;
        const float headRadius = Config::ENEMY_HEAD_RADIUS * kGhostHeadScale;
        Vector3 headPos = headCenterFromRoot(rootPos);
        headPos.y -= kGhostHeadLowerOffset;

        // Keep the ghost head centered on the solid head anchor; shrinking from
        // the shared center makes a zero-offset ghost disappear inside the actor.
        DrawCylinder(drawRoot,
                     bodyRadius,
                     bodyRadius,
                     bodyHeight,
                     16,
                     fillTint);
        DrawCylinderWires(drawRoot,
                          bodyRadius,
                          bodyRadius,
                          bodyHeight,
                          16,
                          outlineTint);
        DrawSphere(headPos, headRadius, fillTint);
        DrawSphereWires(headPos, headRadius, 10, 10, outlineTint);
    }

    void render(const client::RemotePlayerRenderItem& item) const {
        Color tint = item.alive ? item.tint : Color{120, 120, 120, 180};
        if (item.ghost) {
            renderGhostAtRoot(item.rootPosition, item.yawRadians, tint);
            return;
        }
        renderAtRoot(item.rootPosition, item.yawRadians, tint);
    }

    void renderGhost() const {
        static const Color playerColor = Color{46, 107, 255, 255}; // #2E6BFF

        if (model && model->isLoaded()) {
            // Render ghost model at delayed position
            Vector3 ghostGroundPos = ghostPosition;
            ghostGroundPos.y = 0.0f; // Ground level

            // Rotate model to face movement direction
            // Note: Model appears static (no animation) as we're only translating/rotating
            model->drawWiresEx(ghostGroundPos, {0, 1, 0}, yaw * RAD2DEG, {0.0009f, 0.0009f, 0.0009f}, Fade(playerColor, 0.4f));
        } else {
            // Solid colored transparent ghost - same shape as enemy ghost
            Color ghostColor = Fade(playerColor, 0.4f); // 40% opacity

            // Render at ground level (y = 0.0f)
            Vector3 ghostGroundPos = ghostPosition;
            ghostGroundPos.y = 0.0f;

            // Body cylinder
            DrawCylinder(ghostGroundPos, Config::ENEMY_BODY_RADIUS,
                        Config::ENEMY_BODY_RADIUS, Config::ENEMY_BODY_HEIGHT, 16, ghostColor);

            // Head sphere
            Vector3 ghostHeadPos = {ghostGroundPos.x, ghostGroundPos.y + Config::ENEMY_HEAD_OFFSET, ghostGroundPos.z};
            DrawSphere(ghostHeadPos, Config::ENEMY_HEAD_RADIUS, ghostColor);
        }
    }

    void renderHealthBar(Vector3 displayPos, float healthPercent, const Camera3D& camera, float alpha = 1.0f) const {
        healthPercent = std::clamp(healthPercent, 0.0f, 1.0f);
        Vector3 center = healthBarCenterFromRoot(displayPos);
        float barWidth = 0.975f; // 25% shorter than enemy bar baseline
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

        float currentWidth = barWidth * healthPercent;
        if (currentWidth > 0.001f) {
            Vector3 fillCenter = Vector3Subtract(drawCenter, Vector3Scale(right, (barWidth - currentWidth) * 0.5f));
            float alphaClamped = std::clamp(alpha, 0.0f, 1.0f);
            Color fillColor = Color{60, 220, 120, static_cast<unsigned char>(alphaClamped * 255.0f)};
            drawQuad(fillCenter, currentWidth, barHeight * 0.7f, fillColor);
        }
        rlEnableDepthTest();
    }

    void renderHealthBar(const client::RemotePlayerRenderItem& item,
                         const Camera3D& camera,
                         float alpha = 1.0f) const {
        if (!item.alive || item.ghost) {
            return;
        }
        renderHealthBar(item.rootPosition, item.healthPercent, camera, alpha);
    }

private:
    static sim::Vec3 toSim(Vector3 value) {
        return sim::Vec3{value.x, value.y, value.z};
    }

    static Vector3 fromSim(const sim::Vec3& value) {
        return Vector3{value.x, value.y, value.z};
    }
};
