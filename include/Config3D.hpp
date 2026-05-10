#pragma once

namespace Config {
    // Screen & Viewport
    constexpr int SCREEN_WIDTH = 1920;
    constexpr int SCREEN_HEIGHT = 1080;
    constexpr int TARGET_FPS = 60;

    // Player
    constexpr float PLAYER_EYE_HEIGHT = 1.7f;
    constexpr float PLAYER_SPEED = 8.0f;  // Increased from 5.0f
    constexpr float PLAYER_JUMP_VELOCITY = 11.0f;
    constexpr float PLAYER_GRAVITY = -24.0f;
    constexpr int PLAYER_MAX_JUMPS = 3;   // single + double + triple
    constexpr float MOUSE_SENSITIVITY = 0.002f;
    constexpr float MAX_PITCH = 1.5f;
    constexpr float MIN_PITCH = -1.5f;
    constexpr float PLAYER_MAX_HEALTH = 100.0f;

    // Enemy
    constexpr float ENEMY_SPEED = 6.0f;  // Increased from 3.0f
    constexpr float ENEMY_RADIUS = 0.8f;
    constexpr float ENEMY_BODY_RADIUS = 0.4f;
    constexpr float ENEMY_BODY_HEIGHT = 1.6f;
    constexpr float ENEMY_HEAD_RADIUS = 0.4f;
    constexpr float ENEMY_HEAD_OFFSET = 2.0f;
    constexpr float HEALTH_BAR_VERTICAL_OFFSET =
        ENEMY_HEAD_OFFSET + ENEMY_HEAD_RADIUS + 0.2f;
    constexpr float ENEMY_MAX_HEALTH = 100.0f;
    constexpr float ENEMY_TARGET_THRESHOLD = 0.5f;

    // Combat
    constexpr float SHOOT_COOLDOWN = 0.5f;
    constexpr float SHOOT_DAMAGE = 1000.0f;
    constexpr float SHOOT_RANGE = 1000.0f;

    // Arena
    constexpr float ARENA_SIZE = 25.0f;
    constexpr float ARENA_WALL_HEIGHT = 2.0f;
    constexpr float ARENA_WALL_THICKNESS = 2.0f;

    // Weapon muzzle offset relative to camera (player local space)
    constexpr float WEAPON_FORWARD_OFFSET = 0.35f;
    constexpr float WEAPON_RIGHT_OFFSET = 0.2f;
    constexpr float WEAPON_DOWN_OFFSET = 0.25f;
}
