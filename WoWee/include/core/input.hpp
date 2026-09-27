#pragma once

#include <SDL2/SDL.h>
#include <array>
#include <glm/glm.hpp>

namespace wowee {
namespace core {

class Input {
public:
    static Input& getInstance();

    void update();
    void handleEvent(const SDL_Event& event);

    // Keyboard
    bool isKeyPressed(SDL_Scancode key) const;
    bool isKeyJustPressed(SDL_Scancode key) const;
    bool isKeyJustReleased(SDL_Scancode key) const;

    // Mouse
    bool isMouseButtonPressed(int button) const;
    bool isMouseButtonJustPressed(int button) const;
    bool isMouseButtonJustReleased(int button) const;

    glm::vec2 getMousePosition() const { return mousePosition; }
    glm::vec2 getMouseDelta() const { return mouseDelta; }
    float getMouseWheelDelta() const { return mouseWheelDelta; }

    bool isMouseLocked() const { return mouseLocked; }
    void setMouseLocked(bool locked);

private:
    Input() = default;
    ~Input() = default;
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    static constexpr int NUM_KEYS = SDL_NUM_SCANCODES;
    static constexpr int NUM_MOUSE_BUTTONS = 8;

    std::array<bool, NUM_KEYS> currentKeyState = {};
    std::array<bool, NUM_KEYS> previousKeyState = {};

    std::array<bool, NUM_MOUSE_BUTTONS> currentMouseState = {};
    std::array<bool, NUM_MOUSE_BUTTONS> previousMouseState = {};
    // A tap on the tablet often presses and releases inside one long frame.
    // Sampling the button only at frame start misses it, so the edges are
    // latched from the events themselves.
    std::array<bool, NUM_MOUSE_BUTTONS> pressedEvent_ = {};
    std::array<bool, NUM_MOUSE_BUTTONS> releasedEvent_ = {};
    std::array<bool, NUM_MOUSE_BUTTONS> edgePressed_ = {};
    std::array<bool, NUM_MOUSE_BUTTONS> edgeReleased_ = {};

    glm::vec2 mousePosition = glm::vec2(0.0f);
    glm::vec2 previousMousePosition = glm::vec2(0.0f);
    glm::vec2 mouseDelta = glm::vec2(0.0f);
    float mouseWheelDelta = 0.0f;
    bool mouseLocked = false;
};

} // namespace core
} // namespace wowee
