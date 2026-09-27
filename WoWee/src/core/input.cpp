#include "core/input.hpp"

namespace wowee {
namespace core {

Input& Input::getInstance() {
    static Input instance;
    return instance;
}

void Input::update() {
    // Copy current state to previous
    previousKeyState = currentKeyState;
    previousMouseState = currentMouseState;
    previousMousePosition = mousePosition;

    // Get current keyboard state
    const Uint8* keyState = SDL_GetKeyboardState(nullptr);
    for (int i = 0; i < NUM_KEYS; ++i) {
        currentKeyState[i] = keyState[i];
    }

    // Get current mouse state
    int mouseX, mouseY;
    Uint32 mouseState = SDL_GetMouseState(&mouseX, &mouseY);
    mousePosition = glm::vec2(static_cast<float>(mouseX), static_cast<float>(mouseY));

    // SDL_BUTTON(x) is defined as (1 << (x-1)), so button indices are 1-based.
    // SDL_BUTTON(0) is undefined behavior (negative shift). Start at 1.
    currentMouseState[0] = false;
    for (int i = 1; i < NUM_MOUSE_BUTTONS; ++i) {
        currentMouseState[i] = (mouseState & SDL_BUTTON(i)) != 0;
        edgePressed_[i] = pressedEvent_[i] || (currentMouseState[i] && !previousMouseState[i]);
        edgeReleased_[i] = releasedEvent_[i] || (!currentMouseState[i] && previousMouseState[i]);
        pressedEvent_[i] = false;
        releasedEvent_[i] = false;
    }

    // Calculate mouse delta
    mouseDelta = mousePosition - previousMousePosition;

    // Reset wheel delta (will be set by handleEvent)
    mouseWheelDelta = 0.0f;
}

void Input::handleEvent(const SDL_Event& event) {
    if (event.type == SDL_MOUSEWHEEL) {
        mouseWheelDelta = static_cast<float>(event.wheel.y);
    } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
        const int button = event.button.button;
        if (button > 0 && button < NUM_MOUSE_BUTTONS) {
            if (event.type == SDL_MOUSEBUTTONDOWN) pressedEvent_[button] = true;
            else releasedEvent_[button] = true;
        }
        mousePosition = glm::vec2(static_cast<float>(event.button.x),
                                   static_cast<float>(event.button.y));
    }
}

bool Input::isKeyPressed(SDL_Scancode key) const {
    if (key < 0 || key >= NUM_KEYS) return false;
    return currentKeyState[key];
}

bool Input::isKeyJustPressed(SDL_Scancode key) const {
    if (key < 0 || key >= NUM_KEYS) return false;
    return currentKeyState[key] && !previousKeyState[key];
}

bool Input::isKeyJustReleased(SDL_Scancode key) const {
    if (key < 0 || key >= NUM_KEYS) return false;
    return !currentKeyState[key] && previousKeyState[key];
}

bool Input::isMouseButtonPressed(int button) const {
    if (button < 0 || button >= NUM_MOUSE_BUTTONS) return false;
    return currentMouseState[button];
}

bool Input::isMouseButtonJustPressed(int button) const {
    if (button < 0 || button >= NUM_MOUSE_BUTTONS) return false;
    return edgePressed_[button];
}

bool Input::isMouseButtonJustReleased(int button) const {
    if (button < 0 || button >= NUM_MOUSE_BUTTONS) return false;
    return edgeReleased_[button];
}

void Input::setMouseLocked(bool locked) {
    mouseLocked = locked;
    SDL_SetRelativeMouseMode(locked ? SDL_TRUE : SDL_FALSE);
}

} // namespace core
} // namespace wowee
