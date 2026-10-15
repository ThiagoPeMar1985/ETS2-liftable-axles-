#include "trailer_manipulation.hpp"

#include <atomic>
#include <cfloat>

#include "imgui.h"

#include "common/scssdk_telemetry_truck_common_channels.h"

#include "core.hpp"
#include "memory/memory_utils.hpp"
#include "prism/offsets.hpp"
#include "settings/settings.hpp"

namespace ts_extra_utilities
{
    constexpr uint32_t max_trailers = 20;

    // Console line executor (1.61 VA 0x1401e0c20, 1.61.1.9s 0x1401e0cc0 — the
    // pattern below stays unique across both): runs one command line exactly
    // like typing it in the console — the same function the command queue and the
    // console input go through. rcx is a char** holding the line text, edx is the
    // queue context; the game itself passes -1 for direct execution.
    using console_exec_fn = int( __fastcall )( char**, int );
    console_exec_fn* g_console_exec = nullptr;
    bool console_exec_missing = false;

    bool console_exec( const char* line )
    {
        if ( g_console_exec == nullptr && !console_exec_missing )
        {
            g_console_exec = memory::get_function_from_pattern< console_exec_fn >(
                "40 55 41 54 41 55 48 8D AC 24 00 F4 FF FF 48 81 EC 00 0D 00 00 44 8B EA 4C 8B E1" );
            if ( g_console_exec == nullptr )
            {
                console_exec_missing = true;
                CCore::g_instance->warning( "Console executor not found, trailer covers keybinds are unavailable" );
            }
        }
        if ( g_console_exec == nullptr ) return false;

        auto* text = const_cast< char* >( line );
        g_console_exec( &text, -1 );
        return true;
    }

    // Last g_trailer_cables_mode value we sent; -1 when the plugin has not sent
    // one yet (or the game state is unknown), which the keybinds treat as closed.
    int covers_mode = -1;

    void set_covers_mode( const int mode )
    {
        if ( console_exec( fmt::format( "g_trailer_cables_mode {}", mode ).c_str() ) )
        {
            covers_mode = mode;
        }
    }

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
        void* controller = nullptr; // trailer+0x6C8, detects a trailer swap/rebuild on change
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

    void release_all()
    {
        for ( uint32_t i = 0; i < max_trailers; ++i )
        {
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
    }

    bool CTrailerManipulation::init()
    {
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

    namespace ui
    {
        // On/off pill switch drawn with the draw list (ImGui has no native one).
        bool toggle_switch( const char* id, bool* v )
        {
            const float h = ImGui::GetFrameHeight();
            const float w = h * 1.9f;
            ImGui::InvisibleButton( id, ImVec2( w, h ) );
            const bool changed = ImGui::IsItemClicked();
            if ( changed ) *v = !*v;
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetItemRectMin();
            const float r = h * 0.5f;
            const ImU32 bg = *v ? IM_COL32( 46, 160, 90, 255 ) : IM_COL32( 92, 94, 102, 255 );
            dl->AddRectFilled( p, ImVec2( p.x + w, p.y + h ), bg, r );
            const float kx = *v ? p.x + w - r : p.x + r;
            dl->AddCircleFilled( ImVec2( kx, p.y + r ), r - 3.5f, IM_COL32( 245, 245, 245, 255 ) );
            return changed;
        }

        // Moves the cursor so the next widget of `width` hugs the row's right edge.
        void right_side( const float width )
        {
            ImGui::SameLine( 0.f, 0.f );
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width );
        }

        void section( const char* text )
        {
            ImGui::Spacing();
            ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.55f, 0.80f, 0.62f, 1.f ) );
            ImGui::TextUnformatted( text );
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::Spacing();
        }

