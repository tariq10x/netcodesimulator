#pragma once

#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "CharacterThumbnail.hpp"
#include "Config3D.hpp"
#include "DisplayManager.hpp"
#include "MainMenu.hpp"
#include "TypographyService.hpp"
#include "app/CharacterPresetStore.hpp"

class CharacterEditorScreen {
public:
    CharacterEditorScreen() {
        reloadProfiles();
    }

    GameMode update(float /*dt*/, bool allowBackShortcut = true) {
        const bool escapePressed = IsKeyPressed(KEY_ESCAPE);
        const bool qPressed = IsKeyPressed(KEY_Q);
        if (allowBackShortcut && (escapePressed || qPressed)) {
            if (nameEditing_) {
                nameEditing_ = false;
                return GameMode::CHARACTER_EDITOR;
            }
            return GameMode::MAIN_MENU;
        }

        const Vector2 mouse = display::mousePosition();
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            activeSlider_ = Slider::None;
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(mouse, backButton())) {
            return GameMode::MAIN_MENU;
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            handleClick(mouse);
        }
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && activeSlider_ != Slider::None) {
            updateSlider(activeSlider_, mouse.x);
        }

        handleNameInput();
        return GameMode::CHARACTER_EDITOR;
    }

    void render() const {
        ClearBackground(Color{9, 13, 20, 255});
        renderBackdrop();

        drawText(TypographyStyleId::ScreenTitle,
                 "Character Editor",
                 64.0f,
                 48.0f,
                 RAYWHITE);
        drawText(TypographyStyleId::Body,
                 "Create visual character silhouettes for hosted sessions.",
                 66.0f,
                 102.0f,
                 Color{180, 192, 210, 255});

        drawButton(backButton(), "Back", false, true);
        renderProfileList();
        renderEditorPanel();
        renderPreview();

        if (!statusMessage_.empty()) {
            drawText(TypographyStyleId::Caption,
                     statusMessage_,
                     64.0f,
                     1010.0f,
                     Color{244, 210, 130, 255});
        }
    }

    const character::CharacterProfile& selectedProfileForTest() const {
        return editingProfile_;
    }

    std::size_t profileCountForTest() const {
        return profiles_.size();
    }

