#include "device_store.h"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <windows.h>
#include <wincrypt.h>

namespace wwmm
{
VaultIO systemVault(const QString &directory, const QString &)
{
    const QString path = QDir(directory).filePath("identity.dpapi");
    return {[path](QByteArray &out, QString &error) {
                QFile file(path);
                if (!file.exists()) {
                    out.clear();
                    return true;
                }
                if (!file.open(QIODevice::ReadOnly) || file.size() > 131072) {
                    error = QObject::tr("无法读取本机设备身份。");
                    return false;
                }
                auto cipher = file.readAll();
                DATA_BLOB in{DWORD(cipher.size()), reinterpret_cast<BYTE *>(cipher.data())}, clear{};
                if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                                        &clear)) {
                    error = QObject::tr("Windows 无法解密当前用户的设备身份。");
                    return false;
                }
                out = QByteArray(reinterpret_cast<char *>(clear.pbData), clear.cbData);
                SecureZeroMemory(clear.pbData, clear.cbData);
                LocalFree(clear.pbData);
                if (out.isEmpty()) {
                    error = QObject::tr("设备身份记录为空。");
                    return false;
                }
                return true;
            },
            [path](const QByteArray &clear, QString &error) {
                DATA_BLOB in{DWORD(clear.size()),
                             reinterpret_cast<BYTE *>(const_cast<char *>(clear.constData()))},
                    cipher{};
                if (!CryptProtectData(&in, L"WWMM Remote", nullptr, nullptr, nullptr,
                                      CRYPTPROTECT_UI_FORBIDDEN, &cipher)) {
                    error = QObject::tr("Windows 无法保护设备身份。");
                    return false;
                }
                QSaveFile file(path);
                const bool ok =
                    file.open(QIODevice::WriteOnly) &&
                    file.write(reinterpret_cast<char *>(cipher.pbData), cipher.cbData) == cipher.cbData &&
                    file.commit();
                LocalFree(cipher.pbData);
                if (!ok)
                    error = QObject::tr("无法保存设备身份。");
                return ok;
            }};
}
} // namespace wwmm
