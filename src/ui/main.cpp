#include "filecopierwindow.h"
#include "ntfspipeserver.h"
#include <QApplication>
#include <QStyleFactory>
#include <QMessageBox>
#include <windows.h>

// ═══ Windows 服务入口 ═══
static SERVICE_STATUS        g_svcStatus = {};
static SERVICE_STATUS_HANDLE g_svcHandle = nullptr;

static void WINAPI svcCtrlHandler(DWORD ctrl)
{
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
        g_svcStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_svcHandle, &g_svcStatus);
        NTFSPipeServer::stop();
        g_svcStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_svcHandle, &g_svcStatus);
        break;
    default:
        SetServiceStatus(g_svcHandle, &g_svcStatus);
    }
}

static void WINAPI svcMain(DWORD /*argc*/, LPWSTR* /*argv*/)
{
    g_svcHandle = RegisterServiceCtrlHandlerW(L"MediaFileOrganizer", svcCtrlHandler);
    g_svcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_svcStatus.dwCurrentState = SERVICE_RUNNING;
    g_svcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    SetServiceStatus(g_svcHandle, &g_svcStatus);

    NTFSPipeServer::run();

    g_svcStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_svcHandle, &g_svcStatus);
}

// ═══ 主入口 ═══
int main(int argc, char *argv[])
{
    // ─── 服务模式 ───
    if (argc > 1 && qstrcmp(argv[1], "/service") == 0) {
        SERVICE_TABLE_ENTRYW table[] = {
            { (LPWSTR)L"MediaFileOrganizer", svcMain },
            { nullptr, nullptr }
        };
        StartServiceCtrlDispatcherW(table);
        return 0;
    }

    QApplication a(argc, argv);

    // ─── 安装服务 ───
    if (argc > 1 && qstrcmp(argv[1], "/install") == 0) {
        QString exePath = QApplication::applicationFilePath();
        QString errMsg;
        if (ServiceInstaller::install(exePath, &errMsg)) {
            QMessageBox::information(nullptr, "安装成功",
                "NTFS 快速扫描服务已安装并启动。\n无需管理员即可快速扫描 NTFS 磁盘。");
        } else {
            QMessageBox::warning(nullptr, "安装失败", errMsg.isEmpty() ?
                "无法安装服务。请以管理员身份运行此程序。" : errMsg);
        }
        return 0;
    }

    // ─── 卸载服务 ───
    if (argc > 1 && qstrcmp(argv[1], "/uninstall") == 0) {
        if (ServiceInstaller::uninstall()) {
            QMessageBox::information(nullptr, "卸载成功", "NTFS 快速扫描服务已卸载。");
        } else {
            QMessageBox::warning(nullptr, "卸载失败", "服务可能未安装或需要管理员权限。");
        }
        return 0;
    }

    // ─── 正常 GUI 模式 ───
    a.setStyle(QStyleFactory::create("Fusion"));
    QFont font = a.font();
    font.setPointSize(9);
    a.setFont(font);

    FileCopierWindow w;
    w.show();
    return a.exec();
}