private:
    enum class Slider {
        None = 0,
        ShoulderWidth = 1,
        ShoulderHeight = 2,
        ShoulderAngle = 3
    };

    app::CharacterPresetStore store_{};
    std::vector<character::CharacterProfile> profiles_{};
    int selectedIndex_{0};
    character::CharacterProfile editingProfile_{character::defaultProfile()};
    bool nameEditing_{false};
    Slider activeSlider_{Slider::None};
    std::string statusMessage_{};

    static Rectangle backButton() {
        return Rectangle{1510.0f, 52.0f, 260.0f, 56.0f};
    }

    static Rectangle listPanel() {
        return Rectangle{64.0f, 164.0f, 430.0f, 770.0f};
    }

    static Rectangle editorPanel() {
        return Rectangle{530.0f, 164.0f, 560.0f, 770.0f};
    }

    static Rectangle previewPanel() {
        return Rectangle{1126.0f, 164.0f, 730.0f, 770.0f};
    }

    static Rectangle nameField() {
        const Rectangle panel = editorPanel();
        return Rectangle{panel.x + 36.0f, panel.y + 94.0f, panel.width - 72.0f, 56.0f};
    }

    static Rectangle newButton() {
        const Rectangle panel = listPanel();
        return Rectangle{panel.x + 28.0f, panel.y + panel.height - 82.0f, 118.0f, 48.0f};
    }

    static Rectangle saveButton() {
        const Rectangle panel = listPanel();
        return Rectangle{panel.x + 158.0f, panel.y + panel.height - 82.0f, 118.0f, 48.0f};
    }

    static Rectangle deleteButton() {
        const Rectangle panel = listPanel();
        return Rectangle{panel.x + 288.0f, panel.y + panel.height - 82.0f, 112.0f, 48.0f};
    }

    static Rectangle resetButton() {
        const Rectangle panel = editorPanel();
        return Rectangle{panel.x + 36.0f, panel.y + panel.height - 92.0f, 190.0f, 50.0f};
    }

    static Rectangle sliderTrack(Slider slider) {
        const Rectangle panel = editorPanel();
        const float y = slider == Slider::ShoulderWidth
            ? panel.y + 246.0f
            : slider == Slider::ShoulderHeight
                ? panel.y + 370.0f
                : panel.y + 494.0f;
        return Rectangle{panel.x + 42.0f, y, panel.width - 84.0f, 10.0f};
    }

    static float sliderMin(Slider slider) {
        switch (slider) {
            case Slider::ShoulderWidth: return character::kMinShoulderWidth;
            case Slider::ShoulderHeight: return character::kMinShoulderHeight;
            case Slider::ShoulderAngle: return character::kMinShoulderAngleDeg;
            case Slider::None: break;
        }
        return 0.0f;
    }

    static float sliderMax(Slider slider) {
        switch (slider) {
            case Slider::ShoulderWidth: return character::kMaxShoulderWidth;
            case Slider::ShoulderHeight: return character::kMaxShoulderHeight;
            case Slider::ShoulderAngle: return character::kMaxShoulderAngleDeg;
            case Slider::None: break;
        }
        return 1.0f;
    }

    float sliderValue(Slider slider) const {
        switch (slider) {
            case Slider::ShoulderWidth: return editingProfile_.appearance.shoulderWidth;
            case Slider::ShoulderHeight: return editingProfile_.appearance.shoulderHeight;
            case Slider::ShoulderAngle: return editingProfile_.appearance.shoulderAngleDeg;
            case Slider::None: break;
        }
        return 0.0f;
    }

    void setSliderValue(Slider slider, float value) {
        switch (slider) {
            case Slider::ShoulderWidth:
                editingProfile_.appearance.shoulderWidth = value;
                break;
            case Slider::ShoulderHeight:
                editingProfile_.appearance.shoulderHeight = value;
                break;
            case Slider::ShoulderAngle:
                editingProfile_.appearance.shoulderAngleDeg = value;
                break;
            case Slider::None:
                break;
        }
        editingProfile_.appearance =
            character::normalizeAppearance(editingProfile_.appearance);
    }

    static void drawText(TypographyStyleId style,
                         const std::string& text,
                         float x,
                         float y,
                         Color color) {
        TypographyService::shared().draw(style, text, Vector2{x, y}, color);
    }

    static void drawButton(const Rectangle& rect,
                           const std::string& label,
                           bool active,
                           bool enabled) {
        const Vector2 mouse = display::mousePosition();
        const bool hovered = enabled && CheckCollisionPointRec(mouse, rect);
        const Color fill = !enabled
            ? Color{38, 44, 58, 230}
            : active || hovered
                ? Color{58, 91, 142, 245}
                : Color{32, 42, 60, 240};
        DrawRectangleRounded(rect, 0.12f, 8, fill);
        DrawRectangleRoundedLines(rect,
                                  0.12f,
                                  8,
                                  enabled ? Color{96, 133, 190, 255}
                                          : Color{68, 74, 88, 255});
        const TypographyStyle& style =
            TypographyService::shared().style(TypographyStyleId::ButtonLabel);
        TypographyService::shared().drawCentered(TypographyStyleId::ButtonLabel,
                                                 label,
                                                 rect.x + rect.width * 0.5f,
                                                 rect.y + rect.height * 0.5f -
                                                     style.lineHeight * 0.5f,
                                                 enabled ? RAYWHITE
                                                         : Color{150, 158, 174, 255});
    }

    void reloadProfiles() {
        profiles_ = store_.loadProfiles();
        if (profiles_.empty()) {
            profiles_.push_back(character::defaultProfile());
        }
        selectedIndex_ = std::clamp(selectedIndex_, 0, static_cast<int>(profiles_.size() - 1u));
        editingProfile_ = profiles_[static_cast<std::size_t>(selectedIndex_)];
        nameEditing_ = false;
    }

    void selectProfile(int index) {
        if (index < 0 || index >= static_cast<int>(profiles_.size())) {
            return;
        }
        selectedIndex_ = index;
        editingProfile_ = profiles_[static_cast<std::size_t>(selectedIndex_)];
        nameEditing_ = false;
        statusMessage_.clear();
    }

    void createProfile() {
        character::CharacterProfile profile = editingProfile_;
        profile.builtIn = false;
        profile.name = "New Character";
        profile.id = store_.nextAvailableProfileId(profile.name);
        profile = character::normalizeProfile(profile);
        if (!store_.save(profile)) {
            statusMessage_ = "Could not create character preset.";
            return;
        }
        reloadProfiles();
        for (std::size_t index = 0; index < profiles_.size(); ++index) {
            if (profiles_[index].id == profile.id) {
                selectProfile(static_cast<int>(index));
                break;
            }
        }
        statusMessage_ = "Created character preset.";
    }

    void saveProfile() {
        character::CharacterProfile profile = character::normalizeProfile(editingProfile_);
        if (profile.builtIn) {
            profile.builtIn = false;
            if (profile.name.empty() || profile.name == "Default") {
                profile.name = "Default Copy";
            }
            profile.id = store_.nextAvailableProfileId(profile.name);
        }
        if (!store_.save(profile)) {
            statusMessage_ = "Could not save character preset.";
            return;
        }
        const std::string savedId = profile.id;
        reloadProfiles();
        for (std::size_t index = 0; index < profiles_.size(); ++index) {
            if (profiles_[index].id == savedId) {
                selectProfile(static_cast<int>(index));
                break;
            }
        }
        statusMessage_ = "Saved character preset.";
    }

    void deleteProfile() {
        if (editingProfile_.builtIn ||
            character::isBuiltInProfileId(editingProfile_.id)) {
            statusMessage_ = "Default character cannot be deleted.";
            return;
        }
        if (!store_.remove(editingProfile_.id)) {
            statusMessage_ = "Could not delete character preset.";
            return;
        }
        selectedIndex_ = 0;
        reloadProfiles();
        statusMessage_ = "Deleted character preset.";
    }

    void resetAppearance() {
        editingProfile_.appearance = character::defaultAppearance();
        statusMessage_ = "Reset visual shoulder values.";
    }

    void handleClick(Vector2 mouse) {
        if (CheckCollisionPointRec(mouse, backButton())) {
            return;
        }
        if (CheckCollisionPointRec(mouse, newButton())) {
            createProfile();
            return;
        }
        if (CheckCollisionPointRec(mouse, saveButton())) {
            saveProfile();
            return;
        }
        if (CheckCollisionPointRec(mouse, deleteButton())) {
            deleteProfile();
            return;
        }
        if (CheckCollisionPointRec(mouse, resetButton())) {
            resetAppearance();
            return;
        }
        if (CheckCollisionPointRec(mouse, nameField())) {
            nameEditing_ = true;
            return;
        }

        for (Slider slider : {Slider::ShoulderWidth, Slider::ShoulderHeight, Slider::ShoulderAngle}) {
            Rectangle bounds = sliderTrack(slider);
            bounds.y -= 18.0f;
            bounds.height += 36.0f;
            if (CheckCollisionPointRec(mouse, bounds)) {
                activeSlider_ = slider;
                updateSlider(slider, mouse.x);
                return;
            }
        }

        const Rectangle panel = listPanel();
        const float rowHeight = 58.0f;
        const float startY = panel.y + 78.0f;
        for (std::size_t index = 0; index < profiles_.size(); ++index) {
            const Rectangle row{
                panel.x + 24.0f,
                startY + static_cast<float>(index) * (rowHeight + 10.0f),
                panel.width - 48.0f,
                rowHeight
            };
            if (CheckCollisionPointRec(mouse, row)) {
                selectProfile(static_cast<int>(index));
                return;
            }
        }

        nameEditing_ = false;
    }

    void updateSlider(Slider slider, float mouseX) {
        const Rectangle track = sliderTrack(slider);
        const float t = std::clamp((mouseX - track.x) / track.width, 0.0f, 1.0f);
        setSliderValue(slider, sliderMin(slider) + (sliderMax(slider) - sliderMin(slider)) * t);
        statusMessage_.clear();
    }

    void handleNameInput() {
        if (!nameEditing_) {
            return;
        }

        int ch = GetCharPressed();
        while (ch > 0) {
            if (ch >= 32 && ch <= 126 && editingProfile_.name.size() < 32u) {
                editingProfile_.name.push_back(static_cast<char>(ch));
            }
            ch = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !editingProfile_.name.empty()) {
            editingProfile_.name.pop_back();
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            nameEditing_ = false;
        }
    }

    void renderBackdrop() const {
        for (int z = -24; z <= 24; z += 4) {
            DrawLine(0, 540 + z * 12, Config::SCREEN_WIDTH, 430 + z * 10, Color{20, 28, 42, 255});
        }
    }

    void renderProfileList() const {
        const Rectangle panel = listPanel();
        DrawRectangleRounded(panel, 0.035f, 12, Color{18, 24, 36, 245});
        DrawRectangleRoundedLines(panel, 0.035f, 12, Color{68, 90, 128, 255});
        drawText(TypographyStyleId::SectionTitle,
                 "Presets",
                 panel.x + 28.0f,
                 panel.y + 24.0f,
                 RAYWHITE);

        const float rowHeight = 58.0f;
        const float startY = panel.y + 78.0f;
        const Vector2 mouse = display::mousePosition();
        for (std::size_t index = 0; index < profiles_.size(); ++index) {
            const Rectangle row{
                panel.x + 24.0f,
                startY + static_cast<float>(index) * (rowHeight + 10.0f),
                panel.width - 48.0f,
                rowHeight
            };
            if (row.y + row.height > panel.y + panel.height - 104.0f) {
                break;
            }
            const bool selected = static_cast<int>(index) == selectedIndex_;
            const bool hovered = CheckCollisionPointRec(mouse, row);
            DrawRectangleRounded(row,
                                 0.08f,
                                 8,
                                 selected ? Color{52, 78, 118, 245}
                                          : hovered ? Color{34, 46, 66, 245}
                                                    : Color{26, 34, 48, 235});
            DrawRectangleRoundedLines(row,
                                      0.08f,
                                      8,
                                      selected ? SKYBLUE : Color{70, 84, 112, 255});
            const Rectangle thumb{
                row.x + 12.0f,
                row.y + 7.0f,
                64.0f,
                row.height - 14.0f
            };
            ui::drawCharacterSilhouetteThumbnail(
                thumb,
                profiles_[index].appearance,
                Color{255, 130, 72, 255},
                Fade(WHITE, selected ? 0.82f : 0.62f),
                selected ? Color{16, 24, 36, 255} : Color{18, 24, 36, 255});
            drawText(TypographyStyleId::Body,
                     profiles_[index].name,
                     row.x + 92.0f,
                     row.y + 15.0f,
                     profiles_[index].builtIn ? Color{220, 228, 240, 255} : RAYWHITE);
        }

        drawButton(newButton(), "New", false, true);
        drawButton(saveButton(), "Save", false, true);
        drawButton(deleteButton(), "Delete", false, !editingProfile_.builtIn);
    }

    void renderEditorPanel() const {
        const Rectangle panel = editorPanel();
        DrawRectangleRounded(panel, 0.035f, 12, Color{18, 24, 36, 245});
        DrawRectangleRoundedLines(panel, 0.035f, 12, Color{68, 90, 128, 255});
        drawText(TypographyStyleId::SectionTitle,
                 "Shape",
                 panel.x + 36.0f,
                 panel.y + 28.0f,
                 RAYWHITE);

        drawText(TypographyStyleId::Caption,
                 "Name",
                 nameField().x,
                 nameField().y - 28.0f,
                 Color{166, 178, 198, 255});
        DrawRectangleRounded(nameField(),
                             0.08f,
                             8,
                             nameEditing_ ? Color{38, 58, 86, 255}
                                          : Color{28, 36, 52, 245});
        DrawRectangleRoundedLines(nameField(),
                                  0.08f,
                                  8,
                                  nameEditing_ ? SKYBLUE : Color{74, 92, 124, 255});
        drawText(TypographyStyleId::Body,
                 editingProfile_.name,
                 nameField().x + 16.0f,
                 nameField().y + 15.0f,
                 RAYWHITE);

        renderSlider(Slider::ShoulderWidth, "Shoulder Width", "m");
        renderSlider(Slider::ShoulderHeight, "Shoulder Height", "m");
        renderSlider(Slider::ShoulderAngle, "Shoulder Angle", "deg");
        drawButton(resetButton(), "Reset Shape", false, true);
    }

    void renderSlider(Slider slider, const std::string& label, const std::string& unit) const {
        const Rectangle track = sliderTrack(slider);
        const float value = sliderValue(slider);
        const float t = (value - sliderMin(slider)) / (sliderMax(slider) - sliderMin(slider));
        const float handleX = track.x + track.width * std::clamp(t, 0.0f, 1.0f);
        const float labelY = track.y - 52.0f;

        drawText(TypographyStyleId::FieldLabel, label, track.x, labelY, RAYWHITE);
        char buffer[64]{};
        std::snprintf(buffer,
                      sizeof(buffer),
                      unit == "deg" ? "%.0f %s" : "%.2f %s",
                      value,
                      unit.c_str());
        const std::string valueLabel = buffer;
        const int textWidth =
            TypographyService::shared().measureWidth(TypographyStyleId::FieldLabel, valueLabel);
        drawText(TypographyStyleId::FieldLabel,
                 valueLabel,
                 track.x + track.width - static_cast<float>(textWidth),
                 labelY,
                 Color{244, 210, 130, 255});
        DrawRectangleRounded(track, 0.5f, 8, Color{52, 60, 74, 255});
        Rectangle fill = track;
        fill.width = handleX - track.x;
        DrawRectangleRounded(fill, 0.5f, 8, Color{76, 132, 220, 255});
        DrawCircleV(Vector2{handleX, track.y + track.height * 0.5f},
                    14.0f,
                    activeSlider_ == slider ? RAYWHITE : Color{220, 226, 236, 255});
    }

    void renderPreview() const {
        const Rectangle panel = previewPanel();
        DrawRectangleRounded(panel, 0.035f, 12, Color{18, 24, 36, 245});
        DrawRectangleRoundedLines(panel, 0.035f, 12, Color{68, 90, 128, 255});
        drawText(TypographyStyleId::SectionTitle,
                 "Preview",
                 panel.x + 36.0f,
                 panel.y + 28.0f,
                 RAYWHITE);

        const character::CharacterGeometry geometry =
            character::buildCharacterGeometry(editingProfile_.appearance);
        const float metersToPixels =
            std::min(210.0f, (panel.width - 180.0f) / character::kMaxShoulderWidth);
        const Vector2 root{
            panel.x + panel.width * 0.5f,
            panel.y + panel.height * 0.78f
        };
        auto project = [&](const sim::Vec3& point) {
            return Vector2{
                root.x + point.x * metersToPixels,
                root.y - point.y * metersToPixels
            };
        };

        const Color silhouette{255, 130, 72, 255};
        const Color silhouetteDark{160, 70, 42, 255};
        const Color guide{74, 92, 124, 170};
        DrawLineEx(Vector2{panel.x + 96.0f, root.y},
                   Vector2{panel.x + panel.width - 96.0f, root.y},
                   2.0f,
                   guide);

        const float torsoWidth = geometry.torso.radius * 2.0f * metersToPixels;
        const float torsoHeight =
            (geometry.torso.end.y - geometry.torso.start.y) * metersToPixels;
        const Rectangle torso{
            root.x - torsoWidth * 0.5f,
            root.y - torsoHeight,
            torsoWidth,
            torsoHeight
        };
        DrawRectangleRounded(torso, 0.45f, 14, silhouette);
        DrawRectangleRoundedLines(torso, 0.45f, 14, Fade(WHITE, 0.74f));

        auto drawShoulder = [&](const character::CharacterPrimitive& shoulder) {
            if (shoulder.radius <= 0.0f) {
                return;
            }
            const Vector2 start = project(shoulder.start);
            const Vector2 end = project(shoulder.end);
            const float thickness = std::max(6.0f, shoulder.radius * 2.0f * metersToPixels);
            DrawLineEx(start, end, thickness, silhouette);
            DrawCircleV(start, thickness * 0.5f, silhouette);
            DrawCircleV(end, thickness * 0.5f, silhouette);
            DrawLineEx(start, end, 2.0f, silhouetteDark);
        };
        drawShoulder(geometry.leftShoulder);
        drawShoulder(geometry.rightShoulder);

        const Vector2 head = project(geometry.head.start);
        DrawCircleV(head, geometry.head.radius * metersToPixels, silhouette);
        DrawCircleLines(static_cast<int>(head.x),
                        static_cast<int>(head.y),
                        geometry.head.radius * metersToPixels,
                        Fade(WHITE, 0.74f));
    }
};
