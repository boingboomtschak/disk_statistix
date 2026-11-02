#pragma once
// "Sonoran Gothic" (the user's Windows Terminal scheme) applied to the UI and the treemap.
#include "imgui.h"

namespace theme {

constexpr ImU32 Rgb(uint32_t rgb, int a = 255) {
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a);
}
inline ImVec4 Vec(uint32_t rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a);
}

// Terminal scheme
constexpr uint32_t kBackground = 0x241F17, kForeground = 0xEAF4DE, kSelection = 0x574B37;
constexpr uint32_t kBlack = 0x6B6658, kRed = 0xC45D5D, kGreen = 0x61944C, kYellow = 0xC7B07B, kBlue = 0x207549,
                   kPurple = 0x8B90B8, kCyan = 0xBD7852, kWhite = 0xBFC7B5;
constexpr uint32_t kBrightBlack = 0xABA28C, kBrightRed = 0xF57474, kBrightGreen = 0x8AD46D, kBrightYellow = 0xFCDF9C,
                   kBrightBlue = 0x2FAB6B, kBrightPurple = 0xB9C0F5, kBrightCyan = 0xF59C6B, kBrightWhite = 0xF5FFE8;

// Derived shades for surfaces
constexpr uint32_t kDeep = 0x16120C;     // map background
constexpr uint32_t kSurface = 0x2E2820;  // popups, headers
constexpr uint32_t kRaised = 0x3A3226;   // buttons, frames
constexpr uint32_t kBorder = 0x4A4032;
constexpr uint32_t kActive = 0x6B5C43;

// File-type palette, biggest type first; alternates hue families so neighbours differ.
constexpr uint32_t kPalette[] = {kBrightBlue, kRed,         kYellow, kPurple,     kCyan,  kBrightGreen,
                                 kBrightPurple, kBrightRed, kBrightYellow, kBlue, kBrightCyan, kGreen,
                                 kBrightBlack, kWhite,      0x9C6B8E, 0x5E8C8A};
constexpr uint32_t kOtherType = 0x5C5548;  // everything past the palette
constexpr uint32_t kFolder = 0x8C7A5A;

// Semantic colours
constexpr uint32_t kOk = kBrightGreen, kError = kBrightRed, kWarn = kBrightCyan;
constexpr uint32_t kSelect = kBrightYellow;  // selection outline
constexpr uint32_t kDup = kBrightPurple;     // duplicate outlines / text
constexpr uint32_t kDupHeader = 0x4C507A;    // duplicate folder title strip
constexpr uint32_t kDanger = 0xA8423F, kDangerHover = kRed, kDangerActive = 0xD86A66;

inline void Apply(ImGuiStyle& st) {
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = Vec(kForeground);
    c[ImGuiCol_TextDisabled] = Vec(kBrightBlack);
    c[ImGuiCol_WindowBg] = Vec(kBackground);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = Vec(kSurface, 0.98f);
    c[ImGuiCol_Border] = Vec(kBorder);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = Vec(kRaised);
    c[ImGuiCol_FrameBgHovered] = Vec(kSelection);
    c[ImGuiCol_FrameBgActive] = Vec(kActive);
    c[ImGuiCol_TitleBg] = Vec(kSurface);
    c[ImGuiCol_TitleBgActive] = Vec(kRaised);
    c[ImGuiCol_TitleBgCollapsed] = Vec(kSurface);
    c[ImGuiCol_MenuBarBg] = Vec(kSurface);
    c[ImGuiCol_ScrollbarBg] = Vec(kBackground, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = Vec(kBorder);
    c[ImGuiCol_ScrollbarGrabHovered] = Vec(kSelection);
    c[ImGuiCol_ScrollbarGrabActive] = Vec(kActive);
    c[ImGuiCol_CheckMark] = Vec(kBrightGreen);
    c[ImGuiCol_SliderGrab] = Vec(kBrightBlue);
    c[ImGuiCol_SliderGrabActive] = Vec(kBrightGreen);
    c[ImGuiCol_Button] = Vec(kRaised);
    c[ImGuiCol_ButtonHovered] = Vec(kSelection);
    c[ImGuiCol_ButtonActive] = Vec(kActive);
    c[ImGuiCol_Header] = Vec(kSelection);
    c[ImGuiCol_HeaderHovered] = Vec(kSelection, 0.7f);
    c[ImGuiCol_HeaderActive] = Vec(kActive);
    c[ImGuiCol_Separator] = Vec(kBorder);
    c[ImGuiCol_SeparatorHovered] = Vec(kSelection);
    c[ImGuiCol_SeparatorActive] = Vec(kActive);
    c[ImGuiCol_ResizeGrip] = Vec(kBorder, 0.5f);
    c[ImGuiCol_ResizeGripHovered] = Vec(kSelection);
    c[ImGuiCol_ResizeGripActive] = Vec(kActive);
    c[ImGuiCol_Tab] = Vec(kSurface);
    c[ImGuiCol_TabHovered] = Vec(kSelection);
    c[ImGuiCol_TabSelected] = Vec(kRaised);
    c[ImGuiCol_TabSelectedOverline] = Vec(kBrightYellow);
    c[ImGuiCol_TabDimmed] = Vec(kSurface);
    c[ImGuiCol_TabDimmedSelected] = Vec(kRaised);
    c[ImGuiCol_PlotHistogram] = Vec(kBrightBlue);
    c[ImGuiCol_PlotHistogramHovered] = Vec(kBrightGreen);
    c[ImGuiCol_TableHeaderBg] = Vec(kSurface);
    c[ImGuiCol_TableBorderStrong] = Vec(kBorder);
    c[ImGuiCol_TableBorderLight] = Vec(kBorder, 0.6f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = Vec(kForeground, 0.03f);
    c[ImGuiCol_TextSelectedBg] = Vec(kSelection);
    c[ImGuiCol_NavCursor] = Vec(kBrightYellow);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.05f, 0.04f, 0.02f, 0.6f);
}

}  // namespace theme
