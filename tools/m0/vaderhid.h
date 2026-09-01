#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct HidDeviceInfo
{
    std::wstring devicePath;
    std::wstring manufacturer;
    std::wstring product;
    std::wstring serial;
    unsigned short vid = 0;
    unsigned short pid = 0;
    unsigned short version = 0;
    unsigned short usagePage = 0;
    unsigned short usage = 0;
    unsigned short inputReportByteLength = 0;
    unsigned short outputReportByteLength = 0;
    unsigned short featureReportByteLength = 0;
};

std::vector<HidDeviceInfo> EnumerateHidDevices();

bool IsVader5Pro(const HidDeviceInfo& device);
