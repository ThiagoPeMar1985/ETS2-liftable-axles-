#pragma once
#include <string>
#include <Windows.h>

namespace ts_extra_utilities::settings
{
    enum class Action
    {
        TOGGLE_UI,
        TOGGLE_CURSOR,
        COUNT,
    };

    constexpr int action_count = static_cast< int >( Action::COUNT );
    constexpr int trailer_slot_count = 20; // matches max_trailers in trailer_manipulation.cpp

    struct Settings
    {
        UINT keys[ action_count ] = {}; // virtual-key codes, 0 = unbound
        // Bitmask of trailer axles the lift key may raise, per position in the trailer
        // chain. -1 = not configured, use the trailer's own liftable axles.
        int32_t axle_masks[ trailer_slot_count ];
        // When reverse gear engages with all trailer axles down, only the first
        // liftable axle of each trailer is kept and raised automatically; leaving
        // reverse lowers it and restores the saved selection.
        bool reverse_lift = true;
    };

    extern Settings g_settings;

    // %APPDATA%\ts-liftable-axles\settings.ini, shared by ETS2 and ATS
    void load();
    void save();

    UINT key( Action action );
    const char* action_name( Action action );
    std::string key_name( UINT vk );

    // Rebinding: while capturing, the next key press is taken by capture_key() instead of the game.
    void begin_capture( Action action );
    bool is_capturing();
    bool is_capturing( Action action );
    // Returns true when the key was consumed. Escape cancels, anything else becomes the binding.
    bool capture_key( UINT vk );
}
