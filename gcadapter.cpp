// Ported from the Dolphin Emulator Project
// Licensed under GPLv2+

#include <thread>
#include <chrono>
#include <mutex>

#include <libusb.h>
#include <oplog.h>

#include "gcadapter.h"
#include "Controller #1.1.h"

#define TEST_DEADZONE(num, zone) (std::abs(num) > zone) ? num : 0;

using namespace std::literals::chrono_literals;

namespace GCAdapter
{
    enum ControllerTypes
    {
        CONTROLLER_NONE = 0,
        CONTROLLER_WIRED = 1,
        CONTROLLER_WIRELESS = 2
    };

    static const uint8_t STICK_CENTER = 0x80;

    static uint8_t s_controller_type[4] = { CONTROLLER_NONE, CONTROLLER_NONE, CONTROLLER_NONE, CONTROLLER_NONE };
    static uint8_t s_controller_rumble[4];

    static std::mutex s_mutex;
    static uint8_t s_controller_payload[37];
    static uint8_t s_controller_payload_swap[37];

    static int s_controller_payload_size = 0;

    static std::thread s_adapter_thread;
    static Flag s_adapter_thread_running;

    static std::thread s_adapter_detect_thread;
    static Flag s_adapter_detect_thread_running;

    static std::function<void(void)> s_detect_callback;

    static bool s_detected = false;
    static libusb_device_handle* s_handle = nullptr;
    static bool s_libusb_driver_not_supported = false;
    static libusb_context* s_libusb_context = nullptr;
    static bool s_libusb_hotplug_enabled = false;
    static libusb_hotplug_callback_handle s_hotplug_handle;

    static uint8_t s_endpoint_in = 0;
    static uint8_t s_endpoint_out = 0;

    static bool s_switch_l_z_trig = true;

    static void Read()
    {
        while (s_adapter_thread_running.IsSet())
        {
            libusb_interrupt_transfer(s_handle, s_endpoint_in, s_controller_payload_swap, sizeof(s_controller_payload_swap), &s_controller_payload_size, 16);

            {
                std::lock_guard<std::mutex> lk(s_mutex);
                std::swap(s_controller_payload_swap, s_controller_payload);
            }

            std::this_thread::yield();
        }
    }

    static bool CheckDeviceAccess(libusb_device* device)
    {
        int ret;
        libusb_device_descriptor desc;
        int dRet = libusb_get_device_descriptor(device, &desc);
        if (dRet)
        {
            // could not acquire the descriptor, no point in trying to use it.
            LOG_ERROR(GCAdapter) << "libusb_get_device_descriptor failed with error: " << dRet;
            return false;
        }

        if (desc.idVendor == 0x057e && desc.idProduct == 0x0337)
        {
            LOG_DEBUG(GCAdapter) << "Found GC Adapter with Vendor: 0x" << std::hex << desc.idVendor << " Product: 0x" << std::hex << desc.idProduct << " Devnum: 1";

            uint8_t bus = libusb_get_bus_number(device);
            uint8_t port = libusb_get_device_address(device);
            ret = libusb_open(device, &s_handle);
            if (ret)
            {
                if (ret == LIBUSB_ERROR_ACCESS)
                {
                    if (dRet)
                    {
                        LOG_ERROR(GCAdapter) << "no access to this device: Bus " << bus << " Device " << port << ": ID ????:???? (couldn't get id).";
                    }
                    else
                    {
                        LOG_ERROR(GCAdapter) << "no access to this device: Bus " << bus << " Device " << port << ": ID 0x" << std::hex << desc.idVendor << ":0x" << std::hex << desc.idProduct << ".";
                    }
                }
                else
                {
                    LOG_ERROR(GCAdapter) << "libusb_open failed to open device with error = " << ret;
                    if (ret == LIBUSB_ERROR_NOT_SUPPORTED)
                        s_libusb_driver_not_supported = true;
                }
            }
            else if ((ret = libusb_kernel_driver_active(s_handle, 0)) == 1)
            {
                if ((ret = libusb_detach_kernel_driver(s_handle, 0)) && ret != LIBUSB_ERROR_NOT_SUPPORTED)
                {
                    LOG_ERROR(GCAdapter) << "libusb_detach_kernel_driver failed with error: " << ret;
                }
            }
            // this split is needed so that we don't avoid claiming the interface when
            // detaching the kernel driver is successful
            if (ret != 0 && ret != LIBUSB_ERROR_NOT_SUPPORTED)
            {
                return false;
            }
            else if ((ret = libusb_claim_interface(s_handle, 0)))
            {
                LOG_ERROR(GCAdapter) << "libusb_claim_interface failed with error: " << ret;
            }
            else
            {
                return true;
            }
        }
        return false;
    }

