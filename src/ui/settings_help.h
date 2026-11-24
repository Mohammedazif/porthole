/* Help and shortcut copy from the Porthole pc-v6 desktop reference. */
typedef struct {
    ph_control_icon icon;
    const char *title, *summary, *steps[3], *note;
} help_guide;

static const help_guide help_guides[] = {
    {PH_ICON_WIFI, "Connect your iPhone",
     "Pair an iPhone, reconnect, or switch between saved devices.",
     {"Connect your iPhone and PC to the same Wi-Fi network.",
      "In the iPhone app, scan the QR code on this PC or enter its pairing details manually.",
      "Reconnect by selecting this PC under Saved PC in the iPhone app."},
#ifdef _WIN32
     "If Windows Firewall prompts you, allow Porthole on private networks. Use Settings → Devices to add or select an iPhone."},
#else
     "To pair another iPhone, open Settings → Devices → Add iPhone. To switch back, select a saved iPhone there."},
#endif
    {PH_ICON_PHONE, "Share your screen",
     "Start or stop an iPhone broadcast.",
     {"Connect your iPhone to this PC in Porthole.",
      "On your iPhone, select Share screen, then Start Broadcast.",
      "To stop, use the broadcast indicator on your iPhone. File transfers remain available while connected."},
     "Some protected content may not appear in the shared screen."},
    {PH_ICON_ARROWS, "Send and receive files",
     "Transfer files between your iPhone and PC.",
     {"To send to iPhone, drop files into Porthole or select Send files.",
      "To send to PC, select files in Photos or Files on your iPhone, tap Share, then choose Porthole.",
      "Received files appear in transfer history. Use Open Downloads to find files saved on this PC."},
     "You can transfer files without sharing your screen. Change the receive folder in Settings → Transfers."},
    {PH_ICON_MOUSE, "Set up mouse and keyboard",
     "Configure Bluetooth input and pointer behavior.",
     {"Open Mouse and keyboard in the Porthole iPhone app to allow PC controls.",
      "Follow the Bluetooth setup guide on your PC and configure AssistiveTouch on your iPhone.",
      "Keep Screen sharing only enabled to activate controls only while broadcasting."},
     "Relative pointer mode requires AssistiveTouch button assignments. Clicks and typing in Settings stay on this PC."},
    {PH_ICON_CLIPBOARD, "Use clipboard sync",
     "Manage text and image synchronization.",
     {"Connect your iPhone and PC in Porthole.",
      "Manage Clipboard sync in the iPhone app and allow the desired transfer directions.",
      "Keep Porthole active on your iPhone when sending or receiving clipboard content."},
     "File transfers and clipboard sync work independently of screen sharing."},
};

typedef struct {
    const char *action, *keys;
} help_shortcut;

static const help_shortcut application_shortcuts[] = {
    {"Open Help and shortcuts in Settings", "F1"},
    {"Open the Bluetooth setup guide", "F2"},
    {"Show connection statistics", "F3"},
    {"Switch connected device", "F6"},
};

static const help_shortcut shared_screen_shortcuts[] = {
    {"Toggle full screen", "Alt + Enter"},
    {"Toggle always on top", "Alt + T"},
    {"Toggle actual pixel size", "Alt + 1"},
    {"Save a screenshot on this PC", "F8"},
    {"Start or stop screen recording", "F9"},
};

static const help_shortcut iphone_navigation_shortcuts[] = {
    {"Home", "Alt + H"},
    {"Spotlight", "Alt + F"},
    {"App Switcher", "Alt + S"},
    {"Control Center", "Alt + C"},
    {"Notification Center", "Alt + N"},
    {"Take an iPhone screenshot", "Alt + P"},
    {"Back", "Alt + ←"},
    {"Increase or decrease iPhone volume", "Alt + ↑ / ↓"},
    {"Show or hide the iPhone keyboard", "Alt + K"},
};

static const help_shortcut text_input_shortcuts[] = {
    {"Copy from a focused iPhone text field", "Ctrl + C"},
    {"Paste into a focused iPhone text field", "Ctrl + V"},
    {"Select all in a focused iPhone text field", "Ctrl + A"},
};

static const help_shortcut pointer_and_gestures_shortcuts[] = {
    {"Tap", "Left click"},
    {"Secondary tap", "Right click"},
    {"Swipe", "Click and drag"},
    {"Double-tap at the pointer", "Alt + D"},
    {"Long press at the pointer", "Alt + L"},
    {"Swipe up or down", "Alt + Page Down / Up"},
    {"Swipe in a direction", "Alt + Shift + arrow"},
    {"Swipe up from the bottom edge", "Alt + U"},
    {"Toggle feed scrolling", "F4"},
    {"Scroll horizontally", "Shift + wheel"},
    {"Cancel a gesture or release held input", "Esc"},
};

static const help_shortcut pc_audio_shortcuts[] = {
    {"Mute or unmute PC audio", "Alt + M"},
    {"Decrease or increase PC audio volume", "Alt + − / ="},
    {"Adjust PC audio synchronization", "Alt + , / ."},
};

static const struct {
    const char *title;
    const help_shortcut *shortcuts;
    size_t count;
} help_shortcut_groups[] = {
    {"Application", application_shortcuts, SDL_arraysize(application_shortcuts)},
    {"Shared screen", shared_screen_shortcuts, SDL_arraysize(shared_screen_shortcuts)},
    {"iPhone navigation", iphone_navigation_shortcuts, SDL_arraysize(iphone_navigation_shortcuts)},
    {"Text input", text_input_shortcuts, SDL_arraysize(text_input_shortcuts)},
    {"Pointer and gestures", pointer_and_gestures_shortcuts, SDL_arraysize(pointer_and_gestures_shortcuts)},
    {"PC audio", pc_audio_shortcuts, SDL_arraysize(pc_audio_shortcuts)},
};
