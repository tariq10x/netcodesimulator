#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sim/SimulationRules.hpp"

namespace client {

struct FovVisibilityConfig {
    float verticalFovDegrees{70.0f};
    float aspectRatio{16.0f / 9.0f};
    float rangeMeters{100.0f};
    bool requireLineOfSight{true};
};

struct FovVisibilitySample {
    bool insideCone{false};
    bool lineOfSight{true};
    bool visible{false};
    float distanceMeters{0.0f};
    float horizontalAngleDegrees{0.0f};
    float verticalAngleDegrees{0.0f};
    sim::Vec3 subjectCenter{};
};

struct FovVisibilityQuery {
    sim::PlayerState observer{};
    sim::PlayerState subject{};
    sim::MovementEnvironment environment{};
    sim::SimConfig simConfig{};
    FovVisibilityConfig config{};
};

class FovVisibilityModel {
public:
    static FovVisibilitySample evaluate(const FovVisibilityQuery& query) {
        FovVisibilitySample sample;
        if (query.observer.health <= 0.0f || query.subject.health <= 0.0f) {
            return sample;
        }

        const sim::HitscanTarget subjectTarget =
            sim::buildPlayerHitscanTarget(query.subject, query.simConfig);
        sample.subjectCenter = subjectTarget.center;

        const sim::Vec3 toSubject = subtract(subjectTarget.center, query.observer.position);
        sample.distanceMeters = length(toSubject);
        if (sample.distanceMeters <= 0.0001f ||
            sample.distanceMeters > query.config.rangeMeters) {
            return sample;
        }

        const sim::Vec3 direction = scale(toSubject, 1.0f / sample.distanceMeters);
        const sim::Vec3 forward = normalize(
            sim::lookDirection(query.observer.yaw, query.observer.pitch));
        const sim::Vec3 right = normalize(sim::rightFromYaw(query.observer.yaw));
        const sim::Vec3 up = normalize(cross(right, forward));

        const float forwardDot = dot(direction, forward);
        const float rightDot = dot(direction, right);
        const float upDot = dot(direction, up);
        if (forwardDot <= 0.0f) {
            return sample;
        }

        const float horizontalRadians = std::atan2(rightDot, forwardDot);
        const float verticalRadians = std::atan2(
            upDot,
            std::sqrt((forwardDot * forwardDot) + (rightDot * rightDot)));
        sample.horizontalAngleDegrees = radiansToDegrees(horizontalRadians);
        sample.verticalAngleDegrees = radiansToDegrees(verticalRadians);

        const float verticalFovRadians = degreesToRadians(query.config.verticalFovDegrees);
        const float horizontalFovRadians =
            2.0f * std::atan(std::tan(verticalFovRadians * 0.5f) * query.config.aspectRatio);
        sample.insideCone =
            std::fabs(horizontalRadians) <= horizontalFovRadians * 0.5f &&
            std::fabs(verticalRadians) <= verticalFovRadians * 0.5f;
        if (!sample.insideCone) {
            return sample;
        }

        sample.lineOfSight = true;
        if (query.config.requireLineOfSight) {
            const sim::HitscanRay line{
                query.observer.position,
                direction,
                sample.distanceMeters
            };
            const float obstacleDistance =
                sim::traceHitscanObstacleDistance(line, query.environment.collisionBoxes);
            sample.lineOfSight =
                obstacleDistance < 0.0f || obstacleDistance >= sample.distanceMeters;
        }
        sample.visible = sample.insideCone && sample.lineOfSight;
        return sample;
    }

private:
    static constexpr float kPi = 3.14159265358979323846f;

    static sim::Vec3 subtract(const sim::Vec3& lhs, const sim::Vec3& rhs) {
        return sim::Vec3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
    }

    static sim::Vec3 scale(const sim::Vec3& value, float scalar) {
        return sim::Vec3{value.x * scalar, value.y * scalar, value.z * scalar};
    }

    static float dot(const sim::Vec3& lhs, const sim::Vec3& rhs) {
        return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
    }

    static sim::Vec3 cross(const sim::Vec3& lhs, const sim::Vec3& rhs) {
        return sim::Vec3{
            lhs.y * rhs.z - lhs.z * rhs.y,
            lhs.z * rhs.x - lhs.x * rhs.z,
            lhs.x * rhs.y - lhs.y * rhs.x
        };
    }

