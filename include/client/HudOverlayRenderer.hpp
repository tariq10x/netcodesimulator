#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include <raylib.h>

#include "Config3D.hpp"
#include "TypographyService.hpp"
#include "client/RenderFrame.hpp"

namespace client {

class HudOverlayRenderer {
public:
    static void renderCompactScore(const CompactScoreOverlayFrame& frame) {
        if (!frame.visible) {
            return;
        }

        const Rectangle shadow{Config::SCREEN_WIDTH - 392.0f, 24.0f, 360.0f, 136.0f};
        const Rectangle card{shadow.x, shadow.y - 4.0f, shadow.width, shadow.height};
        DrawRectangleRounded(shadow, 0.20f, 12, Fade(BLACK, 0.22f));
        DrawRectangleRounded(card, 0.20f, 12, Fade(Color{12, 16, 24, 255}, 0.88f));
        DrawRectangleRoundedLines(card, 0.20f, 12, Fade(WHITE, 0.12f));

        drawText(TypographyStyleId::Caption,
                 "MATCH",
                 Vector2{card.x + 18.0f, card.y + 12.0f},
                 Fade(WHITE, 0.7f));

        drawText(TypographyStyleId::ScoreboardSummary,
                 paddedScore(frame.score.attackerScore),
                 Vector2{card.x + 18.0f, card.y + 36.0f},
                 teamColor(sim::TeamId::Attacker));
        drawText(TypographyStyleId::Caption,
                 "ATK",
                 Vector2{card.x + 18.0f, card.y + 84.0f},
                 Fade(teamColor(sim::TeamId::Attacker), 0.8f));

        const std::string defenderScore = paddedScore(frame.score.defenderScore);
        const int defenderWidth =
            TypographyService::shared().measureWidth(TypographyStyleId::ScoreboardSummary,
                                                     defenderScore);
        drawText(TypographyStyleId::ScoreboardSummary,
                 defenderScore,
                 Vector2{card.x + card.width - 18.0f - static_cast<float>(defenderWidth),
                         card.y + 36.0f},
                 teamColor(sim::TeamId::Defender));
        drawText(TypographyStyleId::Caption,
                 "DEF",
                 Vector2{card.x + card.width - 58.0f, card.y + 84.0f},
                 Fade(teamColor(sim::TeamId::Defender), 0.8f));

        const std::string scoreDivider = ":";
        const int dividerWidth =
            TypographyService::shared().measureWidth(TypographyStyleId::ScoreboardTitle,
                                                     scoreDivider);
        drawText(TypographyStyleId::ScoreboardTitle,
                 scoreDivider,
                 Vector2{card.x + (card.width * 0.5f) - (static_cast<float>(dividerWidth) * 0.5f),
                         card.y + 38.0f},
                 Fade(WHITE, 0.9f));

        const Rectangle localStrip{card.x + 14.0f, card.y + 102.0f, card.width - 28.0f, 28.0f};
        DrawRectangleRounded(localStrip,
                             0.45f,
                             8,
                             Fade(teamColor(frame.score.localTeam), 0.12f));

        std::string localIdentity = frame.score.localIdentity.empty()
            ? "Local"
            : frame.score.localIdentity;
        if (frame.score.localIdentity.empty()) {
            localIdentity += " Player";
        }
        drawText(TypographyStyleId::Caption,
                 localIdentity,
                 Vector2{localStrip.x + 10.0f, localStrip.y + 1.0f},
                 frame.score.localAlive ? RAYWHITE : Fade(RAYWHITE, 0.7f));

        const std::string localRecord =
            "K/D " + std::to_string(frame.score.localKills) + "/" +
            std::to_string(frame.score.localDeaths);
        const int localRecordWidth =
            TypographyService::shared().measureWidth(TypographyStyleId::Caption, localRecord);
        drawText(TypographyStyleId::Caption,
                 localRecord,
                 Vector2{localStrip.x + localStrip.width - 10.0f -
                             static_cast<float>(localRecordWidth),
                         localStrip.y + 1.0f},
                 Fade(WHITE, 0.8f));
    }

