#include "ntfspipeserver.h"
#include "ntfsscanner.h"
#include <windows.h>
#include <QDir>
#include <QDebug>

// ─── 获取卷访问权限 ───
static bool enableBackupPrivilege()
{
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;
    TOKEN_PRIVILEGES tp;
    LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME, &tp.Privileges[0].Luid);
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(hToken);
    return ok && GetLastError() == ERROR_SUCCESS;
}

// ═══════════════════════════════════════════════════════════════
// 管道协议常量
// ═══════════════════════════════════════════════════════════════

static const DWORD PIPE_TIMEOUT   = 30000;  // 30 秒超时
static const DWORD PIPE_BUF_SIZE  = 262144; // 256KB 缓冲区

// ═══════════════════════════════════════════════════════════════
// 服务端
// ═══════════════════════════════════════════════════════════════

static volatile bool g_serverRunning = false;

void NTFSPipeServer::stop()
{
    g_serverRunning = false;
}

void NTFSPipeServer::run()
{
    g_serverRunning = true;

    // 安全描述符: 允许 Everyone 读写 (否则普通用户连不上 SYSTEM 建的管道)
    SECURITY_DESCRIPTOR sd = {};
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE); // NULL DACL = 全员可访问

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    while (g_serverRunning) {
        HANDLE hPipe = CreateNamedPipeW(
            MFO_PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            PIPE_BUF_SIZE, PIPE_BUF_SIZE,
            PIPE_TIMEOUT, &sa);

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }

        // 等待客户端连接
        if (!ConnectNamedPipe(hPipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(hPipe);
            continue;
        }
        qDebug() << "[SVC] client connected";

        // ─── 读取请求 ───
        wchar_t buf[8192] = {};
        DWORD bytesRead = 0;
        if (!ReadFile(hPipe, buf, sizeof(buf) - 2, &bytesRead, nullptr) || bytesRead == 0) {
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            continue;
        }

        QString request = QString::fromWCharArray(buf, bytesRead / 2).trimmed();

        // 解析: "SCAN:rootPath|ext1|ext2|..."
        if (!request.startsWith("SCAN:")) {
            // 无效请求 → 返回错误
            const wchar_t* err = L"ERROR:Invalid request\n";
            DWORD w;
            WriteFile(hPipe, err, (DWORD)(wcslen(err) * 2), &w, nullptr);
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            continue;
        }

        QString body = request.mid(5); // 去掉 "SCAN:"
        int sep = body.indexOf('|');
        QString rootPath = (sep >= 0) ? body.left(sep) : body;
        QStringList filters;
        if (sep >= 0) {
            for (const QString& f : body.mid(sep + 1).split('|'))
                if (!f.isEmpty()) filters << f;
        }

        // ─── 执行 NTFS 快速扫描 ───
        enableBackupPrivilege();
        QVector<FileEntry> results = NTFSScanner::fastScan(rootPath, filters);

        // ─── 返回结果 + 诊断 ───
        QString diag = QString("DIAG:svc_scanned(%1 files)\n").arg(results.size());
        std::wstring wDiag = diag.toStdWString();
        DWORD written = 0;
        WriteFile(hPipe, wDiag.c_str(), (DWORD)(wDiag.size() * 2), &written, nullptr);
        (void)written;
        QString header = QString("COUNT:%1\n").arg(results.size());
        std::wstring wHeader = header.toStdWString();
        written = 0;
        WriteFile(hPipe, wHeader.c_str(), (DWORD)(wHeader.size() * 2), &written, nullptr);

        // FILE:size|path\n  (分批写入, 每次 100 条)
        QString batch;
        batch.reserve(65536);
        for (int i = 0; i < results.size(); ++i) {
            batch += QString("FILE:%1|%2\n")
                         .arg(results[i].fileSize)
                         .arg(results[i].filePath);
            if (batch.size() > 60000 || i == results.size() - 1) {
                std::wstring wBatch = batch.toStdWString();
                WriteFile(hPipe, wBatch.c_str(), (DWORD)(wBatch.size() * 2), &written, nullptr);
                batch.clear(); batch.reserve(65536);
            }
        }

        // DONE\n
        const wchar_t* done = L"DONE\n";
        WriteFile(hPipe, done, (DWORD)(wcslen(done) * 2), &written, nullptr);

        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

// ═══════════════════════════════════════════════════════════════
// 客户端
// ═══════════════════════════════════════════════════════════════

QVector<FileEntry> NTFSPipeClient::scanViaPipe(const QString& rootPath,
                                                 const QStringList& filters,
                                                 QString* outDiag)
{
    QVector<FileEntry> results;
    if (filters.isEmpty()) return results;

    HANDLE hPipe = CreateFileW(
        MFO_PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (hPipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (outDiag) *outDiag = QString("管道连接失败 err=%1").arg(err);
        return results;
    }

    // 设置超时
    DWORD mode = PIPE_READMODE_MESSAGE | PIPE_WAIT;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    // ─── 发送请求 ───
    QString req = "SCAN:" + rootPath + "|" + filters.join("|") + "\n";
    std::wstring wReq = req.toStdWString();
    DWORD written = 0;
    if (!WriteFile(hPipe, wReq.c_str(), (DWORD)(wReq.size() * 2), &written, nullptr)) {
        CloseHandle(hPipe);
        return results;
    }

    // ─── 读取响应 ───
    wchar_t buf[65536];
    QString leftover;
    int fileCount = -1;
    results.reserve(10000);

    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(hPipe, buf, sizeof(buf) - 2, &bytesRead, nullptr))
            break;
        if (bytesRead == 0) break;

        QString chunk = leftover + QString::fromWCharArray(buf, bytesRead / 2);
        leftover.clear();

        QStringList lines = chunk.split('\n');
        // 最后一行可能不完整
        if (!chunk.endsWith('\n')) {
            leftover = lines.takeLast();
        }

        for (const QString& line : lines) {
            if (line.startsWith("DIAG:")) {
                if (outDiag) *outDiag = line.mid(5);
            } else if (line.startsWith("COUNT:")) {
                fileCount = line.mid(6).toInt();
                results.reserve(fileCount);
            } else if (line.startsWith("FILE:")) {
                QString data = line.mid(5); // "size|path"
                int sep = data.indexOf('|');
                if (sep > 0) {
                    qint64 size = data.left(sep).toLongLong();
                    QString path = data.mid(sep + 1);
                    results.append(FileEntry(path, size));
                }
            } else if (line.startsWith("DONE")) {
                goto done;
            } else if (line.startsWith("ERROR:")) {
                results.clear();
                goto done;
            }
        }
    }
done:
    CloseHandle(hPipe);
    return results;
}

