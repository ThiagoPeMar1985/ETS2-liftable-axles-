#include "trailer_manipulation.hpp"

#include <algorithm>
#include <atomic>

#include "imgui.h"

#include "common/scssdk_telemetry_truck_common_channels.h"

#include "core.hpp"
#include "hooks/function_hook.hpp"
#include "memory/memory_utils.hpp"
#include "prism/offsets.hpp"
#include "settings/settings.hpp"

namespace ts_extra_utilities
{
    constexpr uint32_t max_trailers = 20;

    // Shared between the render thread (UI) and the game thread (the hook). The hook only
    // compares the pointer, it never dereferences it, so a stale entry is harmless.
    struct LockedSteering
    {
        std::atomic< prism::vehicle_wheel_steering_data_t* > data{ nullptr };
        std::atomic< float > angle{ 0.f };
    };

    LockedSteering locked_steering[ max_trailers ];

    // UI-only state, render thread
    bool locked_ui[ max_trailers ] = {};
    float angle_ui[ max_trailers ] = {};
    bool centering_ui[ max_trailers ] = {}; // easing back to 0 at steering_speed

    constexpr uint32_t max_axles = 16; // sane upper bound for the flag count sanity check

    // Per trailer chain position: the lift controller and axle flag array seen last,
    // plus the values the game gave them. The controller is created at trailer setup
    // only when at least one axle is natively liftable; its mask at +0x38 decides
    // which axles the native trailer lift key raises, and the flag bytes decide which
    // axles the wheel-position pass treats as lifted. Rewriting both every frame
    // selects the liftable set per axle, including axles that are not liftable by
    // default.
    struct LiftAxles
    {
        void* controller = nullptr; // trailer+0x6A0, detects a trailer swap/rebuild on change
        void* axle_data = nullptr; // trailer+0x200
        uint8_t* flags = nullptr; // per-axle liftable flags inside axle_data
        uint8_t orig_flags[ max_axles ]{}; // the flags the game set, restored on unload
        uint32_t orig_mask = 0; // the mask the game built, restored on unload
        uint32_t applied_mask = 0; // last mask we wrote; a different value means the game re-initialized
        uint32_t native_axles = 0; // orig_flags folded to axle bits, the default selection
        uint32_t axle_count = 0;
    };

    LiftAxles lift_axles[ max_trailers ];

    // Reverse-gear lift assist ("van der leest" style): with all axles on the
    // ground, engaging reverse keeps only the first liftable axle selected and
    // raises it; leaving reverse lowers it and restores the saved selection.
    // Fed by telemetry channels, read on the render thread.
    std::atomic< bool > reverse_light{ false };
    std::atomic< int32_t > engine_gear{ 0 };

    // While true the effective selection is the first liftable axle only; the
    // saved axle_masks are never touched.
    bool reverse_override = false;
    // True when the plugin commanded the raise itself — only then is lowering on
    // reverse exit ours to undo.
    bool reverse_auto_lift = false;
    bool was_reversing = false;

    SCSAPI_VOID on_light_reverse( const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t )
    {
        reverse_light.store( value != nullptr && value->value_bool.value != 0, std::memory_order_relaxed );
    }

    SCSAPI_VOID on_engine_gear( const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t )
    {
        engine_gear.store( value != nullptr ? value->value_s32.value : 0, std::memory_order_relaxed );
    }

    std::shared_ptr< CFunctionHook > set_individual_steering_hook = nullptr;

    /**
     * \brief Hook for set_individual_steering, which the game calls every steering advance with the
     * angle it computed from the joint. For a trailer the user took control of, swap in their angle.
     *
     * Before 1.61 this feature skipped physics_trailer_u::steering_advance entirely, but that function
     * now also runs a per-wheel update after setting the angle, so only the angle is replaced here.
     */
    void hk_set_individual_steering( prism::vehicle_wheel_steering_data_t* data, float angle )
    {
        for ( auto& locked : locked_steering )
        {
            if ( data != nullptr && locked.data.load( std::memory_order_relaxed ) == data )
            {
                angle = locked.angle.load( std::memory_order_relaxed );
                break;
            }
        }
        set_individual_steering_hook->get_original< prism::set_individual_steering_fn >()( data, angle );
    }

