#include "ui/widget_renderer.hpp"

#include "ui/widget_tree.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/vk_context.hpp"
#include "core/app_clock.hpp"
#include "ui/interface_fonts.hpp"
#include "core/logger.hpp"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <cmath>
#include <vector>

namespace wowee {
namespace ui {

namespace {

/// A path that resolved to nothing. Stored rather than retried, so one bad
/// SetTexture in an addon does not read a missing file every frame forever.
constexpr VkDescriptorSet kMissing = VK_NULL_HANDLE;

uint32_t packColor(const float rgba[4], float alpha) {
    auto ch = [](float v) {
        return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return IM_COL32(ch(rgba[0]), ch(rgba[1]), ch(rgba[2]), ch(rgba[3] * alpha));
}

} // namespace

void WidgetRenderer::initialize(pipeline::AssetManager* assets,
                                rendering::VkContext* vkCtx) {
    assets_ = assets;
    vkCtx_ = vkCtx;
}

VkDescriptorSet WidgetRenderer::resident(const std::string& path) const {
    if (path.empty()) return kMissing;
    auto it = textures_.find(path);
    return (it == textures_.end()) ? kMissing : it->second;
}

VkDescriptorSet WidgetRenderer::texture(const std::string& path) {
    auto it = textures_.find(path);
    if (it != textures_.end()) return it->second;
    if (!assets_ || !vkCtx_ || path.empty()) return kMissing;

    // Addons write "Interface\\Foo\\Bar" without the extension as often as with
    // it, and the real client accepts both.
    std::string resolved = path;
    const bool hasExt = resolved.size() > 4 &&
        (resolved.compare(resolved.size() - 4, 4, ".blp") == 0 ||
         resolved.compare(resolved.size() - 4, 4, ".BLP") == 0);
    if (!hasExt) resolved += ".blp";

    auto data = assets_->readFile(resolved);
    if (data.empty()) {
        LOG_WARNING("Widget texture not found: ", path);
        textures_[path] = kMissing;
        return kMissing;
    }
    auto image = pipeline::BLPLoader::load(data);
    if (!image.isValid()) {
        LOG_WARNING("Widget texture unreadable: ", resolved);
        textures_[path] = kMissing;
        return kMissing;
    }
    VkDescriptorSet set = vkCtx_->uploadImGuiTexture(image.data.data(),
                                                     image.width, image.height);
    textures_[path] = set;
    return set;
}


void WidgetRenderer::drawBackdrop(ImDrawList* dl, const Widget& w,
                                  float x0, float y0, float x1, float y1) {
    // Background sits inside the insets, which is what keeps it from showing
    // through the border drawn over it.
    const float bx0 = x0 + w.insetLeft;
    const float by0 = y0 + w.insetTop;
    const float bx1 = x1 - w.insetRight;
    const float by1 = y1 - w.insetBottom;
    if (bx1 > bx0 && by1 > by0) {
        VkDescriptorSet bg = resident(w.bgFile);
        const uint32_t col = packColor(w.backdropColor, w.alpha);
        if (bg != kMissing) {
            // Tiling repeats the art at its own size instead of stretching it,
            // which is the difference between a stone wall and a smear.
            float u1 = 1.0f, v1 = 1.0f;
            if (w.tileBackground && w.edgeSize > 0.0f) {
                u1 = (bx1 - bx0) / w.edgeSize;
                v1 = (by1 - by0) / w.edgeSize;
            }
            dl->AddImage(reinterpret_cast<ImTextureID>(bg), ImVec2(bx0, by0), ImVec2(bx1, by1),
                         ImVec2(0.0f, 0.0f), ImVec2(u1, v1), col);
        }
        // A backdrop with no background file has no background — only its edge.
        // Filling the rect instead painted every such frame in the backdrop
        // colour, which defaults to opaque white, and a wide one is a white
        // slab across the screen. Nothing is drawn here either while the art is
        // still being read.
    }

    VkDescriptorSet edge = resident(w.edgeFile);
    if (edge == kMissing || w.edgeSize <= 0.0f) return;

    // The edge file is eight square tiles in a row. Measured against the art
    // rather than assumed: UI-Tooltip-Border is 128x16 and UI-DialogBox-Border
    // 256x32, both exactly eight tiles wide.
    const float e = w.edgeSize;
    const uint32_t col = packColor(w.borderColor, w.alpha);
    auto piece = [&](int index, float px0, float py0, float px1, float py1) {
        const float u0 = index / 8.0f, u1 = (index + 1) / 8.0f;
        dl->AddImage(reinterpret_cast<ImTextureID>(edge), ImVec2(px0, py0), ImVec2(px1, py1),
                     ImVec2(u0, 0.0f), ImVec2(u1, 1.0f), col);
    };
    // Edges first, then corners over them, so a corner is never clipped by the
    // run it meets.
    piece(0, x0,     y0 + e, x0 + e, y1 - e);   // left
    piece(1, x1 - e, y0 + e, x1,     y1 - e);   // right
    piece(2, x0 + e, y0,     x1 - e, y0 + e);   // top
    piece(3, x0 + e, y1 - e, x1 - e, y1);       // bottom
    piece(4, x0,     y0,     x0 + e, y0 + e);   // top-left
    piece(5, x1 - e, y0,     x1,     y0 + e);   // top-right
    piece(6, x0,     y1 - e, x0 + e, y1);       // bottom-left
    piece(7, x1 - e, y1 - e, x1,     y1);       // bottom-right
}

void WidgetRenderer::drawStatusBar(ImDrawList* dl, const Widget& w,
                                   float x0, float y0, float x1, float y1) {
    const float f = w.barFraction();
    if (f <= 0.0f) return;
    // Horizontal bars fill from the left; vertical ones from the bottom, which
    // in screen terms means growing upward from y1.
    const float fx1 = w.barVertical ? x1 : x0 + (x1 - x0) * f;
    const float fy0 = w.barVertical ? y1 - (y1 - y0) * f : y0;
    const uint32_t col = packColor(w.barColor, w.alpha);

    VkDescriptorSet tex = resident(w.barTexture);
    if (tex != kMissing) {
        // The texture is cropped to the filled part rather than squashed into
        // it, so a bar at half value shows half its art at its own scale.
        const float u1 = w.barVertical ? 1.0f : f;
        const float v0 = w.barVertical ? (1.0f - f) : 0.0f;
        dl->AddImage(reinterpret_cast<ImTextureID>(tex), ImVec2(x0, fy0), ImVec2(fx1, y1),
                     ImVec2(0.0f, v0), ImVec2(u1, 1.0f), col);
    } else if (w.barTexture.empty()) {
        dl->AddRectFilled(ImVec2(x0, fy0), ImVec2(fx1, y1), col);
    }
}

void WidgetRenderer::drawSlider(ImDrawList* dl, const Widget& w,
                                float x0, float y0, float x1, float y1) {
    VkDescriptorSet thumb = resident(w.thumbTexture);
    if (thumb == kMissing) return;

    // The thumb sits at the value along the track, and is as wide as the track
    // is narrow — a scroll bar's grip is square to its channel.
    const float f = w.barFraction();
    const uint32_t col = packColor(w.barColor, w.alpha);
    if (w.barVertical) {
        const float size = x1 - x0;
        // Screen y grows downward while a slider's value grows upward, so the
        // full value belongs at the top of the track.
        const float span = (y1 - y0) - size;
        const float top = y1 - size - f * span;
        dl->AddImage(reinterpret_cast<ImTextureID>(thumb), ImVec2(x0, top),
                     ImVec2(x1, top + size), ImVec2(0, 0), ImVec2(1, 1), col);
    } else {
        const float size = y1 - y0;
        const float span = (x1 - x0) - size;
        const float left = x0 + f * span;
        dl->AddImage(reinterpret_cast<ImTextureID>(thumb), ImVec2(left, y0),
                     ImVec2(left + size, y1), ImVec2(0, 0), ImVec2(1, 1), col);
    }
}

void WidgetRenderer::drawCooldown(ImDrawList* dl, const Widget& w,
                                  float x0, float y0, float x1, float y1) {
    if (w.cooldownDuration <= 0.0) return;
    const double elapsed = core::appTimeSeconds() - w.cooldownStart;
    if (elapsed < 0.0 || elapsed >= w.cooldownDuration) return;
    const float remaining =
        1.0f - static_cast<float>(elapsed / w.cooldownDuration);

    // A wedge from twelve o'clock, shrinking clockwise as the time runs out.
    // Drawn to the corners rather than to an inscribed circle — the thing being
    // covered is a square icon, and a circle would leave its corners lit — and
    // clipped to the frame so the overrun does not spill onto its neighbours.
    const ImVec2 centre((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
    const float radius = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    constexpr float kTwoPi = 6.2831853f;
    constexpr int kSegments = 48;
    const int used = std::max(1, static_cast<int>(kSegments * remaining));

    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    dl->PathClear();
    dl->PathLineTo(centre);
    for (int i = 0; i <= used; ++i) {
        // -pi/2 starts at the top; positive sweep runs clockwise on a screen
        // whose y grows downward.
        const float a = -kTwoPi * 0.25f +
                        kTwoPi * remaining * (static_cast<float>(i) / used);
        dl->PathLineTo(ImVec2(centre.x + std::cos(a) * radius,
                              centre.y + std::sin(a) * radius));
    }
    dl->PathFillConvex(IM_COL32(0, 0, 0, 160));
    dl->PopClipRect();
}

void WidgetRenderer::render(WidgetTree& tree, float screenW, float screenH) {
    // ImGui's backend is torn down and restarted when the anti-aliasing
    // changes, and every descriptor set in this cache came from the pool that
    // went with it. Holding them past that point means drawing with freed
    // descriptors, which the GPU answers by being reset.
    if (vkCtx_) {
        const uint32_t generation = vkCtx_->imguiBackendGeneration();
        if (generation != imguiGenerationSeen_) {
            imguiGenerationSeen_ = generation;
            if (!textures_.empty()) {
                LOG_WARNING("ImGui's backend restarted; dropping ",
                            textures_.size(),
                            " cached textures rather than drawing with the "
                            "descriptor sets it freed");
                textures_.clear();
            }
        }
    }

    tree.layout(screenW, screenH);
    const auto& order = tree.drawOrder();
    if (order.empty()) return;

    // Resolve textures before recording anything, and only a few per frame.
    //
    // Uploading one ends in vkDeviceWaitIdle. Doing that from inside the draw
    // loop stalls the whole device in the middle of building a frame, which is
    // the shape of problem this renderer has already been bitten by once —
    // enough synchronous submits in a row and the main loop stalls, a fence wait
    // fails, and the device is lost. Hoisting them out means the wait happens
    // between frames instead of during one, and the budget means a screen full
    // of new art costs several quiet frames rather than one very long one.
    constexpr int kUploadsPerFrame = 8;
    std::vector<const std::string*> wanted;
    wanted.reserve(kUploadsPerFrame);
    auto want = [&](const std::string& path) {
        if (static_cast<int>(wanted.size()) >= kUploadsPerFrame || path.empty()) return;
        if (textures_.find(path) != textures_.end()) return;
        for (const std::string* p : wanted) if (*p == path) return;
        wanted.push_back(&path);
    };
    for (const Widget* w : order) {
        if (static_cast<int>(wanted.size()) >= kUploadsPerFrame) break;
        if (w->kind == WidgetKind::Texture && !w->solidColor) want(w->texturePath);
        if (w->kind == WidgetKind::Frame) {
            if (w->hasBackdrop) { want(w->bgFile); want(w->edgeFile); }
            if (w->isStatusBar) want(w->barTexture);
            if (w->isSlider) want(w->thumbTexture);
        }
    }

    // One submit and one wait for the whole batch rather than one of each per
    // texture. Every upload used to be its own immediate submit, and with
    // FrameXML asking for hundreds of distinct files the seconds after a load
    // cost 70-140ms a frame. Batched, how many go in a frame stops mattering
    // much, which is why the budget can be larger and the burst shorter.
    //
    // Synchronous, because the draw below uses whatever was just uploaded; the
    // asynchronous form would let this frame sample an image whose copy has not
    // landed. Nothing to upload means no batch at all, so an idle frame does
    // not allocate a command buffer to record nothing into.
    if (!wanted.empty() && vkCtx_) {
        vkCtx_->beginUploadBatch();
        for (const std::string* path : wanted) texture(*path);
        vkCtx_->endUploadBatchSync();
    }

    // Interface units to pixels. The tree is laid out against a virtual screen
    // 768 units tall so a frame is the same apparent size on every display;
    // this is the one place that becomes pixels.
    const float s = tree.uiScale();

    // What is actually on screen, named, once, when asked for.
    //
    // A stray label is very hard to identify from a screenshot: the text says
    // what it says, and nothing says which frame put it there. This lists every
    // drawn widget with its name, its rect and its text, which turns "what is
    // that in the middle of the screen" into one line of log.
    // 1 lists what is drawn; 2 lists every named widget whether drawn or not,
    // which is what shows a container's own rect — a frame paints nothing
    // itself, so the thing that mispositioned everything under it never
    // appears in a list of what was painted.
    static const int dumpWidgets = [] {
        const char* v = std::getenv("WOWEE_WIDGET_DUMP");
        if (!v || !*v) return 0;
        return std::atoi(v);
    }();
    // Not on the first frame. Textures upload a few per frame, so a dump taken
    // immediately reports nothing resident and says only that the load had not
    // finished — which is true and useless. A couple of seconds in, what is
    // missing is missing for a reason.
    static int framesSeen = 0;
    static bool dumped = false;
    ++framesSeen;
    if (dumpWidgets && !dumped && framesSeen > 30) {
        dumped = true;
        // The screen it was laid out against, because a coordinate means
        // nothing without it: 1920 is the middle of one display and off
        // the edge of another.
        LOG_WARNING("WidgetDump: ", order.size(), " widgets drawn on ",
                    screenW, "x", screenH, " px, ", screenW / s, "x",
                    screenH / s, " units (scale ", s, "), ",
                    textures_.size(), " textures resident");
        for (const Widget* w : order) {
            LOG_WARNING("  ", (w->name.empty() ? "(unnamed)" : w->name),
                        " kind=", static_cast<int>(w->kind),
                        " rect=(", w->left, ",", w->bottom, " ", w->rectW, "x", w->rectH, ")",
                        " alpha=", w->alpha,
                        // Whether its art has actually reached the GPU. A
                        // texture with a correct rect and nothing uploaded
                        // draws nothing at all, and looks identical in a list
                        // of what was "drawn" to one that worked.
                        (w->kind == WidgetKind::Texture && !w->solidColor
                             ? (resident(w->texturePath) == kMissing ? " NOTRESIDENT" : "")
                             : ""),
                        // The vertex colour multiplies the image, so a zero
                        // alpha here draws nothing while the widget's own alpha
                        // still reads one. And the UVs, because a collapsed
                        // pair samples a single pixel.
                        (w->kind == WidgetKind::Texture ? " rgba=" : ""),
                        (w->kind == WidgetKind::Texture ? w->color[0] : 0.0f), ",",
                        (w->kind == WidgetKind::Texture ? w->color[1] : 0.0f), ",",
                        (w->kind == WidgetKind::Texture ? w->color[2] : 0.0f), ",",
                        (w->kind == WidgetKind::Texture ? w->color[3] : 0.0f),
                        (w->kind == WidgetKind::Texture ? " uv=" : ""),
                        (w->kind == WidgetKind::Texture ? w->texCoord[0] : 0.0f), ",",
                        (w->kind == WidgetKind::Texture ? w->texCoord[1] : 0.0f), ",",
                        (w->kind == WidgetKind::Texture ? w->texCoord[2] : 0.0f), ",",
                        (w->kind == WidgetKind::Texture ? w->texCoord[3] : 0.0f),
                        (w->texturePath.empty() ? "" : " tex="), w->texturePath,
                        (w->text.empty() ? "" : " text='"), w->text,
                        (w->text.empty() ? "" : "'"));
        }
        if (dumpWidgets >= 2) {
            LOG_WARNING("WidgetDump: every named widget, drawn or not");
            for (size_t id = 1; id < tree.size(); ++id) {
                const Widget* w = tree.get(static_cast<uint32_t>(id));
                if (!w || w->name.empty()) continue;
                LOG_WARNING("  ", w->name, " kind=", static_cast<int>(w->kind),
                            " rect=(", w->left, ",", w->bottom, " ",
                            w->rectW, "x", w->rectH, ")",
                            " anchors=", w->anchors.size(),
                            " shown=", w->shown ? 1 : 0,
                            " visible=", w->visible ? 1 : 0);
            }
        }
    }

    // Behind ImGui's own windows, so the existing interface stays on top while
    // the two coexist, but still over the 3D scene.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    // Level 5 draws ImGui's own font atlas through this same call, at a fixed
    // place. It is the one texture ImGui is certain to have uploaded, so it
    // separates two things that look identical from outside: whether AddImage
    // works on this draw list at all, and whether the descriptor sets this
    // renderer uploads are good. If the glyph sheet appears, the call is fine
    // and the textures are not.
    if (dumpWidgets >= 5) {
        dl->AddImage(ImGui::GetIO().Fonts->TexRef, ImVec2(40.0f, 40.0f),
                     ImVec2(440.0f, 440.0f));
    }

    for (const Widget* w : order) {
        // WoW measures from the bottom-left and upward; the screen measures from
        // the top-left and downward. Flip here, at the one place it matters, so
        // every anchor rule upstream reads the way Blizzard documents it.
        const float x0 = w->left * s;
        const float y0 = screenH - (w->bottom + w->rectH) * s;
        const float x1 = (w->left + w->rectW) * s;
        const float y1 = screenH - w->bottom * s;

        if (w->kind == WidgetKind::Frame) {
            if (w->hasBackdrop) drawBackdrop(dl, *w, x0, y0, x1, y1);
            if (w->isStatusBar) drawStatusBar(dl, *w, x0, y0, x1, y1);
            if (w->isSlider) drawSlider(dl, *w, x0, y0, x1, y1);
            if (w->isCooldown) drawCooldown(dl, *w, x0, y0, x1, y1);
            if (w->isEditBox) {
                // Its own text, drawn where a label would be, with a caret
                // while it has focus so it is clear which box is listening.
                ImFont* font = interfaceFace(w->fontFace);
                if (!font) font = interfaceFace("frizqt__");
                if (!font) font = ImGui::GetFont();
                const float size = ((w->fontHeight > 0.0f) ? w->fontHeight
                                                           : ImGui::GetFontSize()) * s;
                const uint32_t col = packColor(w->color, w->alpha);
                const float ty = y0 + ((y1 - y0) - size) * 0.5f;
                if (!w->editText.empty()) {
                    dl->AddText(font, size, ImVec2(x0 + 4.0f, ty), col,
                                w->editText.c_str());
                }
                if (w->editFocused) {
                    const std::string upTo = w->editText.substr(
                        0, std::min(w->cursorPos, w->editText.size()));
                    const float caret = font
                        ? font->CalcTextSizeA(size, FLT_MAX, 0.0f, upTo.c_str()).x
                        : 0.0f;
                    const float cx = x0 + 4.0f + caret;
                    dl->AddLine(ImVec2(cx, ty), ImVec2(cx, ty + size), col);
                }
            }
            continue;
        }

        // Level 3 outlines every widget where it believes it is. If the
        // outlines appear and the art does not, the images are the problem; if
        // neither appears, the whole layer is being covered or discarded. That
        // is two possibilities told apart by looking, rather than inferred from
        // a screenshot.
        if (dumpWidgets >= 3) {
            dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 0, 255, 200));
        }
        // Level 4 paints every widget solid instead of drawing its art. An
        // outline can be missed against a busy scene and dark art can be
        // mistaken for nothing at all; a solid block either covers the bottom
        // of the screen or it does not, and that answers whether these pixels
        // are reached without anyone having to squint at a screenshot.
        if (dumpWidgets >= 4) {
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                              IM_COL32(255, 0, 255, 255));
            continue;
        }

        if (w->kind == WidgetKind::Texture) {
            // A colour set with SetTexture(r,g,b) fills; a texture with no
            // file at all draws nothing. Treating the second as the first
            // painted every undecided region in the default white, which for a
            // full-width backdrop is a white slab across the screen.
            if (w->solidColor) {
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                                  packColor(w->color, w->alpha));
                continue;
            }
            if (w->texturePath.empty()) continue;
            // Only what is already resident. Anything still queued draws on a
            // later frame rather than forcing an upload here.
            auto it = textures_.find(w->texturePath);
            if (it == textures_.end() || it->second == kMissing) continue;
            VkDescriptorSet tex = it->second;
            // SetTexCoord is left/right/top/bottom in WoW's own order, and its
            // vertical sense already matches the image, so it passes through.
            dl->AddImage(reinterpret_cast<ImTextureID>(tex),
                         ImVec2(x0, y0), ImVec2(x1, y1),
                         ImVec2(w->texCoord[0], w->texCoord[2]),
                         ImVec2(w->texCoord[1], w->texCoord[3]),
                         packColor(w->color, w->alpha));
        } else if (w->kind == WidgetKind::FontString) {
            // Font objects carry a height, and honouring it is most of what
            // makes a label look right — a heading and a footnote are the same
            // words at different sizes. The atlas holds one face, so this scales
            // it rather than swapping fonts; loading FRIZQT__ properly needs an
            // atlas rebuild, which cannot happen while a frame is being built.
            ImFont* font = interfaceFace(w->fontFace);
            // The interface's own default, not the client's: ImGui draws with
            // whatever was added first, and that is deliberately the built-in
            // face so this client's panels are left alone.
            if (!font) font = interfaceFace("frizqt__");
            if (!font) font = ImGui::GetFont();
            const float base = ImGui::GetFontSize();
            const float size = ((w->fontHeight > 0.0f) ? w->fontHeight : base) * s;
            (void)base;
            const ImVec2 extent =
                font ? font->CalcTextSizeA(size, FLT_MAX, 0.0f, w->text.c_str())
                     : ImGui::CalcTextSize(w->text.c_str());
            float tx = x0;
            if (w->justifyH == "CENTER")     tx = x0 + (w->rectW - extent.x) * 0.5f;
            else if (w->justifyH == "RIGHT") tx = x1 - extent.x;
            const float ty = y0 + (w->rectH - extent.y) * 0.5f;
            // An outline is drawn as the same glyphs in black around the text.
            // ImGui has no outlined draw, and offsetting a few copies is what
            // the effect amounts to at these sizes — it is what keeps a
            // nameplate legible against whatever is behind it.
            if (!w->fontOutline.empty()) {
                const float d = (w->fontOutline == "THICK") ? 2.0f : 1.0f;
                const uint32_t shadow = IM_COL32(0, 0, 0,
                    static_cast<int>(std::clamp(w->alpha, 0.0f, 1.0f) * 255.0f));
                const ImVec2 around[8] = {
                    {-d, 0}, {d, 0}, {0, -d}, {0, d},
                    {-d, -d}, {d, -d}, {-d, d}, {d, d},
                };
                for (const ImVec2& o : around) {
                    dl->AddText(font, size, ImVec2(tx + o.x, ty + o.y), shadow,
                                w->text.c_str());
                }
            }
            dl->AddText(font, size, ImVec2(tx, ty),
                        packColor(w->color, w->alpha), w->text.c_str());
        }
    }
}

} // namespace ui
} // namespace wowee
