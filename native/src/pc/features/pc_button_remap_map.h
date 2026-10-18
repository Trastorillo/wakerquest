#pragma once

// Button remap (issue #15): which physical input drives each GameCube pad action. Pure logic, so
// cos_pc_tests checks it on the host; the SDL side (reading inputs, config, menu) builds on it.
//
// Input::A..Y are the printed Switch letters (A = East, B = South, X = North, Y = West). The
// default map is the one the port always used: the GC A button on the south face button, and so on.
// A valid map gives every action its own input, so assigning an input another action holds swaps
// the two instead of leaving one action without a button.

#include <array>
#include <cstddef>
#include <cstdint>

namespace pc::remap {

enum class Action : uint8_t { A, B, X, Y, Z, L, R, Start, Up, Down, Left, Right, Count };

enum class Input : uint8_t {
    A, B, X, Y, L, R, ZL, ZR, Plus, Up, Down, Left, Right, LStick, RStick,
    Count,
    None = 0xFF,
};

using Map = std::array<Input, (size_t)Action::Count>;

inline Map defaultMap() {
    Map m{};
    m[(size_t)Action::A] = Input::B;
    m[(size_t)Action::B] = Input::A;
    m[(size_t)Action::X] = Input::Y;
    m[(size_t)Action::Y] = Input::X;
    m[(size_t)Action::Z] = Input::R;
    m[(size_t)Action::L] = Input::ZL;
    m[(size_t)Action::R] = Input::ZR;
    m[(size_t)Action::Start] = Input::Plus;
    m[(size_t)Action::Up] = Input::Up;
    m[(size_t)Action::Down] = Input::Down;
    m[(size_t)Action::Left] = Input::Left;
    m[(size_t)Action::Right] = Input::Right;
    return m;
}

// The config key of an action.
inline const char* actionKey(Action a) {
    static const char* const keys[(size_t)Action::Count] = {
        "COS_PAD_A",  "COS_PAD_B",    "COS_PAD_X",    "COS_PAD_Y",    "COS_PAD_Z",    "COS_PAD_L",
        "COS_PAD_R",  "COS_PAD_START", "COS_PAD_UP",   "COS_PAD_DOWN", "COS_PAD_LEFT", "COS_PAD_RIGHT",
    };
    return (size_t)a < (size_t)Action::Count ? keys[(size_t)a] : "";
}

// The config value of an input.
inline const char* inputValue(Input in) {
    static const char* const values[(size_t)Input::Count] = {
        "a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "up", "down", "left", "right", "lstick", "rstick",
    };
    return (size_t)in < (size_t)Input::Count ? values[(size_t)in] : "";
}

// Exact lowercase match of inputValue(); leaves *out alone and returns false for anything else.
inline bool parseInput(const char* s, Input* out) {
    if (s == nullptr) {
        return false;
    }
    for (size_t i = 0; i < (size_t)Input::Count; ++i) {
        const char* v = inputValue((Input)i);
        size_t k = 0;
        while (v[k] != '\0' && v[k] == s[k]) {
            ++k;
        }
        if (v[k] == '\0' && s[k] == '\0') {
            *out = (Input)i;
            return true;
        }
    }
    return false;
}

// Every action has a real input and no input is used twice.
inline bool isValid(const Map& m) {
    for (size_t i = 0; i < m.size(); ++i) {
        if ((size_t)m[i] >= (size_t)Input::Count) {
            return false;
        }
        for (size_t j = i + 1; j < m.size(); ++j) {
            if (m[i] == m[j]) {
                return false;
            }
        }
    }
    return true;
}

struct AssignResult {
    Map map;
    bool swapped;  // another action held `in` and took `a`'s old input
    Action other;  // that action; meaningful only when swapped
};

// Gives action `a` the input `in`. If another action holds `in`, it takes `a`'s old input.
inline AssignResult assign(const Map& m, Action a, Input in) {
    AssignResult r{m, false, Action::Count};
    const size_t ai = (size_t)a;
    if (ai >= (size_t)Action::Count || m[ai] == in) {
        return r;
    }
    for (size_t i = 0; i < m.size(); ++i) {
        if (i != ai && m[i] == in) {
            r.map[i] = m[ai];
            r.swapped = true;
            r.other = (Action)i;
            break;
        }
    }
    r.map[ai] = in;
    return r;
}

} // namespace pc::remap
