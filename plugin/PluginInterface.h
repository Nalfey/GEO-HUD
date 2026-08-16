#pragma once

#include <cstdint>

// Binding for the Windower 4 native plugin ABI.
//
// Windower calls into a plugin purely through the two vtables below, so the
// declaration order, the virtual-ness and the calling conventions here are
// fixed by the host binary and cannot be changed. Types the plugin never calls
// are left incomplete on purpose.

#if !defined(WINDOWER_INTERFACE_VERSION)
#define WINDOWER_INTERFACE_VERSION 0x04070300
#endif

struct HostSettings;
class HostConsole;
class HostText;
class HostPrimitives;
class HostPacketStream;
class HostGame;

class PluginManager {
public:
    virtual HostSettings* __stdcall GetSettings(HostSettings*);
    virtual void* __stdcall GetWindowHandle();
    virtual void* __stdcall GetDirect3D8Device();
    virtual HostConsole* __stdcall GetConsole();
    virtual HostText* __stdcall GetTextHandler();
    virtual HostPrimitives* __stdcall GetPrimitiveHandler();
    virtual HostPacketStream* __stdcall GetPacketStreamHandler();
    virtual HostGame* __stdcall GetGame();
    virtual PluginManager* __thiscall Destroy(std::uint8_t);
};

class PluginBase {
public:
    virtual const char* __stdcall GetPluginAuthor() = 0;
    virtual const char* __stdcall GetPluginName() = 0;

    virtual void __stdcall Load(PluginManager* host) { plugin_manager_ = host; }
    virtual void __stdcall Unload() {}
    virtual bool __stdcall IgnoreUnload() { return false; }
    virtual void __stdcall PreRender() {}
    virtual void __stdcall PostRender() {}
    virtual void __stdcall PluginCommand(const char*) {}
    virtual bool __stdcall UnhandledCommand(const char*) { return false; }
    virtual void __stdcall IncomingText(void*, void*, void*) {}
    virtual void __stdcall OutgoingText(void*, void*, void*) {}
    virtual bool __stdcall IncomingChunk(void*, void*, void*, bool handled) { return handled; }
    virtual bool __stdcall OutgoingChunk(void*, void*, void*, bool handled) { return handled; }
    virtual bool __stdcall Mouse(void*, void*, void*, void*, bool handled) { return handled; }
    virtual bool __stdcall Keyboard(void*, void*, bool handled) { return handled; }
    virtual void __stdcall AddItem(void*, void*, void*, void*) {}
    virtual void __stdcall RemoveItem(void*, void*, void*, void*) {}
    virtual PluginBase* __thiscall Destroy(std::uint8_t) { return this; }

protected:
    PluginManager* plugin_manager_ = nullptr;
};

extern "C" {
    std::uint32_t GetInterfaceVersion();
    PluginBase* CreateInstance();
}