bool NTFSPipeClient::isServiceRunning()
{
    HANDLE hPipe = CreateFileW(MFO_PIPE_NAME, GENERIC_READ,
                                0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipe);
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════
// 服务安装/卸载
// ═══════════════════════════════════════════════════════════════

static const wchar_t* SVC_NAME = L"MediaFileOrganizer";

bool ServiceInstaller::install(const QString& exePath, QString* outError)
{
    QString nativePath = QDir::toNativeSeparators(exePath);
    std::wstring wPath = (nativePath + " /service").toStdWString();

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        if (outError) *outError = QString("OpenSCManager 失败 (错误 %1)\n请确认以管理员身份运行").arg(GetLastError());
        return false;
    }

    SC_HANDLE svc = CreateServiceW(
        scm, SVC_NAME, L"Media File Organizer",
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        wPath.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr);

    DWORD err = GetLastError();
    if (!svc) {
        if (err == ERROR_SERVICE_EXISTS) {
            svc = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS);
            if (!svc) {
                if (outError) *outError = "服务已存在但无法打开, 请先 /uninstall";
                CloseServiceHandle(scm);
                return false;
            }
        } else {
            if (outError) *outError = QString("CreateService 失败 (错误 %1)\n路径: %2").arg(err).arg(nativePath);
            CloseServiceHandle(scm);
            return false;
        }
    }

    SERVICE_DESCRIPTIONW desc = {};
    desc.lpDescription = (LPWSTR)L"为 MediaFileOrganizer 提供 NTFS 快速扫描服务";
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    if (!StartServiceW(svc, 0, nullptr)) {
        DWORD startErr = GetLastError();
        if (startErr != ERROR_SERVICE_ALREADY_RUNNING) {
            if (outError) *outError = QString("启动服务失败 (错误 %1)").arg(startErr);
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return false;
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

bool ServiceInstaller::uninstall(QString* /*outError*/)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return false;

    SC_HANDLE svc = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (!svc) { CloseServiceHandle(scm); return false; }

    // 停止服务
    SERVICE_STATUS status;
    ControlService(svc, SERVICE_CONTROL_STOP, &status);

    // 删除服务
    BOOL ok = DeleteService(svc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

bool ServiceInstaller::isInstalled()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, SVC_NAME, SERVICE_QUERY_STATUS);
    if (svc) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return true;
    }
    CloseServiceHandle(scm);
    return false;
}
