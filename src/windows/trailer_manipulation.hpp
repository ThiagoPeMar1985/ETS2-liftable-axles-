#pragma once
#include <cstdint>

#include "window.hpp"

namespace ts_extra_utilities
{
    // Per-axle trailer lift control: pick which natively liftable axles respond
    // to the game's own trailer lift key, plus the reverse-gear assist that
    // raises the first liftable axle automatically.
    class CTrailerManipulation : public CWindow
    {
    private:
        bool valid_ = false;

        void render_trailer_lift( uint32_t i ) const;

    public:
        CTrailerManipulation();
        ~CTrailerManipulation() override;

        bool init() override;
        // every frame, also while the window is hidden: refreshes the trailer chain
        // and applies the axle selection / reverse assist
        void update( float dt );
        void render() override;
    };
}
