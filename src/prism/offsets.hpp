#pragma once
#include <cstdint>

// Field offsets for the trailer steering feature, re-derived for game version 1.61
// (ETS2 and ATS share them). The structs in prism/ still describe the 1.53-era layout
// and are NOT valid for these fields anymore, so read them through here instead.
//
// How each was found (offline, from the exe):
//  - steering / steering data: physics_trailer_u::steering_advance stores the angle to
//    +0x690 and passes the pointer at +0x698 to set_individual_steering
//  - slave trailer: prism::physics_trailer_u::connect_slave tests +0x1060 against null
//  - first trailer: the `cheat repair` handler walks game_actor +0x18 (truck) and +0xC8
namespace ts_extra_utilities::prism::offsets
{
    constexpr uint32_t game_actor_trailer = 0x00C8; // game_trailer_actor_u*, first trailer
    constexpr uint32_t trailer_slave = 0x1060; // game_trailer_actor_u*, next trailer in the chain
    constexpr uint32_t trailer_steering = 0x0690; // float, -1..1, written by the game every advance
    constexpr uint32_t trailer_steering_data = 0x0698; // vehicle_wheel_steering_data_t*, null when not steerable

    // Axle lift, 1.61. Each trailer owns a lift controller at +0x6A0 (only created
    // when the chassis data marks at least one axle liftable). The controller's init
    // builds a per-axle bitmask at +0x38 from the axle flags; its per-frame update
    // only animates the lift progress of masked axles, and the wheel-position pass
    // raises an axle when its flag byte is set and the state byte is nonzero.
    // Writing the mask + flag bytes picks which axles respond to the game's own
    // trailer lift key. Only natively liftable axles may be selected: forcing a
    // non-liftable axle crashes the wheel/suspension update, so the selection is
    // always masked to the original flag set, and nothing is written while the
    // controller state is not "down".
    constexpr uint32_t trailer_lift_controller = 0x06A0; // axle lift controller
    constexpr uint32_t lift_controller_state = 0x003C; // byte: 0 down, 1 lowering, 2 raising, 3 up
    constexpr uint32_t lift_controller_axle_mask = 0x0038; // uint32_t, one bit per axle
    constexpr uint32_t lift_controller_axle_count = 0x0008; // uint32_t
    constexpr uint32_t lift_controller_progress = 0x0018; // float* array, per-axle lift progress
    constexpr uint32_t lift_controller_progress_count = 0x0020; // u64, same length as axles

    // The axle/chassis dyn object at trailer+0x200 holds a byte flag per axle at
    // +0x5C0 (array_t: ptr +0x5C8, count +0x5D0). The wheel-position update reads the
    // flag plus controller state to decide whether an axle is raised.
    constexpr uint32_t trailer_axle_data = 0x0200; // axle/chassis dyn object
    constexpr uint32_t axle_data_flags = 0x05C8; // uint8_t* per-axle flags
    constexpr uint32_t axle_data_flags_count = 0x05D0; // u64 flag count = axle count

    // Game-actor lift bookkeeping byte: bit0 = truck axles raised, bit1 = trailer
    // axles raised. The native lift key toggles the bits so its "is lifted" checks,
    // telemetry and the toggle direction stay consistent; the plugin keeps them in
    // sync when it commands a lift itself.
    constexpr uint32_t game_actor_lift_state = 0x0248;

    template < typename T >
    T& field( void* object, const uint32_t offset )
    {
        return *reinterpret_cast< T* >( reinterpret_cast< uint8_t* >( object ) + offset );
    }
}
