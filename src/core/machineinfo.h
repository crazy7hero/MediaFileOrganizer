#ifndef MACHINEINFO_H
#define MACHINEINFO_H

#include <QString>

class MachineInfo
{
public:
    static QString getCpuId();
    static QString getDiskSerial();
    static QString getMotherboardSerial();
    static QString getUniqueKey();
};

#endif
