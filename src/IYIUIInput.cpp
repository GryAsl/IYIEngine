// Moved verbatim out of IYIUI.cpp; input, hit testing and focus navigation.

#include "IYI/IYIUI/IYIUI.h"
#include "IYI/Application.h"
#include "IYI/Utf8Text.h"
#include "IYI/FrameInput.h"
#include "IYI/Log.h"
#include <algorithm>
#include <cmath>

namespace IYI::UI {

static bool InsideClipAncestors(UIElement* elem, Vector2 pt)
{
    UIElement* cur = elem->Parent;
    while (cur) {
        if (cur->ClipChildren) {
            Vector2 localPt = cur->ResolvedTransform.ApplyInverse(pt);
            if (!CheckCollisionPointRec(localPt, cur->ResolvedRect)) {
                return false;
            }
        }
        cur = cur->Parent;
    }
    return true;
}

// The SPEC-IYIUI §11 walk, without side effects: the element that gets the pointer at a logical
// point (nullptr = none) and the innermost scroll view under it. HitTestTree and PointerOverUI share
// it, so "the UI has the pointer" can never disagree with what the UI actually reacts to.
UIElement* UICanvas::PickElement(Vector2 logicalMousePos, UIElement** outHoveredScroll) const
{
    UIElement* hitElement = nullptr;
    UIElement* hoveredScroll = nullptr;

    auto walkHit = [&](UIElement* elem, auto&& self) -> bool {
        if (!elem || !elem->Visible || !elem->Enabled || elem->ResolvedOpacity <= 0.01f) {
            return false;
        }

        if (elem->Type == UIElementType::ScrollView) {
            UIScrollView* sv = static_cast<UIScrollView*>(elem);
            Rectangle thumb = sv->GetThumbRect();
            if (thumb.width > 0.0f && thumb.height > 0.0f) {
                Vector2 localPt = elem->ResolvedTransform.ApplyInverse(logicalMousePos);
                Rectangle thumbHit = { thumb.x - 4.0f, thumb.y, thumb.width + 6.0f, thumb.height };
                if (CheckCollisionPointRec(localPt, thumbHit) && InsideClipAncestors(elem, logicalMousePos)) {
                    hoveredScroll = elem;
                    hitElement = elem;
                    return true;
                }
            }
        }

        for (int i = static_cast<int>(elem->Children.size()) - 1; i >= 0; --i) {
            if (self(elem->Children[i].get(), self)) {
                if (elem->Type == UIElementType::ScrollView && !hoveredScroll) {
                    Vector2 localPt = elem->ResolvedTransform.ApplyInverse(logicalMousePos);
                    if (CheckCollisionPointRec(localPt, elem->ResolvedRect) && InsideClipAncestors(elem, logicalMousePos)) {
                        hoveredScroll = elem;
                    }
                }
                return true;
            }
        }

        Vector2 localPt = elem->ResolvedTransform.ApplyInverse(logicalMousePos);
        const Rectangle hitRect = elem->Type == UIElementType::Button ? static_cast<UIButton*>(elem)->HitRect() : elem->ResolvedRect;
        if (CheckCollisionPointRec(localPt, hitRect)) {
            if (InsideClipAncestors(elem, logicalMousePos)) {
                if (elem->Type == UIElementType::ScrollView && !hoveredScroll) {
                    hoveredScroll = elem;
                }
                if (elem->Type == UIElementType::Button || elem->Type == UIElementType::Slider || elem->Type == UIElementType::TextInput) {
                    hitElement = elem;
                    return true;
                }
                if (elem->Type == UIElementType::Panel) {
                    UIPanel* p = static_cast<UIPanel*>(elem);
                    if (p->BlocksInputBehind) {
                        hitElement = elem;
                        return true;
                    }
                }
            }
        }
        return false;
    };

    walkHit(m_root.get(), walkHit);
    if (outHoveredScroll) *outHoveredScroll = hoveredScroll;
    return hitElement;
}

void UICanvas::HitTestTree(Vector2 logicalMousePos, float dt)
{
    UIElement* hoveredScroll = nullptr;
    UIElement* hitElement = PickElement(logicalMousePos, &hoveredScroll);

    bool isPressed = GameInput().MousePressed[MOUSE_BUTTON_LEFT];
    bool isReleased = GameInput().MouseReleased[MOUSE_BUTTON_LEFT];

#ifdef IYI_MOBILE
    if (isPressed) {
        m_touchScroll = static_cast<UIScrollView*>(hoveredScroll);
        m_touchStart = m_touchLast = logicalMousePos;
        m_scrollVelocity = 0.0f;
        m_touchDragging = false;
    }
    if (m_touchScroll && (GameInput().MouseDown[MOUSE_BUTTON_LEFT] || isReleased)) {
        const float dx = logicalMousePos.x - m_touchStart.x;
        const float dy = logicalMousePos.y - m_touchStart.y;
        if (!m_touchDragging && dx*dx + dy*dy > 144.0f) {
            m_touchDragging = true;
            m_touchLast = m_touchStart;
            if (m_capturedElement) {
                if (m_capturedElement->Type == UIElementType::Button) static_cast<UIButton*>(m_capturedElement)->Pressed = false;
                if (m_capturedElement->Type == UIElementType::Slider) static_cast<UISlider*>(m_capturedElement)->Pressed = false;
                if (m_capturedElement->Type == UIElementType::TextInput) static_cast<UITextInput*>(m_capturedElement)->Pressed = false;
                m_capturedElement = nullptr;
            }
            if (m_hoveredElement) {
                if (m_hoveredElement->Type == UIElementType::Button) static_cast<UIButton*>(m_hoveredElement)->Hovered = false;
                if (m_hoveredElement->Type == UIElementType::Slider) static_cast<UISlider*>(m_hoveredElement)->Hovered = false;
                if (m_hoveredElement->Type == UIElementType::TextInput) static_cast<UITextInput*>(m_hoveredElement)->Hovered = false;
                m_hoveredElement = nullptr;
            }
        }
        if (m_touchDragging) {
            const float delta = logicalMousePos.y - m_touchLast.y;
            const float maxScroll = std::max(0.0f, m_touchScroll->ContentHeight - m_touchScroll->ResolvedRect.height);
            m_touchScroll->ScrollOffset = std::clamp(m_touchScroll->ScrollOffset - delta, 0.0f, maxScroll);
            if (dt > 0.0f && (!isReleased || delta != 0.0f)) m_scrollVelocity = -delta / dt;
            m_touchLast = logicalMousePos;
            if (isReleased) m_touchDragging = false;
            return; // A drag owns the release; no widget may receive a click or hover.
        }
        m_touchLast = logicalMousePos;
        if (isReleased) m_touchScroll = nullptr;
    } else if (m_touchScroll && m_scrollVelocity != 0.0f) {
        const float maxScroll = std::max(0.0f, m_touchScroll->ContentHeight - m_touchScroll->ResolvedRect.height);
        const float decay = std::exp(-6.0f * std::max(0.0f, dt));
        const float offset = m_touchScroll->ScrollOffset + m_scrollVelocity * (1.0f - decay) / 6.0f;
        m_touchScroll->ScrollOffset = std::clamp(offset, 0.0f, maxScroll);
        m_scrollVelocity *= decay;
        if (offset != m_touchScroll->ScrollOffset || std::fabs(m_scrollVelocity) < 5.0f) {
            m_scrollVelocity = 0.0f;
            m_touchScroll = nullptr;
        }
    }
#else
    (void)dt;
#endif

    if (m_capturedElement) {
        if (m_capturedElement->Type == UIElementType::Button) {
            UIButton* btn = static_cast<UIButton*>(m_capturedElement);
            btn->Pressed = true;
            if (isReleased) {
                btn->Pressed = false;
                Vector2 localPt = btn->ResolvedTransform.ApplyInverse(logicalMousePos);
                if (CheckCollisionPointRec(localPt, btn->HitRect()) && InsideClipAncestors(btn, logicalMousePos)
#ifdef IYI_MOBILE
                    && !GameInput().LongTouchRelease
#endif
                ) {
                    btn->Clicked = true;
                    LogEvent("ui_click", { { "element", btn->Name } });
                }
                m_capturedElement = nullptr;
            }
        } else if (m_capturedElement->Type == UIElementType::Slider) {
            UISlider* s = static_cast<UISlider*>(m_capturedElement);
            s->Pressed = true;
            Vector2 localPt = s->ResolvedTransform.ApplyInverse(logicalMousePos);
            float oldVal = s->Value;
            float t = s->Vertical
                ? (1.0f - (localPt.y - s->ResolvedRect.y) / (s->ResolvedRect.height > 0.0f ? s->ResolvedRect.height : 1.0f))
                : ((localPt.x - s->ResolvedRect.x) / (s->ResolvedRect.width > 0.0f ? s->ResolvedRect.width : 1.0f));
            t = std::clamp(t, 0.0f, 1.0f);
            float range = s->MaxValue - s->MinValue;
            float newVal = s->MinValue + t * range;
            if (s->Step > 0.0f) {
                newVal = s->MinValue + std::round((newVal - s->MinValue) / s->Step) * s->Step;
                newVal = std::clamp(newVal, s->MinValue, s->MaxValue);
            }
            s->Value = newVal;
            if (s->Value != oldVal) {
                s->Changed = true;
            }
            if (isReleased) {
                s->Pressed = false;
                m_capturedElement = nullptr;
            }
        } else if (m_capturedElement->Type == UIElementType::TextInput) {
            UITextInput* ti = static_cast<UITextInput*>(m_capturedElement);
            ti->Pressed = true;
            if (isReleased) {
                ti->Pressed = false;
                m_capturedElement = nullptr;
            }
        } else if (m_capturedElement->Type == UIElementType::ScrollView) {
            UIScrollView* sv = static_cast<UIScrollView*>(m_capturedElement);
            Vector2 localPt = sv->ResolvedTransform.ApplyInverse(logicalMousePos);
            float visibleRatio = sv->ContentHeight > 0.0f ? (sv->ResolvedRect.height / sv->ContentHeight) : 1.0f;
            float thumbH = std::max(12.0f, sv->ResolvedRect.height * visibleRatio);
            float trackH = sv->ResolvedRect.height - thumbH;
            float newThumbY = localPt.y - sv->DragThumbGrabOffset;
            float t = trackH > 0.0f ? ((newThumbY - sv->ResolvedRect.y) / trackH) : 0.0f;
            float maxScroll = std::max(0.0f, sv->ContentHeight - sv->ResolvedRect.height);
            sv->ScrollOffset = std::clamp(t * maxScroll, 0.0f, maxScroll);
            if (isReleased) {
                m_capturedElement = nullptr;
            }
        }
    } else {
        if (m_hoveredElement && m_hoveredElement != hitElement) {
            if (m_hoveredElement->Type == UIElementType::Button) {
                static_cast<UIButton*>(m_hoveredElement)->Hovered = false;
            } else if (m_hoveredElement->Type == UIElementType::Slider) {
                static_cast<UISlider*>(m_hoveredElement)->Hovered = false;
            } else if (m_hoveredElement->Type == UIElementType::TextInput) {
                static_cast<UITextInput*>(m_hoveredElement)->Hovered = false;
            }
        }

        m_hoveredElement = hitElement;

        if (m_hoveredElement) {
            if (m_hoveredElement->Type == UIElementType::Button) {
                static_cast<UIButton*>(m_hoveredElement)->Hovered = true;
            } else if (m_hoveredElement->Type == UIElementType::Slider) {
                static_cast<UISlider*>(m_hoveredElement)->Hovered = true;
            } else if (m_hoveredElement->Type == UIElementType::TextInput) {
                static_cast<UITextInput*>(m_hoveredElement)->Hovered = true;
            }

            if (isPressed) {
                if (m_hoveredElement->Type == UIElementType::Button) {
                    UIButton* btn = static_cast<UIButton*>(m_hoveredElement);
                    btn->Pressed = true;
                    m_capturedElement = btn;
                    SetFocused(btn);
                } else if (m_hoveredElement->Type == UIElementType::Slider) {
                    UISlider* s = static_cast<UISlider*>(m_hoveredElement);
                    s->Pressed = true;
                    m_capturedElement = s;
                    SetFocused(s);
                    Vector2 localPt = s->ResolvedTransform.ApplyInverse(logicalMousePos);
                    float oldVal = s->Value;
                    float t = s->Vertical
                        ? (1.0f - (localPt.y - s->ResolvedRect.y) / (s->ResolvedRect.height > 0.0f ? s->ResolvedRect.height : 1.0f))
                        : ((localPt.x - s->ResolvedRect.x) / (s->ResolvedRect.width > 0.0f ? s->ResolvedRect.width : 1.0f));
                    t = std::clamp(t, 0.0f, 1.0f);
                    float range = s->MaxValue - s->MinValue;
                    float newVal = s->MinValue + t * range;
                    if (s->Step > 0.0f) {
                        newVal = s->MinValue + std::round((newVal - s->MinValue) / s->Step) * s->Step;
                        newVal = std::clamp(newVal, s->MinValue, s->MaxValue);
                    }
                    s->Value = newVal;
                    if (s->Value != oldVal) {
                        s->Changed = true;
                    }
                } else if (m_hoveredElement->Type == UIElementType::TextInput) {
                    UITextInput* ti = static_cast<UITextInput*>(m_hoveredElement);
                    ti->Pressed = true;
                    m_capturedElement = ti;
                    SetFocused(ti);
                    ti->CursorVisible = true;
                    ti->CursorBlinkTimer = 0.0f;
                } else if (m_hoveredElement->Type == UIElementType::ScrollView) {
                    UIScrollView* sv = static_cast<UIScrollView*>(m_hoveredElement);
                    m_capturedElement = sv;
                    Vector2 localPt = sv->ResolvedTransform.ApplyInverse(logicalMousePos);
                    Rectangle thumb = sv->GetThumbRect();
                    sv->DragThumbGrabOffset = localPt.y - thumb.y;
                }
            }
        }
    }

    const Vector2 w = GameInput().Wheel;
    const float wheel = std::fabs(w.x) > std::fabs(w.y) ? w.x : w.y;
    if (wheel != 0.0f && hoveredScroll) {
        UIScrollView* sv = static_cast<UIScrollView*>(hoveredScroll);
        sv->ScrollOffset -= wheel * 30.0f;
        float maxScroll = std::max(0.0f, sv->ContentHeight - sv->ResolvedRect.height);
        sv->ScrollOffset = std::clamp(sv->ScrollOffset, 0.0f, maxScroll);
    }
}

void UICanvas::UpdateFocusNavigation(float dt)
{
    std::vector<UIElement*> focusables;
    auto collectFocusables = [&](UIElement* elem, auto&& self) -> void {
        if (!elem || !elem->Visible || !elem->Enabled) return;
        if (elem->Focusable) {
            focusables.push_back(elem);
        }
        for (auto& c : elem->Children) {
            self(c.get(), self);
        }
    };
    collectFocusables(m_root.get(), collectFocusables);

    if (focusables.empty()) {
        SetFocused(nullptr);
        return;
    }

    if (m_focusedElement) {
        bool found = false;
        for (auto* f : focusables) {
            if (f == m_focusedElement) {
                found = true;
                break;
            }
        }
        if (!found) SetFocused(nullptr);
    }

    if (!GameInput().Ime.KeyboardCaptured && GameInput().KeyPressed[KEY_TAB]) {
        if (GameInput().KeyDown[KEY_LEFT_SHIFT] || GameInput().KeyDown[KEY_RIGHT_SHIFT]) {
            if (!m_focusedElement) {
                SetFocused(focusables.back());
            } else {
                auto it = std::find(focusables.begin(), focusables.end(), m_focusedElement);
                if (it == focusables.begin() || it == focusables.end()) {
                    SetFocused(focusables.back());
                } else {
                    SetFocused(*(it - 1));
                }
            }
        } else {
            if (!m_focusedElement) {
                SetFocused(focusables.front());
            } else {
                auto it = std::find(focusables.begin(), focusables.end(), m_focusedElement);
                if (it == focusables.end() || it + 1 == focusables.end()) {
                    SetFocused(focusables.front());
                } else {
                    SetFocused(*(it + 1));
                }
            }
        }
    }

    int dirX = 0, dirY = 0;
    if (!GameInput().Ime.KeyboardCaptured) {
        if (GameInput().KeyPressed[KEY_LEFT]) dirX -= 1;
        if (GameInput().KeyPressed[KEY_RIGHT]) dirX += 1;
        if (GameInput().KeyPressed[KEY_UP]) dirY -= 1;
        if (GameInput().KeyPressed[KEY_DOWN]) dirY += 1;
    }

    if (GameInput().Gamepads[0].Available) {
        if (GameInput().Gamepads[0].Pressed[GAMEPAD_BUTTON_LEFT_FACE_LEFT]) dirX -= 1;
        if (GameInput().Gamepads[0].Pressed[GAMEPAD_BUTTON_LEFT_FACE_RIGHT]) dirX += 1;
        if (GameInput().Gamepads[0].Pressed[GAMEPAD_BUTTON_LEFT_FACE_UP]) dirY -= 1;
        if (GameInput().Gamepads[0].Pressed[GAMEPAD_BUTTON_LEFT_FACE_DOWN]) dirY += 1;

        float stickX = GameInput().Gamepads[0].Axes[GAMEPAD_AXIS_LEFT_X];
        float stickY = GameInput().Gamepads[0].Axes[GAMEPAD_AXIS_LEFT_Y];
        m_focusNavRepeatTimer -= dt;
        if (m_focusNavRepeatTimer <= 0.0f) {
            if (stickX < -0.5f) { dirX -= 1; m_focusNavRepeatTimer = 0.25f; }
            else if (stickX > 0.5f) { dirX += 1; m_focusNavRepeatTimer = 0.25f; }
            if (stickY < -0.5f) { dirY -= 1; m_focusNavRepeatTimer = 0.25f; }
            else if (stickY > 0.5f) { dirY += 1; m_focusNavRepeatTimer = 0.25f; }
        }
    }

    if ((dirX != 0 || dirY != 0)) {
        if (!m_focusedElement) {
            SetFocused(focusables.front());
        } else {
            Vector2 curCenter = {
                m_focusedElement->ResolvedRect.x + m_focusedElement->ResolvedRect.width * 0.5f,
                m_focusedElement->ResolvedRect.y + m_focusedElement->ResolvedRect.height * 0.5f
            };

            UIElement* bestCandidate = nullptr;
            float bestDistSq = 1e18f;

            for (auto* candidate : focusables) {
                if (candidate == m_focusedElement) continue;

                Vector2 candCenter = {
                    candidate->ResolvedRect.x + candidate->ResolvedRect.width * 0.5f,
                    candidate->ResolvedRect.y + candidate->ResolvedRect.height * 0.5f
                };

                float dx = candCenter.x - curCenter.x;
                float dy = candCenter.y - curCenter.y;

                float dot = dx * static_cast<float>(dirX) + dy * static_cast<float>(dirY);
                if (dot <= 0.0f) continue;

                float distSq = dx * dx + dy * dy;
                if (distSq < bestDistSq) {
                    bestDistSq = distSq;
                    bestCandidate = candidate;
                }
            }

            if (bestCandidate) {
                SetFocused(bestCandidate);
            }
        }
    }

    bool activate = !GameInput().Ime.KeyboardCaptured && (GameInput().KeyPressed[KEY_ENTER] || GameInput().KeyPressed[KEY_SPACE]);
    if (GameInput().Gamepads[0].Available && GameInput().Gamepads[0].Pressed[GAMEPAD_BUTTON_RIGHT_FACE_DOWN]) {
        activate = true;
    }

    if (m_focusedElement) {
        if (m_focusedElement->Type == UIElementType::Button) {
            if (activate) {
                static_cast<UIButton*>(m_focusedElement)->Clicked = true;
                LogEvent("ui_click", { { "element", m_focusedElement->Name }, { "by", "keyboard" } });
            }
        } else if (m_focusedElement->Type == UIElementType::Slider) {
            UISlider* s = static_cast<UISlider*>(m_focusedElement);
            float delta = 0.0f;
            float range = s->MaxValue - s->MinValue;
            float step = s->Step > 0.0f ? s->Step : (range * 0.05f);

            if (s->Vertical) {
                if ((!GameInput().Ime.KeyboardCaptured && GameInput().KeyPressed[KEY_UP]) || (GameInput().Gamepads[0].Available && GameInput().Gamepads[0].Pressed[GAMEPAD_BUTTON_LEFT_FACE_UP])) delta += step;
                if ((!GameInput().Ime.KeyboardCaptured && GameInput().KeyPressed[KEY_DOWN]) || (GameInput().Gamepads[0].Available && GameInput().Gamepads[0].Pressed[GAMEPAD_BUTTON_LEFT_FACE_DOWN])) delta -= step;
            } else {
                if ((!GameInput().Ime.KeyboardCaptured && GameInput().KeyPressed[KEY_RIGHT]) || (GameInput().Gamepads[0].Available && GameInput().Gamepads[0].Pressed[GAMEPAD_BUTTON_LEFT_FACE_RIGHT])) delta += step;
                if ((!GameInput().Ime.KeyboardCaptured && GameInput().KeyPressed[KEY_LEFT]) || (GameInput().Gamepads[0].Available && GameInput().Gamepads[0].Pressed[GAMEPAD_BUTTON_LEFT_FACE_LEFT])) delta -= step;
            }

            if (delta != 0.0f) {
                float oldVal = s->Value;
                s->Value = std::clamp(s->Value + delta, s->MinValue, s->MaxValue);
                if (s->Value != oldVal) {
                    s->Changed = true;
                }
            }
        } else if (m_focusedElement->Type == UIElementType::TextInput) {
            UITextInput* ti = static_cast<UITextInput*>(m_focusedElement);

            // Cursor blink timer
            ti->CursorBlinkTimer += dt;
            if (ti->CursorBlinkTimer >= 0.5f) {
                ti->CursorBlinkTimer -= 0.5f;
                ti->CursorVisible = !ti->CursorVisible;
            }

            // PopInput(GameInput().Chars) yields Unicode codepoints (0+ per frame); store them as UTF-8.
            int c;
            while (ImeAcceptText(ti) && (c = PopInput(GameInput().Chars)) != 0) {
                if (AcceptCodepoint(c, ti->AllowLetters, ti->AllowDigits, ti->AllowSymbols) && ImeAcceptText(ti)) {
                    AppendUtf8(ti->Text, c);
                    ti->CursorVisible = true;
                    ti->CursorBlinkTimer = 0.0f;
                }
                // Rejected characters are silently dropped
            }

            // Ctrl+V (Cmd+V on macOS): paste through the same filter as typed characters.
#ifdef __APPLE__
            const bool pasteMod = GameInput().KeyDown[KEY_LEFT_SUPER] || GameInput().KeyDown[KEY_RIGHT_SUPER];
#else
            const bool pasteMod = GameInput().KeyDown[KEY_LEFT_CONTROL] || GameInput().KeyDown[KEY_RIGHT_CONTROL];
#endif
            if (!GameInput().Ime.KeyboardCaptured && pasteMod && GameInput().KeyPressed[KEY_V]) {
                const std::string pasted = FilterPaste(GameInput().Clipboard, ti->AllowLetters, ti->AllowDigits, ti->AllowSymbols);
                if (!pasted.empty()) {
                    ti->Text += pasted;
                    ti->CursorVisible = true;
                    ti->CursorBlinkTimer = 0.0f;
                }
            }

            // Backspace removes one whole codepoint, never half of a multibyte sequence.
            // Held down it repeats at the OS keyboard repeat delay/rate, like any native field.
            if (!GameInput().Ime.KeyboardCaptured && (GameInput().KeyPressed[KEY_BACKSPACE] || GameInput().KeyRepeat[KEY_BACKSPACE])) {
                if (PopLastCodepoint(ti->Text)) {
                    ti->CursorVisible = true;
                    ti->CursorBlinkTimer = 0.0f;
                }
            }

            // Enter: one-frame submitted flag cleared in ClearTransientFlags
            if (!GameInput().Ime.KeyboardCaptured && (GameInput().KeyPressed[KEY_ENTER] || GameInput().KeyPressed[KEY_KP_ENTER])) {
                ti->Submitted = true;
            }
        }
    }
}

Vector2 UICanvas::LogicalMousePosition() const
{
    float sf = ScaleFactor();
    Vector2 mousePos = GameInput().Mouse;
    const float radialWarp = CurrentApp().GetPostprocessRadialWarp();
    const float width = static_cast<float>(GameInput().ScreenWidth);
    const float height = static_cast<float>(GameInput().ScreenHeight);
    if (radialWarp > 0.0f && width > 0.0f && height > 0.0f) {
        // Match the postprocess shader's output-to-source radial lookup before all
        // UI hit tests. Otherwise curved buttons draw away from their input rects.
        const float x = mousePos.x * 2.0f / width - 1.0f;
        const float y = mousePos.y * 2.0f / height - 1.0f;
        const float scale = 1.0f + radialWarp * (x * x + y * y);
        mousePos = { (1.0f + x * scale) * width * 0.5f,
                     (1.0f + y * scale) * height * 0.5f };
    }
    return { mousePos.x / sf, mousePos.y / sf };
}

bool UICanvas::PointerOverUI() const
{
    return PointerOverUIAt(LogicalMousePosition());
}

bool UICanvas::PointerOverUIAt(Vector2 logicalPos) const
{
    if (!Visible || PendingDestroy || !m_root) return false;
    if (m_capturedElement) return true;   // a held button or a slider drag keeps the pointer anywhere
    UIElement* hoveredScroll = nullptr;
    return PickElement(logicalPos, &hoveredScroll) != nullptr || hoveredScroll != nullptr;
}

} // namespace IYI::UI
