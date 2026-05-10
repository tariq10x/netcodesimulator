#pragma once

namespace net {
namespace server {

struct BotDirectorConfig {
    bool startFrozen{true};
    bool shootingEnabled{true};
    float reactionDelaySeconds{0.35f};
    float shotCooldownSeconds{0.75f};
    float accuracy{0.20f};
    float missYawJitterRadians{0.24f};
    float missPitchJitterRadians{0.08f};
    float decisionIntervalMinSeconds{0.45f};
    float decisionIntervalMaxSeconds{1.10f};
    float preferredDistance{12.0f};
    float retreatDistance{6.5f};
    float lateralMoveMagnitude{0.65f};
    float forwardBiasMagnitude{0.30f};
};

}  // namespace server
}  // namespace net
