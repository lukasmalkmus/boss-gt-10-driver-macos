// IOKit-typed access to an owned device, for the stream engine only.

#pragma once

#include <IOKit/usb/IOUSBLib.h>

#include "GT10USBDevice.h"

// The opened interface for a role, or NULL when it is not held.
IOUSBInterfaceInterface650 **GT10USBDeviceInterface(GT10USBDevice *dev, GT10InterfaceRole role);