    static void AddGCAdapter(libusb_device* device)
    {
        libusb_config_descriptor *config = nullptr;
        libusb_get_config_descriptor(device, 0, &config);
        for (uint8_t ic = 0; ic < config->bNumInterfaces; ic++)
        {
            const libusb_interface *interfaceContainer = &config->interface[ic];
            for (int i = 0; i < interfaceContainer->num_altsetting; i++)
            {
                const libusb_interface_descriptor *interface = &interfaceContainer->altsetting[i];
                for (uint8_t e = 0; e < interface->bNumEndpoints; e++)
                {
                    const libusb_endpoint_descriptor *endpoint = &interface->endpoint[e];
                    if (endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN)
                        s_endpoint_in = endpoint->bEndpointAddress;
                    else
                        s_endpoint_out = endpoint->bEndpointAddress;
                }
            }
        }

        int tmp = 0;
        unsigned char payload = 0x13;
        libusb_interrupt_transfer(s_handle, s_endpoint_out, &payload, sizeof(payload), &tmp, 16);

        s_adapter_thread_running.Set(true);
        s_adapter_thread = std::thread(Read);

        s_detected = true;
        if (s_detect_callback != nullptr)
            s_detect_callback();
    }

    static int HotplugCallback(libusb_context* ctx, libusb_device* dev, libusb_hotplug_event event, void* user_data)
    {
        if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
        {
            if (s_handle == nullptr && CheckDeviceAccess(dev))
                AddGCAdapter(dev);
        }
        else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT)
        {
            if (s_handle != nullptr && libusb_get_device(s_handle) == dev)
                Reset();
        }
        return 0;
    }

    static void ScanThreadFunc()
    {
        LOG_DEBUG(GCAdapter) << "GC Adapter scanning thread started";

        s_libusb_hotplug_enabled = libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG) != 0;
        if (s_libusb_hotplug_enabled)
        {
            if (libusb_hotplug_register_callback(s_libusb_context, (libusb_hotplug_event)(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT), LIBUSB_HOTPLUG_ENUMERATE, 0x057e, 0x0337, LIBUSB_HOTPLUG_MATCH_ANY, HotplugCallback, NULL, &s_hotplug_handle) != LIBUSB_SUCCESS)
                s_libusb_hotplug_enabled = false;

            if (s_libusb_hotplug_enabled)
            {
                LOG_DEBUG(GCAdapter) << "Using libUSB hotplug detection";
            }
        }

        while (s_adapter_detect_thread_running.IsSet())
        {
            if (s_libusb_hotplug_enabled)
            {
                static timeval tv = { 0, 500000 };
                libusb_handle_events_timeout(s_libusb_context, &tv);
            }
            else
            {
                if (s_handle == nullptr)
                {
                    Setup();
                    if (s_detected && s_detect_callback != nullptr)
                        s_detect_callback();
                }
                std::this_thread::sleep_for(500ms);
            }
        }
        LOG_DEBUG(GCAdapter) << "GC Adapter scanning thread stopped";
    }

    void SetAdapterCallback(std::function<void(void)> func)
    {
        s_detect_callback = func;
    }

    void Setup()
    {
        libusb_device** list;
        ssize_t cnt = libusb_get_device_list(s_libusb_context, &list);

        for (int i = 0; i < 4; i++)
        {
            s_controller_type[i] = CONTROLLER_NONE;
            s_controller_rumble[i] = 0;
        }

        for (int d = 0; d < cnt; d++)
        {
            libusb_device* device = list[d];
            if (CheckDeviceAccess(device))
                AddGCAdapter(device);
        }

        libusb_free_device_list(list, 1);
    }

    void StartScanThread()
    {
        s_adapter_detect_thread_running.Set(true);
        s_adapter_detect_thread = std::thread(ScanThreadFunc);
    }

    void StopScanThread()
    {
        if (s_adapter_detect_thread_running.TestAndClear())
        {
            s_adapter_detect_thread.join();
        }
    }

    void Init()
    {
        if (s_handle != nullptr)
            return;

        s_libusb_driver_not_supported = false;

        int ret = libusb_init(&s_libusb_context);

        if (ret)
        {
            LOG_ERROR(GCAdapter) << "libusb_init failed with error: " << ret;
            Shutdown();
        }
        else
        {
            StartScanThread();
        }
    }

    void Shutdown()
    {
        StopScanThread();
        if (s_libusb_hotplug_enabled)
            libusb_hotplug_deregister_callback(s_libusb_context, s_hotplug_handle);

        Reset();

        if (s_libusb_context)
        {
            libusb_exit(s_libusb_context);
            s_libusb_context = nullptr;
        }

        s_libusb_driver_not_supported = false;
    }

    void Reset()
    {
        if (!s_detected)
            return;

        if (s_adapter_thread_running.TestAndClear())
        {
            s_adapter_thread.join();
        }

        for (int i = 0; i < 4; i++)
            s_controller_type[i] = CONTROLLER_NONE;

        s_detected = false;

        if (s_handle)
        {
            libusb_release_interface(s_handle, 0);
            libusb_close(s_handle);
            s_handle = nullptr;
        }

        if (s_detect_callback != nullptr)
            s_detect_callback();

        LOG_DEBUG(GCAdapter) << "GC Adapter detached";
    }

    void Input(int chan, uint32_t* command)
    {
        if (s_handle == nullptr || !s_detected)
            return;

        uint8_t controller_payload_copy[37];

        {
            std::lock_guard<std::mutex> lk(s_mutex);
            std::copy(std::begin(s_controller_payload), std::end(s_controller_payload), std::begin(controller_payload_copy));
        }

        if (s_controller_payload_size != sizeof(controller_payload_copy) || controller_payload_copy[0] != LIBUSB_DT_HID)
        {
            LOG_ERROR(GCAdapter) << "error reading payload (size: " << s_controller_payload_size << ", type: 0x" << std::hex << controller_payload_copy[0] << ")";
            Reset();
        }
        else
        {
            uint8_t type = controller_payload_copy[1 + (9 * chan)] >> 4;
            if (type != CONTROLLER_NONE && s_controller_type[chan] == CONTROLLER_NONE)
            {
                LOG_DEBUG(GCAdapter) << "New device connected to Port " << chan + 1 << " of Type: 0x" << std::hex << (int)controller_payload_copy[1 + (9 * chan)];
            }

            s_controller_type[chan] = type;

            if (s_controller_type[chan] != CONTROLLER_NONE)
            {
                BUTTONS pad;
                memset(&pad, 0, sizeof(pad));
                uint8_t b1 = controller_payload_copy[1 + (9 * chan) + 1];
                uint8_t b2 = controller_payload_copy[1 + (9 * chan) + 2];

                if (b1 & (1 << 0)) pad.A_BUTTON = 1;
                if (b1 & (1 << 1)) pad.B_BUTTON = 1;
                //if (b1 & (1 << 2)) pad->button |= PAD_BUTTON_X; no n64 equivalent
                //if (b1 & (1 << 3)) pad->button |= PAD_BUTTON_Y; no n64 equivalent

                if (b1 & (1 << 4)) pad.L_DPAD = 1;
                if (b1 & (1 << 5)) pad.R_DPAD = 1;
                if (b1 & (1 << 6)) pad.D_DPAD = 1;
                if (b1 & (1 << 7)) pad.U_DPAD = 1;

                if (b2 & (1 << 0)) pad.START_BUTTON = 1;
                if (b2 & (1 << 1)) (s_switch_l_z_trig) ? pad.L_TRIG = 1 : pad.Z_TRIG = 1;
                if (b2 & (1 << 2)) pad.R_TRIG = 1;
                if (b2 & (1 << 3)) (s_switch_l_z_trig) ? pad.Z_TRIG = 1 : pad.L_TRIG = 1;

                pad.Y_AXIS = TEST_DEADZONE((int32_t)controller_payload_copy[1 + (9 * chan) + 3] - STICK_CENTER - 8, 1);
                pad.X_AXIS = TEST_DEADZONE((int32_t)controller_payload_copy[1 + (9 * chan) + 4] - STICK_CENTER - 4, 1);

                int32_t cstick_x = TEST_DEADZONE((int32_t)controller_payload_copy[1 + (9 * chan) + 5] - STICK_CENTER - 4, 0x20);
                int32_t cstick_y = TEST_DEADZONE((int32_t)controller_payload_copy[1 + (9 * chan) + 6] - STICK_CENTER - 5, 0x20);

                pad.L_CBUTTON = (cstick_x < 0);
                pad.R_CBUTTON = (cstick_x > 0);
                pad.U_CBUTTON = (cstick_y > 0);
                pad.D_CBUTTON = (cstick_y < 0);

                if (s_switch_l_z_trig)
                {
                    pad.Z_TRIG = (controller_payload_copy[1 + (9 * chan) + 7] > 0x40);
                }
                else
                {
                    pad.L_TRIG = (controller_payload_copy[1 + (9 * chan) + 7] > 0x40);
                }
                pad.R_TRIG = (controller_payload_copy[1 + (9 * chan) + 8] > 0x40);

                *command = pad.Value;
            }
        }
    }

    bool IsDetected()
    {
        return s_detected;
    }

    bool IsDriverDetected()
    {
        return !s_libusb_driver_not_supported;
    }
}