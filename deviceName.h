#ifndef DEVICENAME_H
#define DEVICENAME_H

class String;

struct __attribute__((packed)) DeviceName {
   public:
    DeviceName();
    DeviceName(const String& name);
    DeviceName(const char* name);

    bool verify() const;

    char value[9];
};

static_assert(sizeof(DeviceName) == sizeof(char) * 9, "Data is not packed");

#endif /* DEVICENAME_H */
