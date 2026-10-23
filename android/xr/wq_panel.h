// WakerQuest's settings panel in the headset (wq_panel.cpp): drawn by the headset layer itself and
// worked with the laser pointer or the controller, as GalaxyQuest's VR settings panel. On the layer's
// thread, with its GL context current.
#pragma once

#include <openxr/openxr.h>

#include <string>

namespace wqp {

// What the panel asks of the layer (wq_xr.cpp): xr.ini's settings.
struct Host {
    std::string (*xrGet)(const char* key);  // the value in effect
    void (*xrSet)(const char* key, const char* value);
    void (*resetPosition)();                // the model back in front of the player
    void (*resetPanels)();                  // the HUD and the dialogues back in their places
};

void init(const Host& host, XrSession session, bool srgb);
bool isOpen();
// Opens at `pose` (its middle, facing the player), or closes; the settings changed are already saved.
void open(const XrPosef& pose);
void close();

// The pointer this frame (LOCAL space): the distance to where the ray meets the panel, 0 off it.
// `clickDown`: the active hand's trigger. *tick: something new under the pointer, or a press.
float pointer(XrVector3f origin, XrVector3f dir, bool clickDown, bool* tick, bool* hovering);
// The controller: the left stick moves between the rows and changes a value, the right stick changes
// the tab, A presses, B closes. *tick as above.
void nav(float stickX, float stickY, float tabX, bool a, bool b, bool* tick);

// The panel's layer (false while it is closed), redrawn when something on it changed.
bool layer(XrSpace space, XrCompositionLayerQuad* out);

// A change of one of the game's settings (pc_settings) for the game thread to apply, as the panel's own
// (wq_xr_panel_take_change); replaces one of the same key not yet applied. Any thread.
void gameChange(const char* key, const char* value);

// A small label floating at `pose` (the model's size while the two hands scale it); empty: none.
bool label(const char* text, const XrPosef& pose, XrSpace space, XrCompositionLayerQuad* out);

}  // namespace wqp
