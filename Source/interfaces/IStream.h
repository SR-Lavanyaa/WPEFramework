#pragma once

#include "Module.h"

#define WPEPLAYER_PROCESS_NODE_ID "/tmp/player"

namespace WPEFramework {
namespace Exchange {

    struct IStream : virtual public Core::IUnknown {
        enum { ID = ID_STREAM };

        enum state {
            Idle = 0,
            Loading,
            Prepared,
            Paused,
            Playing,
            Error
        };

        enum streamtype {
            Undefined = 0,
            Cable = 1,
            Handheld = 2,
            Satellite = 4,
            Terrestrial = 8,
            DAB = 16,
            RF = 31,
            IP = 32,
       };

        enum drmtype {
            None = 0,
            ClearKey,
            PlayReady,
            Widevine,
            Unknown
        };

        struct IControl : virtual public Core::IUnknown {
            enum { ID = ID_STREAM_CONTROL };

            struct IGeometry : virtual public Core::IUnknown {
                enum { ID = ID_STREAM_CONTROL_GEOMETRY };

                virtual ~IGeometry() {}

                virtual uint32_t X() const = 0;
                virtual uint32_t Y() const = 0;
                virtual uint32_t Z() const = 0;
                virtual uint32_t Width() const = 0;
                virtual uint32_t Height() const = 0;
            };

            struct ICallback : virtual public Core::IUnknown {
                enum { ID = ID_STREAM_CONTROL_CALLBACK };

                virtual ~ICallback() {}

                virtual void TimeUpdate(const uint64_t position) = 0;
            };

            virtual ~IControl(){};

            virtual RPC::IValueIterator* Speeds() const = 0;
            virtual void Speed(const int32_t request) = 0;
            virtual int32_t Speed() const = 0;
            virtual void Position(const uint64_t absoluteTime) = 0;
            virtual uint64_t Position() const = 0;
            virtual void TimeRange(uint64_t& begin /* @out */, uint64_t& end /* @out */) const = 0;
            virtual IGeometry* Geometry() const = 0;
            virtual void Geometry(const IGeometry* settings) = 0;
            virtual void Callback(IControl::ICallback* callback) = 0;
        };

        struct ICallback : virtual public Core::IUnknown {
            enum { ID = ID_STREAM_CALLBACK };

            virtual ~ICallback() {}

            virtual void DRM(const uint32_t state) = 0;
            virtual void StateChange(const state newState) = 0;
        };

        virtual ~IStream() {}

        virtual string Metadata() const = 0;
        virtual streamtype Type() const = 0;
        virtual drmtype DRM() const = 0;
        virtual IControl* Control() = 0;
        virtual void Callback(IStream::ICallback* callback) = 0;
        virtual state State() const = 0;
        virtual uint32_t Load(const string& configuration) = 0;
    };

    struct IPlayer : virtual public Core::IUnknown {
        enum { ID = ID_PLAYER };

        virtual ~IPlayer() {}
        virtual IStream* CreateStream(const IStream::streamtype streamType) = 0;
        virtual uint32_t Configure(PluginHost::IShell* service) = 0;
    };

} // namespace Exchange
}