        // Shows the bound key; clicking it starts the capture in settings.
        void keybind_button( const settings::Action action )
        {
            const auto text = settings::is_capturing( action )
                ? std::string( "Pressione uma tecla..." )
                : settings::key_name( settings::key( action ) );
            if ( ImGui::Button( fmt::format( "{}##kb{}", text, static_cast< int >( action ) ).c_str(), ImVec2( 150.f, 0.f ) ) )
            {
                settings::begin_capture( action );
            }
        }
    }

    void CTrailerManipulation::render_trailer_lift( const uint32_t i ) const
    {
        const auto& lift = lift_axles[ i ];
        if ( lift.axle_data == nullptr )
        {
            ImGui::TextDisabled( "Sem dados de eixo" );
            return;
        }

        auto* controller = prism::offsets::field< void* >( trailers[ i ], prism::offsets::trailer_lift_controller );
        const auto state = controller != nullptr
            ? prism::offsets::field< uint8_t >( controller, prism::offsets::lift_controller_state )
            : 0;
        ImGui::Text( "Eixos: %s", state != 0 ? "levantados" : "no chao" );

        // axles moving or up with a real mask physically occupy the air — the
        // selection must stay put until they are back on the ground; a state
        // desynced from an empty mask (nothing lifted) stays editable so the
        // trailer is never soft-locked
        bool locked = state != 0 && lift.applied_mask != 0;
        if ( locked )
        {
            ImGui::TextDisabled( "Abaixe os eixos para alterar quais serao levantados." );
        }

        if ( lift.native_axles == 0 )
        {
            ImGui::TextDisabled( "Nenhum eixo elevavel nesta carreta" );
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
            ImGui::TextDisabled( "Re engatada: somente o primeiro eixo elevavel esta levantado." );
        }

        ImGui::BeginDisabled( locked );
        uint32_t next = selection;
        const float switch_w = ImGui::GetFrameHeight() * 1.9f;
        for ( uint32_t a = 0; a < lift.axle_count; ++a )
        {
            if ( ( lift.native_axles >> a ) & 1 )
            {
                bool on = ( selection >> a ) & 1;
                ImGui::Text( "Eixo %u", a + 1 );
                ui::right_side( switch_w );
                const auto id = fmt::format( "##axle{}", a );
                if ( ui::toggle_switch( id.c_str(), &on ) )
                {
                    next = on ? next | ( 1u << a ) : next & ~( 1u << a );
                }
            }
            else
            {
                ImGui::TextDisabled( "Eixo %u", a + 1 );
                ui::right_side( switch_w );
                ImGui::TextDisabled( "fixo" );
            }
        }
        ImGui::EndDisabled();

        if ( next != selection )
        {
            stored = static_cast< int32_t >( next );
            settings::save();
        }
    }

    void CTrailerManipulation::covers_open()
    {
        // mode 0 = no cables anywhere: every cover in the chain opens at once
        set_covers_mode( 0 );
    }

    void CTrailerManipulation::covers_close()
    {
        // g_trailer_cables_mode: 0 = no cables (all covers open), 1 = first
        // trailer only, 2+ = the whole chain. Each press closes one more
        // trailer; when everything is already closed the value stays put.
        const int cap = trailer_count < 2 ? static_cast< int >( trailer_count ) : 2;
        if ( covers_mode < 0 || covers_mode >= cap )
        {
            set_covers_mode( cap ); // unknown -> all closed; already closed -> reaffirm
        }
        else
        {
            set_covers_mode( covers_mode + 1 );
        }
    }

    void CTrailerManipulation::render()
    {
        ImGui::SetNextWindowSizeConstraints( ImVec2( 600.f, 300.f ), ImVec2( FLT_MAX, FLT_MAX ) );
        ImGui::SetNextWindowSize( ImVec2( 640.f, 420.f ), ImGuiCond_FirstUseEver );

        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 16.f, 12.f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 10.f, 8.f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 4.f );
        ImGui::PushStyleColor( ImGuiCol_WindowBg, ImVec4( 0.09f, 0.10f, 0.12f, 0.98f ) );
        ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.20f, 0.22f, 0.26f, 1.f ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.28f, 0.55f, 0.36f, 1.f ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( 0.20f, 0.65f, 0.40f, 1.f ) );
        ImGui::PushStyleColor( ImGuiCol_Header, ImVec4( 0.16f, 0.18f, 0.21f, 1.f ) );
        ImGui::PushStyleColor( ImGuiCol_HeaderHovered, ImVec4( 0.22f, 0.24f, 0.28f, 1.f ) );
        ImGui::PushStyleColor( ImGuiCol_Separator, ImVec4( 0.28f, 0.30f, 0.34f, 1.f ) );

        ImGui::Begin( "Trailer System" );

        // covers section — always visible so the keybinds are reachable even
        // with no trailer attached
        ui::section( "CARROCERIA / LONAS" );

        ImGui::TextUnformatted( "Abrir carroceria" );
        ui::right_side( 150.f );
        ui::keybind_button( settings::Action::COVERS_NEXT );

        ImGui::TextUnformatted( "Fechar carroceria" );
        ui::right_side( 150.f );
        ui::keybind_button( settings::Action::COVERS_TOGGLE );

        if ( covers_mode >= 0 )
        {
            ImGui::TextDisabled( "Modo dos cabos: %d", covers_mode );
        }

        ui::section( "EIXOS / LIFT" );

        ImGui::TextUnformatted( "Erguer 1o eixo na re" );
        ui::right_side( ImGui::GetFrameHeight() * 1.9f );
        bool reverse_lift = settings::g_settings.reverse_lift;
        if ( ui::toggle_switch( "##reverse_lift", &reverse_lift ) )
        {
            settings::g_settings.reverse_lift = reverse_lift;
            settings::save();
        }

        ImGui::Spacing();
        if ( !this->valid_ )
        {
            ImGui::TextDisabled( "Lift indisponivel nesta versao do jogo, veja o game log." );
        }
        else if ( trailer_count == 0 )
        {
            ImGui::TextDisabled( "Nenhuma carreta conectada" );
        }
        else
        {
            for ( uint32_t i = 0; i < trailer_count; ++i )
            {
                const auto trailer_name = fmt::format( "Carreta {}", i + 1 );
                ImGui::PushID( trailer_name.c_str() );
                if ( ImGui::CollapsingHeader( trailer_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen ) )
                {
                    ImGui::Indent( 8.f );
                    this->render_trailer_lift( i );
                    ImGui::Unindent( 8.f );
                }
                ImGui::PopID();
            }
        }

        ImGui::End();
        ImGui::PopStyleColor( 7 );
        ImGui::PopStyleVar( 3 );
    }
}
