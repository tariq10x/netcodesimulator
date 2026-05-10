#pragma once
#include <iostream>
#include <raylib.h>
#include <string>
#include <utility>

class Model3DWrapper {
  private:
    static int animationFrameCount(const ModelAnimation& animation) {
#if defined(RAYLIB_VERSION_MAJOR) && RAYLIB_VERSION_MAJOR >= 6
        return animation.keyframeCount;
#else
        return animation.frameCount;
#endif
    }

    static void updateModelAnimation(Model& model, const ModelAnimation& animation, int frame) {
#if defined(RAYLIB_VERSION_MAJOR) && RAYLIB_VERSION_MAJOR >= 6
        UpdateModelAnimation(model, animation, static_cast<float>(frame));
#else
        UpdateModelAnimation(model, animation, frame);
#endif
    }

    Model model;
    bool loaded;
    std::string filepath;

    // Animation support
    int animationCount;
    ModelAnimation* animations;
    int currentAnimIndex;
    int currentAnimFrame;

  public:
    Model3DWrapper()
        : loaded(false), animationCount(0), animations(nullptr), currentAnimIndex(0),
          currentAnimFrame(0) {
        // Default empty model
    }

    explicit Model3DWrapper(const std::string& path)
        : loaded(false), filepath(path), animationCount(0), animations(nullptr),
          currentAnimIndex(0), currentAnimFrame(0) {
        load(path);
    }

    ~Model3DWrapper() {
        if (loaded) {
            UnloadModel(model);
        }
        if (animations) {
            UnloadModelAnimations(animations, animationCount);
        }
    }

    // Prevent copying (models should not be copied)
    Model3DWrapper(const Model3DWrapper&) = delete;
    Model3DWrapper& operator=(const Model3DWrapper&) = delete;

    // Allow moving
    Model3DWrapper(Model3DWrapper&& other) noexcept
        : model(other.model), loaded(other.loaded), filepath(std::move(other.filepath)),
          animationCount(other.animationCount), animations(other.animations),
          currentAnimIndex(other.currentAnimIndex), currentAnimFrame(other.currentAnimFrame) {
        other.loaded = false;
        other.animationCount = 0;
        other.animations = nullptr;
        other.currentAnimIndex = 0;
        other.currentAnimFrame = 0;
    }

    bool load(const std::string& path) {
        if (loaded) {
            UnloadModel(model);
            loaded = false;
        }
        if (animations) {
            UnloadModelAnimations(animations, animationCount);
            animations = nullptr;
        }
        animationCount = 0;
        currentAnimIndex = 0;
        currentAnimFrame = 0;

        model = LoadModel(path.c_str());
        loaded = true;
        filepath = path;

        if (model.meshCount == 0) {
            std::cerr << "Warning: Failed to load model from " << path << std::endl;
            loaded = false;
            return false;
        }

        std::cout << "Loaded model: " << path << " (meshes: " << model.meshCount
                  << ", animations disabled)" << std::endl;

        return true;
    }

    void updateAnimation(float /*dt*/) {
        if (!loaded || animationCount == 0 || !animations)
            return;

        // Safety check: ensure valid animation index
        if (currentAnimIndex < 0 || currentAnimIndex >= animationCount) {
            currentAnimIndex = 0;
            return;
        }

        // Safety check: ensure valid frame
        const int frameCount = animationFrameCount(animations[currentAnimIndex]);
        if (frameCount <= 0)
            return;

        currentAnimFrame++;
        if (currentAnimFrame >= frameCount) {
            currentAnimFrame = 0; // Loop animation
        }

        // Try to update animation with safety wrapper
        updateModelAnimation(model, animations[currentAnimIndex], currentAnimFrame);
    }

    void setAnimation(int animIndex) {
        if (animIndex >= 0 && animIndex < animationCount && animIndex != currentAnimIndex) {
            currentAnimIndex = animIndex;
            currentAnimFrame = 0;
        }
    }

    int getCurrentAnimation() const {
        return currentAnimIndex;
    }

    void drawWires(Vector3 position, float scale, Color tint) const {
        if (loaded) {
            DrawModelWires(model, position, scale, tint);
        }
    }

    void drawWiresEx(Vector3 position, Vector3 rotationAxis, float rotationAngle, Vector3 scale,
                     Color tint) const {
        if (loaded) {
            DrawModelWiresEx(model, position, rotationAxis, rotationAngle, scale, tint);
        }
    }

    bool isLoaded() const {
        return loaded;
    }
    bool hasAnimations() const {
        return animationCount > 0;
    }
    int getAnimationCount() const {
        return animationCount;
    }
    const std::string& getFilepath() const {
        return filepath;
    }
};