    static float length(const sim::Vec3& value) {
        return std::sqrt(dot(value, value));
    }

    static sim::Vec3 normalize(const sim::Vec3& value) {
        const float valueLength = length(value);
        if (valueLength <= 0.0001f) {
            return sim::Vec3{};
        }
        return scale(value, 1.0f / valueLength);
    }

    static float degreesToRadians(float degrees) {
        return degrees * kPi / 180.0f;
    }

    static float radiansToDegrees(float radians) {
        return radians * 180.0f / kPi;
    }
};

enum class FovTransitionKind : std::uint8_t {
    Entered = 0,
    Exited = 1
};

struct FovVisibilityTransition {
    FovTransitionKind kind{FovTransitionKind::Entered};
    int perspectiveActorId{0};
    int subjectActorId{0};
    sim::PlayerState observer{};
    sim::PlayerState subject{};
    FovVisibilitySample sample{};
};

struct PerceptionFrame {
    int perspectiveActorId{0};
    sim::PlayerState observer{};
    std::vector<sim::PlayerState> subjects{};
    sim::MovementEnvironment environment{};
    sim::SimConfig simConfig{};
    FovVisibilityConfig config{};
};

class PerceptionEventMonitor {
public:
    std::vector<FovVisibilityTransition> update(const PerceptionFrame& frame) {
        std::vector<FovVisibilityTransition> transitions;
        std::unordered_set<int> evaluatedSubjects;

        for (const sim::PlayerState& subject : frame.subjects) {
            if (subject.playerId == frame.perspectiveActorId) {
                continue;
            }

            evaluatedSubjects.insert(subject.playerId);
            const FovVisibilitySample sample = FovVisibilityModel::evaluate(
                FovVisibilityQuery{
                    frame.observer,
                    subject,
                    frame.environment,
                    frame.simConfig,
                    frame.config
                });

            TrackedVisibility& tracked = tracked_[subject.playerId];
            if (sample.visible && !tracked.visible) {
                transitions.push_back(
                    FovVisibilityTransition{FovTransitionKind::Entered,
                                            frame.perspectiveActorId,
                                            subject.playerId,
                                            frame.observer,
                                            subject,
                                            sample});
            } else if (!sample.visible && tracked.visible) {
                transitions.push_back(
                    FovVisibilityTransition{FovTransitionKind::Exited,
                                            frame.perspectiveActorId,
                                            subject.playerId,
                                            frame.observer,
                                            subject,
                                            sample});
            }

            tracked.visible = sample.visible;
            tracked.lastObserver = frame.observer;
            tracked.lastSubject = subject;
            tracked.lastSample = sample;
        }

        for (auto& [subjectId, tracked] : tracked_) {
            if (tracked.visible && evaluatedSubjects.find(subjectId) == evaluatedSubjects.end()) {
                FovVisibilitySample sample = tracked.lastSample;
                sample.visible = false;
                transitions.push_back(
                    FovVisibilityTransition{FovTransitionKind::Exited,
                                            frame.perspectiveActorId,
                                            subjectId,
                                            tracked.lastObserver,
                                            tracked.lastSubject,
                                            sample});
                tracked.visible = false;
            }
        }

        return transitions;
    }

    std::vector<FovVisibilityTransition> clearVisible(int perspectiveActorId) {
        std::vector<FovVisibilityTransition> transitions;
        for (auto& [subjectId, tracked] : tracked_) {
            if (!tracked.visible) {
                continue;
            }

            FovVisibilitySample sample = tracked.lastSample;
            sample.visible = false;
            transitions.push_back(
                FovVisibilityTransition{FovTransitionKind::Exited,
                                        perspectiveActorId,
                                        subjectId,
                                        tracked.lastObserver,
                                        tracked.lastSubject,
                                        sample});
            tracked.visible = false;
        }
        return transitions;
    }

    void reset() {
        tracked_.clear();
    }

private:
    struct TrackedVisibility {
        bool visible{false};
        sim::PlayerState lastObserver{};
        sim::PlayerState lastSubject{};
        FovVisibilitySample lastSample{};
    };

    std::unordered_map<int, TrackedVisibility> tracked_{};
};

inline const char* toString(FovTransitionKind kind) {
    switch (kind) {
        case FovTransitionKind::Entered:
            return "fov.entered";
        case FovTransitionKind::Exited:
            return "fov.exited";
    }
    return "fov.entered";
}

}  // namespace client