    void release_all()
    {
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            locked_steering[ i ].data.store( nullptr );
            locked_ui[ i ] = false;

            // give the game its own mask and flag bytes back; the objects may already
            // be gone if the plugin is unloaded after trailers were, so guard the writes
            auto& lift = lift_axles[ i ];
            if ( lift.controller != nullptr || lift.flags != nullptr )
            {
                __try
                {
                    if ( lift.controller != nullptr )
                    {
                        prism::offsets::field< uint32_t >( lift.controller, prism::offsets::lift_controller_axle_mask ) = lift.orig_mask;
                    }
                    if ( lift.flags != nullptr )
                    {
                        for ( uint32_t a = 0; a < lift.axle_count && a < max_axles; ++a )
                        {
                            lift.flags[ a ] = lift.orig_flags[ a ];
                        }
                    }
                }
                __except ( EXCEPTION_EXECUTE_HANDLER )
                {
                }
                lift = LiftAxles{};
            }
        }
    }

    CTrailerManipulation::CTrailerManipulation() = default;

    CTrailerManipulation::~CTrailerManipulation()
    {
        release_all();
        set_individual_steering_hook.reset();
    }

    bool CTrailerManipulation::init()
    {
        const auto address = memory::get_address_for_pattern( "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec ? 8b 41 ? 48 8b d9 0f 29 74" );

        if ( address == 0 )
        {
            CCore::g_instance->error( "Could not find 'set_individual_steering' function" );
            return false;
        }
        CCore::g_instance->debug( "Found set_individual_steering function @ +{:x}", memory::as_offset( address ) );

        set_individual_steering_hook = CCore::g_instance->get_hooks_manager()->register_function_hook(
            "set_individual_steering",
            address,
            reinterpret_cast< uint64_t >( &hk_set_individual_steering ) );

        if ( set_individual_steering_hook->hook() != CHook::HOOKED )
        {
            CCore::g_instance->error( "Could not hook 'set_individual_steering'" );
            return false;
        }

        // reverse-gear detection for the lift assist; each channel failing just
        // narrows the fallback, neither is fatal
        const auto* init_params = CCore::g_instance->get_init_params();
        if ( init_params->register_for_channel( SCS_TELEMETRY_TRUCK_CHANNEL_light_reverse,
                SCS_U32_NIL, SCS_VALUE_TYPE_bool, SCS_TELEMETRY_CHANNEL_FLAG_none,
                &on_light_reverse, nullptr ) != SCS_RESULT_ok )
        {
            CCore::g_instance->warning( "Could not register 'truck.light.reverse'" );
        }
        if ( init_params->register_for_channel( SCS_TELEMETRY_TRUCK_CHANNEL_engine_gear,
                SCS_U32_NIL, SCS_VALUE_TYPE_s32, SCS_TELEMETRY_CHANNEL_FLAG_none,
                &on_engine_gear, nullptr ) != SCS_RESULT_ok )
        {
            CCore::g_instance->warning( "Could not register 'truck.engine.gear'" );
        }

        this->valid_ = true;
        return this->valid_;
    }


    // The current trailer chain, refreshed by update() every frame on the render thread.
    void* trailers[ max_trailers ] = {};
    uint32_t trailer_count = 0;

    bool held[ settings::action_count ] = {};

    prism::vehicle_wheel_steering_data_t* steering_data( void* trailer )
    {
        return prism::offsets::field< prism::vehicle_wheel_steering_data_t* >( trailer, prism::offsets::trailer_steering_data );
    }

    void set_locked( const uint32_t i, const bool lock )
    {
        if ( locked_ui[ i ] == lock ) return;
        locked_ui[ i ] = lock;
        CCore::g_instance->info( "{} steering for {}", lock ? "Locking" : "Unlocking", i );
        if ( lock )
        {
            // start from where the game had the wheels so they do not jump
            angle_ui[ i ] = prism::offsets::field< float >( trailers[ i ], prism::offsets::trailer_steering );
            locked_steering[ i ].angle.store( angle_ui[ i ] );
        }
    }

    bool game_has_focus()
    {
        DWORD pid = 0;
        GetWindowThreadProcessId( GetForegroundWindow(), &pid );
        return pid == GetCurrentProcessId();
    }

    // true while the bound key is down; first_press only on the frame it went down
    bool key_down( const settings::Action action, bool* first_press = nullptr )
    {
        const auto vk = settings::key( action );
        const bool down = vk != 0 && ( GetAsyncKeyState( static_cast< int >( vk ) ) & 0x8000 ) != 0;
        bool& was = held[ static_cast< int >( action ) ];
        if ( first_press != nullptr ) *first_press = down && !was;
        was = down;
        return down;
    }

    void CTrailerManipulation::update( const float dt )
    {
        if ( !this->valid_ ) return;

        auto* game_actor = CCore::g_instance->get_game_actor();

        void* trailer = nullptr;
        if ( game_actor != nullptr )
        {
            trailer = prism::offsets::field< void* >( game_actor, prism::offsets::game_actor_trailer );
        }

        trailer_count = 0;
        for ( ; trailer != nullptr && trailer_count < max_trailers; ++trailer_count )
        {
            trailers[ trailer_count ] = trailer;
            trailer = prism::offsets::field< void* >( trailer, prism::offsets::trailer_slave );
        }

        // nothing to steer on a trailer without steerable wheels, or one that is gone
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            if ( i >= trailer_count || steering_data( trailers[ i ] ) == nullptr )
            {
                locked_ui[ i ] = false;
                centering_ui[ i ] = false;
            }
        }

        // hotkeys, ignored while the game is in the background or a key is being rebound
        if ( game_has_focus() && !settings::is_capturing() )
        {
            bool lock_pressed = false, center_pressed = false;
            key_down( settings::Action::TOGGLE_STEERING_LOCK, &lock_pressed );
            key_down( settings::Action::STEER_CENTER, &center_pressed );
            const bool left = key_down( settings::Action::STEER_LEFT );
            const bool right = key_down( settings::Action::STEER_RIGHT );

            bool any_locked = false;
            for ( uint32_t i = 0; i < trailer_count; ++i ) any_locked |= locked_ui[ i ];

            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                if ( steering_data( trailers[ i ] ) == nullptr ) continue;

                if ( lock_pressed )
                {
                    set_locked( i, !any_locked );
                }
                else if ( left || right || center_pressed )
                {
                    set_locked( i, true ); // steering a trailer means taking control of it
                }

                if ( !locked_ui[ i ] ) continue;

                const float step = settings::g_settings.steering_speed * dt;
                if ( center_pressed ) centering_ui[ i ] = true;
                if ( left || right ) centering_ui[ i ] = false;
                if ( centering_ui[ i ] )
                {
                    // ease back to straight instead of snapping
                    if ( angle_ui[ i ] > step ) angle_ui[ i ] -= step;
                    else if ( angle_ui[ i ] < -step ) angle_ui[ i ] += step;
                    else
                    {
                        angle_ui[ i ] = 0.f;
                        centering_ui[ i ] = false;
                    }
                }
                if ( left ) angle_ui[ i ] = ( std::max )( angle_ui[ i ] - step, -1.f );
                if ( right ) angle_ui[ i ] = ( std::min )( angle_ui[ i ] + step, 1.f );
                locked_steering[ i ].angle.store( angle_ui[ i ] );
            }
        }

        // Published every frame: the data pointer changes when trailers are swapped.
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            locked_steering[ i ].data.store( locked_ui[ i ] ? steering_data( trailers[ i ] ) : nullptr );
            if ( i < trailer_count && !locked_ui[ i ] )
            {
                angle_ui[ i ] = prism::offsets::field< float >( trailers[ i ], prism::offsets::trailer_steering );
            }
        }

        // Reverse-gear lift assist: on the engage edge, with every trailer axle on
        // the ground, only the first liftable axle of each trailer stays selected
        // and the controllers are commanded to raise — as if the lift key was
        // pressed. On the disengage edge whatever we raised is lowered again and
        // the saved selection resumes. Axles already up when reverse engages are
        // left untouched.
        const bool reversing = settings::g_settings.reverse_lift
            && ( reverse_light.load( std::memory_order_relaxed )
                || engine_gear.load( std::memory_order_relaxed ) < 0 );

        bool reverse_just_engaged = false;
        if ( reversing && !was_reversing )
        {
            // all trailers must be down, and at least one must have a liftable axle
            bool all_down = trailer_count > 0, any_liftable = false;
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                any_liftable |= lift_axles[ i ].native_axles != 0;
                auto* controller = lift_axles[ i ].controller;
                if ( controller != nullptr
                    && prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) != 0 )
                {
                    all_down = false;
                }
            }
            reverse_override = all_down && any_liftable;
            reverse_just_engaged = reverse_override;
        }
        else if ( !reversing && was_reversing )
        {
            reverse_override = false;
            if ( reverse_auto_lift )
            {
                reverse_auto_lift = false;
                for ( uint32_t i = 0; i < trailer_count; ++i )
                {
                    // fresh read — lift_axles[] may still point at a detached trailer
                    auto* controller = prism::offsets::field< void* >( trailers[ i ], prism::offsets::trailer_lift_controller );
                    if ( controller != nullptr
                        && prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) != 0 )
                    {
                        prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) = 1; // lowering
                    }
                }
                if ( game_actor != nullptr )
                {
                    prism::offsets::field< uint8_t >( game_actor, prism::offsets::game_actor_lift_state ) &= ~2u;
                }
            }
        }
        was_reversing = reversing;

        // Axle lift: rewrite each trailer's lift controller mask (+0x38) and the
        // per-axle liftable flags on the axle data so the game's lift key raises
        // exactly the natively liftable axles picked in the UI.
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
            auto& lift = lift_axles[ i ];

            void* axle_data = nullptr;
            void* controller = nullptr;
            if ( i < trailer_count )
            {
                axle_data = prism::offsets::field< void* >( trailers[ i ], prism::offsets::trailer_axle_data );
                controller = prism::offsets::field< void* >( trailers[ i ], prism::offsets::trailer_lift_controller );
            }

            const auto flag_count = axle_data != nullptr
                ? prism::offsets::field< uint64_t >( axle_data, prism::offsets::axle_data_flags_count )
                : 0;
            auto* flags = axle_data != nullptr
                ? prism::offsets::field< uint8_t* >( axle_data, prism::offsets::axle_data_flags )
                : nullptr;
            if ( flags == nullptr || flag_count == 0 || flag_count > max_axles )
            {
                lift = LiftAxles{}; // detached, or not set up yet
                continue;
            }

            if ( lift.axle_data != axle_data || lift.flags != flags
                || ( controller != nullptr && lift.controller != controller ) )
            {
                // first sight, or the game rebuilt the objects: snapshot its values
                lift.axle_data = axle_data;
                lift.flags = flags;
                lift.axle_count = static_cast< uint32_t >( flag_count );
                lift.controller = controller;
                lift.native_axles = 0;
                lift.orig_mask = controller != nullptr
                    ? prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_mask )
                    : 0;
                for ( uint32_t a = 0; a < lift.axle_count; ++a )
                {
                    lift.orig_flags[ a ] = flags[ a ];
                    if ( flags[ a ] != 0 ) lift.native_axles |= 1u << a;
                }
                lift.applied_mask = ~0u; // force a write this frame
            }
            else if ( controller != nullptr
                && prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_mask ) != lift.applied_mask )
            {
                // the game re-initialized the mask (trailer data reload): re-snapshot
                lift.orig_mask = prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_mask );
                lift.native_axles = 0;
                for ( uint32_t a = 0; a < lift.axle_count; ++a )
                {
                    lift.orig_flags[ a ] = flags[ a ];
                    if ( flags[ a ] != 0 ) lift.native_axles |= 1u << a;
                }
            }

            // an existing controller must agree with the flag array on the axle
            // count; a mismatch means the layout shifted in a game update
            if ( controller != nullptr
                && prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_count ) != lift.axle_count )
            {
                continue;
            }

            if ( controller == nullptr ) continue;

            const auto state = prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state );
            if ( state != 0 && lift.applied_mask == 0 )
            {
                // nothing can be up with an empty mask, but the native toggle
                // still flipped the state — a mask of 0 never animates, so the
                // controller would stay "raising" forever; bring it back to down
                prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) = 0;
            }
            else if ( state != 0 )
            {
                // raised or moving: leave the mask and flags untouched — flipping
                // them mid-air leaves a mixed progress state the game cannot resolve
                continue;
            }

            const auto stored = settings::g_settings.axle_masks[ i ];
            // only natively liftable axles may be selected; mask off anything
            // else so a stale settings value can never reach the game
            uint32_t selection = ( stored < 0 ? lift.native_axles : static_cast< uint32_t >( stored ) )
                & lift.native_axles;
            if ( reverse_override )
            {
                selection = lift.native_axles & ( 0u - lift.native_axles ); // first liftable axle only
            }

            for ( uint32_t a = 0; a < lift.axle_count; ++a )
            {
                flags[ a ] = ( ( selection >> a ) & 1 ) != 0
                    ? lift.orig_flags[ a ]
                    : static_cast< uint8_t >( 0 );
            }

            prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_mask ) = selection;
            lift.applied_mask = selection;
        }

        // the override mask is in place now: command the raise like the native key
        if ( reverse_just_engaged )
        {
            if ( game_actor != nullptr )
            {
                prism::offsets::field< uint8_t >( game_actor, prism::offsets::game_actor_lift_state ) |= 2u;
            }
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                auto* controller = prism::offsets::field< void* >( trailers[ i ], prism::offsets::trailer_lift_controller );
                if ( controller != nullptr && lift_axles[ i ].applied_mask != 0
                    && lift_axles[ i ].applied_mask != ~0u
                    && prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) == 0 )
                {
                    prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state ) = 2; // raising
                }
            }
            reverse_auto_lift = true;
        }

        // temporary diagnostics: dump lift internals to the game log ~4x/second
        static float dump_timer = 0.f;
        dump_timer += dt;
        if ( dump_timer > 0.25f )
        {
            dump_timer = 0.f;
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                auto* axle_data = prism::offsets::field< void* >( trailers[ i ], prism::offsets::trailer_axle_data );
                auto* controller = prism::offsets::field< void* >( trailers[ i ], prism::offsets::trailer_lift_controller );
                if ( axle_data == nullptr || controller == nullptr ) continue;
                const auto state = prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state );
                const auto flag_count = prism::offsets::field< uint64_t >( axle_data, prism::offsets::axle_data_flags_count );
                const auto* flags = prism::offsets::field< uint8_t* >( axle_data, prism::offsets::axle_data_flags );
                const auto prog_count = prism::offsets::field< uint64_t >( controller, prism::offsets::lift_controller_progress_count );
                const auto* progress = prism::offsets::field< float* >( controller, prism::offsets::lift_controller_progress );
                const auto ctrl_mask = prism::offsets::field< uint32_t >( controller, prism::offsets::lift_controller_axle_mask );
                if ( flag_count == 0 || flag_count > 16 || flags == nullptr ) continue;

                std::string flag_str, prog_str;
                for ( uint64_t a = 0; a < flag_count; ++a ) flag_str += fmt::format( "{}", flags[ a ] );
                for ( uint64_t a = 0; a < prog_count && a < 16; ++a ) prog_str += fmt::format( " {:.2}", progress[ a ] );
                CCore::g_instance->info( "lift dbg T{} state={} flags={} prog=[{}] ctrl_mask={:#x}",
                    i, state, flag_str, prog_str, ctrl_mask );
            }
        }
    }

    void CTrailerManipulation::render_trailer_steering( const uint32_t i ) const
    {
        bool lock = locked_ui[ i ];
        if ( ImGui::Checkbox( "Locked##steering", &lock ) )
        {
            set_locked( i, lock );
        }

        ImGui::BeginDisabled( !locked_ui[ i ] );
        bool changed = ImGui::SliderFloat( "Angle", &angle_ui[ i ], -1.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp );

        ImGui::PushItemFlag( ImGuiItemFlags_ButtonRepeat, true );

        if ( ImGui::ArrowButton( "rotate_left", ImGuiDir_Left ) )
        {
            angle_ui[ i ] = ( std::max )( angle_ui[ i ] - 0.02f, -1.f );
            changed = true;
        }
        ImGui::SameLine();
        if ( ImGui::Button( "center" ) )
        {
            centering_ui[ i ] = true; // eased to 0 in update() at steering_speed
        }
        ImGui::SameLine();
        if ( ImGui::ArrowButton( "rotate_right", ImGuiDir_Right ) )
        {
            angle_ui[ i ] = ( std::min )( angle_ui[ i ] + 0.02f, 1.f );
            changed = true;
        }

        ImGui::PopItemFlag();
        ImGui::EndDisabled();

        if ( changed )
        {
            // applied by the game's next steering advance, on its own thread
            locked_steering[ i ].angle.store( angle_ui[ i ] );
        }
    }

    void CTrailerManipulation::render_trailer_lift( const uint32_t i ) const
    {
        const auto& lift = lift_axles[ i ];
        if ( lift.axle_data == nullptr )
        {
            ImGui::TextDisabled( "No axle data" );
            return;
        }

        auto* controller = prism::offsets::field< void* >( trailers[ i ], prism::offsets::trailer_lift_controller );
        const auto state = controller != nullptr
            ? prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state )
            : 0;
        ImGui::Text( "Lift: %s", state != 0 ? "raised" : "down" );

        // axles moving or up with a real mask physically occupy the air — the
        // selection must stay put until they are back on the ground; a state
        // desynced from an empty mask (nothing lifted) stays editable so the
        // trailer is never soft-locked
        bool locked = state != 0 && lift.applied_mask != 0;
        if ( locked )
        {
            ImGui::TextWrapped( "Lower the axles to change which ones lift." );
            ImGui::TextWrapped( "Abaixe os eixos para alterar quais serao levantados." );
        }

        if ( lift.native_axles == 0 )
        {
            ImGui::TextDisabled( "No liftable axles on this trailer" );
            return;
        }

        auto& stored = settings::g_settings.axle_masks[ i ];
        // only natively liftable axles are selectable, matching update()
        uint32_t selection = ( stored < 0 ? lift.native_axles : static_cast< uint32_t >( stored ) )
            & lift.native_axles;
        if ( reverse_override )
        {
            selection = lift.native_axles & ( 0u - lift.native_axles );
            locked = true;
            ImGui::TextWrapped( "Reverse engaged: only the first liftable axle is up." );
            ImGui::TextWrapped( "Re engatada: somente o primeiro eixo elevavel esta levantado." );
        }

        ImGui::BeginDisabled( locked );
        uint32_t next = selection;
        for ( uint32_t a = 0; a < lift.axle_count; ++a )
        {
            if ( ( lift.native_axles >> a ) & 1 )
            {
                bool on = ( selection >> a ) & 1;
                const auto label = fmt::format( "Axle {}##axle", a + 1 );
                if ( ImGui::Checkbox( label.c_str(), &on ) )
                {
                    next = on ? next | ( 1u << a ) : next & ~( 1u << a );
                }
            }
            else
            {
                ImGui::TextDisabled( "Axle %u", a + 1 );
            }
            if ( a + 1 < lift.axle_count ) ImGui::SameLine();
        }
        ImGui::EndDisabled();

        if ( next != selection )
        {
            stored = static_cast< int32_t >( next );
            settings::save();
        }
    }

    void CTrailerManipulation::render()
    {
        ImGui::Begin( "Trailer Manipulation" );

        if ( !this->valid_ )
        {
            ImGui::TextWrapped( "Trailer steering is unavailable for this game version, see the game log." );
        }
        else if ( trailer_count == 0 )
        {
            ImGui::Text( "No trailers found" );
        }
        else
        {
            ImGui::Text( "%u trailer%s found", trailer_count, trailer_count > 1 ? "s" : "" );
            bool reverse_lift = settings::g_settings.reverse_lift;
            if ( ImGui::Checkbox( "Lift first axle in reverse / Erguer 1o eixo na re", &reverse_lift ) )
            {
                settings::g_settings.reverse_lift = reverse_lift;
                settings::save();
            }
            ImGui::Spacing();
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                const auto trailer_name = fmt::format( "Trailer {}", i );
                ImGui::PushID( trailer_name.c_str() );
                if ( ImGui::CollapsingHeader( trailer_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen ) )
                {
                    if ( steering_data( trailers[ i ] ) != nullptr )
                    {
                        ImGui::SeparatorText( "Steering" );
                        this->render_trailer_steering( i );
                    }
                    else
                    {
                        ImGui::TextDisabled( "No steerable wheels" );
                    }

                    ImGui::SeparatorText( "Axle lift" );
                    this->render_trailer_lift( i );
                }
                ImGui::PopID();
            }
        }

        ImGui::End();
    }
}