    static void renderKillFeed(const KillFeedOverlayFrame& frame) {
        if (!frame.visible || frame.entries.empty()) {
            return;
        }

        float y = 26.0f;
        const std::size_t visibleEntries = std::min<std::size_t>(frame.entries.size(), 4u);
        for (std::size_t index = 0; index < visibleEntries; ++index) {
            const KillFeedEntryView& entry = frame.entries[index];
            const Rectangle shadow{24.0f, y + 3.0f, 388.0f, 44.0f};
            const Rectangle row{24.0f, y, 388.0f, 44.0f};
            const Color rowTint = entry.attackerIsLocalPlayer || entry.victimIsLocalPlayer
                ? Fade(Color{90, 126, 178, 255}, 0.36f)
                : Fade(Color{12, 16, 24, 255}, 0.82f);

            DrawRectangleRounded(shadow, 0.24f, 8, Fade(BLACK, 0.16f));
            DrawRectangleRounded(row, 0.24f, 8, rowTint);
            DrawRectangleRoundedLines(row, 0.24f, 8, Fade(WHITE, 0.1f));

            const std::string attackerLabel =
                entry.attackerLabel.empty() ? "Unknown" : entry.attackerLabel;
            const std::string victimLabel =
                entry.victimLabel.empty() ? "Unknown" : entry.victimLabel;
            const std::string separator = " x ";
            const int attackerWidth =
                TypographyService::shared().measureWidth(TypographyStyleId::Body, attackerLabel);
            const int separatorWidth =
                TypographyService::shared().measureWidth(TypographyStyleId::Body, separator);

            const TypographyStyle& bodyStyle =
                TypographyService::shared().style(TypographyStyleId::Body);
            const float baselineY = row.y + row.height * 0.5f - bodyStyle.lineHeight * 0.5f;
            const float startX = row.x + 12.0f;
            drawText(TypographyStyleId::Body,
                     attackerLabel,
                     Vector2{startX, baselineY},
                     teamColor(entry.attackerTeam));
            drawText(TypographyStyleId::Body,
                     separator,
                     Vector2{startX + static_cast<float>(attackerWidth), baselineY},
                     Fade(WHITE, 0.78f));
            drawText(TypographyStyleId::Body,
                     victimLabel,
                     Vector2{startX + static_cast<float>(attackerWidth + separatorWidth),
                             baselineY},
                     teamColor(entry.victimTeam));
            y += 50.0f;
        }
    }

