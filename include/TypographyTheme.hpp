#pragma once

#include <array>
#include <cstdint>

enum class TypographyFontRole : std::uint8_t {
    Regular = 0,
    SemiBold = 1,
    Bold = 2
};

enum class TypographyStyleId : std::uint8_t {
    AppTitle = 0,
    AppSubtitle = 1,
    MenuButton = 2,
    MenuShortcut = 3,
    MenuInstruction = 4,
    FieldLabel = 5,
    FieldValue = 6,
    SectionTitle = 7,
    Body = 8,
    Caption = 9,
    OverlayTitle = 10,
    OverlayBody = 11,
    OverlayAccent = 12,
    ScoreboardTitle = 13,
    ScoreboardSummary = 14,
    ScoreboardSubhead = 15,
    EditorTitle = 16,
    EditorBody = 17,
    Diagnostics = 18,
    ButtonLabel = 19,
    MenuTitle = 20,
    ScreenTitle = 21
};

struct TypographyStyle {
    TypographyStyleId id{};
    TypographyFontRole fontRole{TypographyFontRole::Regular};
    int fontSize{20};
    float spacing{1.0f};
    float lineHeight{24.0f};
};

class TypographyTheme {
public:
    static constexpr const char* kDefaultFamily = "Inter";

    static const TypographyStyle& style(TypographyStyleId id) {
        static const std::array<TypographyStyle, 22> kStyles{
            TypographyStyle{TypographyStyleId::AppTitle, TypographyFontRole::Bold, 72, 1.0f, 78.0f},
            TypographyStyle{TypographyStyleId::AppSubtitle, TypographyFontRole::Regular, 25, 1.0f, 32.0f},
            TypographyStyle{TypographyStyleId::MenuButton, TypographyFontRole::SemiBold, 34, 1.0f, 40.0f},
            TypographyStyle{TypographyStyleId::MenuShortcut, TypographyFontRole::Regular, 20, 1.0f, 26.0f},
            TypographyStyle{TypographyStyleId::MenuInstruction, TypographyFontRole::Regular, 21, 1.0f, 28.0f},
            TypographyStyle{TypographyStyleId::FieldLabel, TypographyFontRole::SemiBold, 22, 1.0f, 28.0f},
            TypographyStyle{TypographyStyleId::FieldValue, TypographyFontRole::Regular, 25, 1.0f, 32.0f},
            TypographyStyle{TypographyStyleId::SectionTitle, TypographyFontRole::SemiBold, 24, 1.0f, 30.0f},
            TypographyStyle{TypographyStyleId::Body, TypographyFontRole::Regular, 21, 1.0f, 28.0f},
            TypographyStyle{TypographyStyleId::Caption, TypographyFontRole::Regular, 18, 1.0f, 24.0f},
            TypographyStyle{TypographyStyleId::OverlayTitle, TypographyFontRole::Bold, 28, 1.0f, 34.0f},
            TypographyStyle{TypographyStyleId::OverlayBody, TypographyFontRole::Regular, 21, 1.0f, 28.0f},
            TypographyStyle{TypographyStyleId::OverlayAccent, TypographyFontRole::SemiBold, 22, 1.0f, 28.0f},
            TypographyStyle{TypographyStyleId::ScoreboardTitle, TypographyFontRole::Bold, 32, 1.0f, 38.0f},
            TypographyStyle{TypographyStyleId::ScoreboardSummary, TypographyFontRole::Bold, 34, 1.0f, 40.0f},
            TypographyStyle{TypographyStyleId::ScoreboardSubhead, TypographyFontRole::Regular, 18, 1.0f, 22.0f},
            TypographyStyle{TypographyStyleId::EditorTitle, TypographyFontRole::Bold, 40, 1.0f, 46.0f},
            TypographyStyle{TypographyStyleId::EditorBody, TypographyFontRole::Regular, 19, 1.0f, 25.0f},
            TypographyStyle{TypographyStyleId::Diagnostics, TypographyFontRole::Regular, 18, 1.0f, 22.0f},
            TypographyStyle{TypographyStyleId::ButtonLabel, TypographyFontRole::SemiBold, 30, 1.0f, 36.0f},
            TypographyStyle{TypographyStyleId::MenuTitle, TypographyFontRole::Bold, 80, 1.0f, 86.0f},
            TypographyStyle{TypographyStyleId::ScreenTitle, TypographyFontRole::Bold, 62, 1.0f, 68.0f},
        };

        return kStyles.at(static_cast<std::size_t>(id));
    }

    static constexpr const char* family() {
        return kDefaultFamily;
    }
};
