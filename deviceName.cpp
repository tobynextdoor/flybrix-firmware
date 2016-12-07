#include "devicename.h"

#include "debug.h"
#include <Arduino.h>
#include <cstring>

DeviceName::DeviceName() : DeviceName("FLYBRIX") {
}

DeviceName::DeviceName(const String& name) : DeviceName(name) {
}

DeviceName::DeviceName(const char* name) {
    strcpy(value, name);
}

bool badChar(char c) {
    if (!c) {
        return false;
    }
    if (c >= 'a' && c <= 'z') {
        return false;
    }
    if (c >= 'A' && c <= 'Z') {
        return false;
    }
    if (c >= '0' && c <= '9') {
        return false;
    }
    for (char c_legal : " _-") {
        if (c == c_legal) {
            return false;
        }
    }
    return true;
}

bool DeviceName::verify() const {
    for (char c : value) {
        if (badChar(c)) {
            DebugPrint(
                "Illegal character in name!"
                " "
                "Names are limited to 0-9, a-z, A-Z, ' ', '_', '-'!");
            return false;
        }
        if (!c) {
            return true;
        }
    }
    DebugPrint("Given device name is too long (max 19 characters)!");
    return false;
}