    static void renderScoreboard(const ScoreboardOverlayFrame& frame) {
        if (!frame.visible) {
            return;
        }

        auto findSection = [&frame](sim::TeamId team) -> const ScoreboardSectionView* {
            const auto it = std::find_if(frame.sections.begin(),
                                         frame.sections.end(),
                                         [team](const ScoreboardSectionView& section) {
                                             return section.team == team;
                                         });
            return it != frame.sections.end() ? &(*it) : nullptr;
        };

        const ScoreboardSectionView emptyAttackerSection{sim::TeamId::Attacker, 0u, {}};
        const ScoreboardSectionView emptyDefenderSection{sim::TeamId::Defender, 0u, {}};
        const ScoreboardSectionView* foundAttackerSection = findSection(sim::TeamId::Attacker);
        const ScoreboardSectionView* foundDefenderSection = findSection(sim::TeamId::Defender);
        const ScoreboardSectionView& attackerSection =
            foundAttackerSection != nullptr ? *foundAttackerSection : emptyAttackerSection;
        const ScoreboardSectionView& defenderSection =
            foundDefenderSection != nullptr ? *foundDefenderSection : emptyDefenderSection;

        const int attackerCount = static_cast<int>(attackerSection.entries.size());
        const int defenderCount = static_cast<int>(defenderSection.entries.size());
        const int maxColumnRows = std::max(attackerCount, defenderCount);
        const int overlayWidth = std::min(Config::SCREEN_WIDTH - 120, 1100);
        const int overlayHeight = std::min(Config::SCREEN_HEIGHT - 120,
                                           std::max(300, 168 + (maxColumnRows * 38)));
        const int overlayX = (Config::SCREEN_WIDTH - overlayWidth) / 2;
        const int overlayY = 72;
        const int outerPadding = 24;
        const int columnGap = 20;
        const int columnWidth = (overlayWidth - (outerPadding * 2) - columnGap) / 2;

        DrawRectangle(0,
                      0,
                      Config::SCREEN_WIDTH,
                      Config::SCREEN_HEIGHT,
                      Fade(BLACK, 0.18f));

        const Rectangle overlayRect{
            static_cast<float>(overlayX),
            static_cast<float>(overlayY),
            static_cast<float>(overlayWidth),
            static_cast<float>(overlayHeight)
        };
        DrawRectangleRounded(overlayRect, 0.04f, 10, Fade(Color{10, 16, 24, 255}, 0.62f));
        DrawRectangleRoundedLines(overlayRect, 0.04f, 10, Fade(Color{255, 210, 120, 255}, 0.85f));

        drawText(TypographyStyleId::ScoreboardTitle,
                 "SCOREBOARD",
                 Vector2{static_cast<float>(overlayX + outerPadding),
                         static_cast<float>(overlayY + 18)},
                 RAYWHITE);

        const std::string summary = std::to_string(frame.attackerScore) +
                                    "  -  " +
                                    std::to_string(frame.defenderScore);
        TypographyService::shared().drawCentered(TypographyStyleId::ScoreboardSummary,
                                                 summary,
                                                 static_cast<float>(overlayX + (overlayWidth / 2)),
                                                 static_cast<float>(overlayY + 14),
                                                 Color{255, 210, 120, 255});

        const int readableColumnY = overlayY + 104;
        const int readableColumnHeight = overlayHeight - 128;

        const auto drawTeamColumn = [=](
            const ScoreboardSectionView& section,
            int columnX,
            int columnWidth,
            Color tint) {
            const Rectangle columnRect{
                static_cast<float>(columnX),
                static_cast<float>(readableColumnY),
                static_cast<float>(columnWidth),
                static_cast<float>(readableColumnHeight)
            };
            DrawRectangleRounded(columnRect, 0.035f, 10, Fade(tint, 0.17f));
            DrawRectangleRoundedLines(columnRect, 0.035f, 10, Fade(tint, 0.9f));

            const std::string header =
                std::string(teamLabel(section.team)) + "  " + std::to_string(section.score);
            drawText(TypographyStyleId::OverlayTitle,
                     header,
                     Vector2{static_cast<float>(columnX + 18),
                             static_cast<float>(readableColumnY + 16)},
                     tint);

            int rowY = readableColumnY + 66;
            if (section.entries.empty()) {
                drawText(TypographyStyleId::Body,
                         "(empty)",
                         Vector2{static_cast<float>(columnX + 18), static_cast<float>(rowY)},
                         Fade(LIGHTGRAY, 0.75f));
                return;
            }

            for (const auto& entry : section.entries) {
                const Color rowColor = entry.isLocalPlayer ? RAYWHITE : Fade(LIGHTGRAY, 0.92f);
                drawText(TypographyStyleId::Body,
                         entry.rowLabel,
                         Vector2{static_cast<float>(columnX + 18), static_cast<float>(rowY)},
                         rowColor);
                rowY += 36;
            }
        };

        drawTeamColumn(attackerSection,
                       overlayX + outerPadding,
                       columnWidth,
                       teamColor(sim::TeamId::Attacker));
        drawTeamColumn(defenderSection,
                       overlayX + outerPadding + columnWidth + columnGap,
                       columnWidth,
                       teamColor(sim::TeamId::Defender));
    }

private:
    static void drawText(TypographyStyleId styleId,
                         std::string_view text,
                         Vector2 position,
                         Color color) {
        TypographyService::shared().draw(styleId, text, position, color);
    }

    static Color teamColor(sim::TeamId team) {
        switch (team) {
            case sim::TeamId::Attacker:
                return Color{255, 119, 72, 255};
            case sim::TeamId::Defender:
                return Color{72, 156, 255, 255};
            case sim::TeamId::Spectator:
            case sim::TeamId::None:
                return Color{180, 180, 180, 255};
        }
        return Color{180, 180, 180, 255};
    }

    static const char* teamLabel(sim::TeamId team) {
        return sim::toString(team);
    }

    static std::string paddedScore(std::uint16_t score) {
        return score < 10u ? "0" + std::to_string(score) : std::to_string(score);
    }
};

}  // namespace client
