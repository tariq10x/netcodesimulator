#pragma once
#include <raylib.h>
#include "Config3D.hpp"
#include "Math3DUtil.hpp"
#include <algorithm>
#include <cmath>

class SpectatorCamera {
private:
    struct State {
        Vector3 position;
        float yaw;
        float pitch;
        float transitionDurationSeconds{0.5f};
    };

    Camera3D camera;
    float yaw;
    float pitch;
    float moveSpeed;
    float fastMultiplier;

    static float pitchLimit() {
        return PI / 2.0f - 0.01f;
    }

    static float clampPitch(float value) {
        return Clamp(value, -pitchLimit(), pitchLimit());
    }

    void updateTarget() {
        Vector3 lookDir = Math3D::lookDirection(yaw, pitch);
        camera.target = Vector3Add(camera.position, lookDir);
    }

public:
    static constexpr float kDefaultCheckpointTransitionSeconds = 0.5f;
    using Checkpoint = State;

    SpectatorCamera()
        : yaw(0.0f),
          pitch(0.0f),
          moveSpeed(12.0f),
          fastMultiplier(2.5f) {
        camera.position = Vector3{0.0f, Config::PLAYER_EYE_HEIGHT, 5.0f};
        camera.up = Vector3{0.0f, 1.0f, 0.0f};
        camera.fovy = 60.0f;
        camera.projection = CAMERA_PERSPECTIVE;
        updateTarget();
    }

    static Checkpoint checkpointFromCamera(const Camera3D& source) {
        Vector3 direction = Vector3Subtract(source.target, source.position);
        if (Vector3LengthSqr(direction) <= 0.000001f) {
            direction = Vector3{0.0f, 0.0f, -1.0f};
        }
        direction = Vector3Normalize(direction);

        return Checkpoint{
            source.position,
            atan2f(direction.x, -direction.z),
            clampPitch(asinf(direction.y))
        };
    }

    static Camera3D freeFlyCamera(const Checkpoint& checkpoint) {
        Camera3D result{};
        result.position = checkpoint.position;
        result.up = Vector3{0.0f, 1.0f, 0.0f};
        result.fovy = 60.0f;
        result.projection = CAMERA_PERSPECTIVE;
        const Vector3 lookDir = Math3D::lookDirection(checkpoint.yaw, clampPitch(checkpoint.pitch));
        result.target = Vector3Add(result.position, lookDir);
        return result;
    }

    static Camera3D followFirstPersonCamera(const Vector3& eyePosition, float yaw, float pitch) {
        return freeFlyCamera(Checkpoint{eyePosition, yaw, pitch});
    }

    static Camera3D followThirdPersonCamera(const Vector3& targetPosition,
                                            float yaw,
                                            float pitch,
                                            float zoom,
                                            float eyeHeight = Config::PLAYER_EYE_HEIGHT) {
        const float clampedPitch = clampPitch(pitch);
        const float clampedZoom = std::max(0.0f, zoom);
        const Vector3 focusPoint = Vector3Add(targetPosition, Vector3{0.0f, eyeHeight, 0.0f});
        const Vector3 lookDir = Math3D::lookDirection(yaw, clampedPitch);

        Camera3D result{};
        result.position = Vector3Subtract(focusPoint, Vector3Scale(lookDir, clampedZoom));
        result.target = focusPoint;
        result.up = Vector3{0.0f, 1.0f, 0.0f};
        result.fovy = 60.0f;
        result.projection = CAMERA_PERSPECTIVE;
        return result;
    }

    void resetFromCamera(const Camera3D& source) {
        setState(checkpointFromCamera(source));
    }

    Checkpoint getState() const {
        return Checkpoint{camera.position, yaw, pitch};
    }

    void setState(const Checkpoint& state) {
        yaw = state.yaw;
        pitch = clampPitch(state.pitch);
        camera = freeFlyCamera(Checkpoint{state.position, yaw, pitch});
    }

    void update(Vector2 moveInput, bool moveUp, bool moveDown, bool fastModifier, Vector2 lookDelta, float dt) {
        yaw += lookDelta.x * Config::MOUSE_SENSITIVITY;
        pitch -= lookDelta.y * Config::MOUSE_SENSITIVITY;
        pitch = clampPitch(pitch);

        Vector3 forward = Math3D::lookDirection(yaw, pitch);
        Vector3 right = Math3D::rightFromYaw(yaw);
        Vector3 moveDir = Vector3{0.0f, 0.0f, 0.0f};

        if (moveInput.y != 0.0f) {
            moveDir = Vector3Add(moveDir, Vector3Scale(forward, moveInput.y));
        }
        if (moveInput.x != 0.0f) {
            moveDir = Vector3Add(moveDir, Vector3Scale(right, moveInput.x));
        }
        if (moveUp) {
            moveDir.y += 1.0f;
        }
        if (moveDown) {
            moveDir.y -= 1.0f;
        }

        if (Vector3Length(moveDir) > 0.001f) {
            moveDir = Vector3Normalize(moveDir);
            float speed = moveSpeed * (fastModifier ? fastMultiplier : 1.0f);
            camera.position = Vector3Add(camera.position, Vector3Scale(moveDir, speed * dt));
        }

        updateTarget();
    }

    const Camera3D& getCamera() const {
        return camera;
    }
};
