#include "vaderhid.h"

#include <hidsdi.h>
#include <setupapi.h>

#include <memory>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

std::vector<HidDeviceInfo> EnumerateHidDevices()
{
    std::vector<HidDeviceInfo> result;

    GUID hidGuid = {};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return result;
    }

    SP_DEVICE_INTERFACE_DATA interfaceData = {};
    interfaceData.cbSize = sizeof(interfaceData);

    for (DWORD index = 0;; ++index) {
        if (!SetupDiEnumDeviceInterfaces(
                deviceInfoSet, nullptr, &hidGuid, index, &interfaceData)) {
            break;
        }

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(
            deviceInfoSet, &interfaceData, nullptr, 0, &requiredSize, nullptr);
        if (requiredSize == 0) {
            continue;
        }

        auto detail = std::make_unique<BYTE[]>(requiredSize);
        auto* detailData =
            reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detail.get());
        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &interfaceData,
                detailData, requiredSize, nullptr, nullptr)) {
            continue;
        }

        HidDeviceInfo info;
        info.devicePath = detailData->DevicePath;

        HANDLE handle = CreateFileW(info.devicePath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            // Keep the path entry even if it cannot be opened.
            result.push_back(std::move(info));
            continue;
        }

        HIDD_ATTRIBUTES attributes = {};
        attributes.Size = sizeof(attributes);
        if (HidD_GetAttributes(handle, &attributes)) {
            info.vid = attributes.VendorID;
            info.pid = attributes.ProductID;
            info.version = attributes.VersionNumber;
        }

        WCHAR buffer[256] = {};
        if (HidD_GetProductString(handle, buffer, sizeof(buffer))) {
            info.product = buffer;
        }
        if (HidD_GetManufacturerString(handle, buffer, sizeof(buffer))) {
            info.manufacturer = buffer;
        }
        if (HidD_GetSerialNumberString(handle, buffer, sizeof(buffer))) {
            info.serial = buffer;
        }

        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (HidD_GetPreparsedData(handle, &preparsed)) {
            HIDP_CAPS caps = {};
            if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
                info.usagePage = caps.UsagePage;
                info.usage = caps.Usage;
                info.inputReportByteLength = caps.InputReportByteLength;
                info.outputReportByteLength = caps.OutputReportByteLength;
                info.featureReportByteLength = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(preparsed);
        }

        CloseHandle(handle);
        result.push_back(std::move(info));
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return result;
}

bool IsVader5Pro(const HidDeviceInfo& device)
{
    return device.vid == 0x37D7 && device.pid == 0x2401;
}
