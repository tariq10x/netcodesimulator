#pragma once

namespace sim::defaults {

inline constexpr float kPlayerEyeHeight = 1.7f;
inline constexpr float kPlayerSpeed = 8.0f;
inline constexpr float kPlayerJumpVelocity = 11.0f;
inline constexpr float kPlayerGravity = -24.0f;
inline constexpr int kPlayerMaxJumps = 3;
inline constexpr float kPlayerCollisionRadius = 0.4f;
inline constexpr float kPlayerCollisionHeight = 1.6f;
inline constexpr float kMinPitch = -1.5f;
inline constexpr float kMaxPitch = 1.5f;
inline constexpr float kPlayerMaxHealth = 100.0f;

inline constexpr float kEnemySpeed = 6.0f;
inline constexpr float kEnemyRadius = 0.8f;
inline constexpr float kEnemyBodyRadius = 0.4f;
inline constexpr float kEnemyBodyHeight = 1.6f;
inline constexpr float kEnemyMaxHealth = 100.0f;
inline constexpr float kEnemyTargetThreshold = 0.5f;

inline constexpr float kWeaponCooldownSeconds = 0.5f;
inline constexpr float kWeaponDamage = 1000.0f;
inline constexpr float kWeaponRange = 1000.0f;
inline constexpr float kWeaponForwardOffset = 0.35f;
inline constexpr float kWeaponRightOffset = 0.2f;
inline constexpr float kWeaponDownOffset = 0.25f;

inline constexpr float kArenaHalfSize = 25.0f;

}  // namespace sim::defaults
