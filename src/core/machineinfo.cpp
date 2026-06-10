#include "machineinfo.h"
#include <windows.h>
#include <intrin.h>
#include <QProcess>
#include <QByteArray>
#include <QCryptographicHash>

QString MachineInfo::getCpuId()
{
    int cpuInfo[4] = { 0 };
    __cpuid(cpuInfo, 1);
    QString cpuId = QString("%1%2%3%4")
                        .arg(cpuInfo[0], 8, 16, QChar('0'))
                        .arg(cpuInfo[1], 8, 16, QChar('0'))
                        .arg(cpuInfo[2], 8, 16, QChar('0'))
                        .arg(cpuInfo[3], 8, 16, QChar('0'));
    return cpuId;
}

QString MachineInfo::getDiskSerial()
{
    QProcess p;
    p.start("wmic diskdrive get serialnumber");
    p.waitForFinished();
    QByteArray ba = p.readAll();
    QString str = QString(ba).replace("SerialNumber", "").replace("\r\n", "").trimmed();
    return str.left(16);
}

QString MachineInfo::getMotherboardSerial()
{
    QProcess p;
    p.start("wmic baseboard get serialnumber");
    p.waitForFinished();
    QByteArray ba = p.readAll();
    QString str = QString(ba).replace("SerialNumber", "").replace("\r\n", "").trimmed();
    return str.left(16);
}

QString MachineInfo::getUniqueKey()
{
    QString all = getCpuId() + getDiskSerial() + getMotherboardSerial();
    QByteArray hash = QCryptographicHash::hash(all.toUtf8(), QCryptographicHash::Sha256);
    return hash.toHex().left(32);
}
