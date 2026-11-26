#include "device_store.h"
#include <Security/Security.h>

namespace wwmm
{
namespace
{
CFMutableDictionaryRef item(const QString &account)
{
    auto query = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, CFSTR("dev.wwmm.remote.pairing.v1"));
    const auto label = account.toCFString();
    CFDictionarySetValue(query, kSecAttrAccount, label);
    CFRelease(label);
    return query;
}
bool check(OSStatus status, QString &error)
{
    if (status == errSecSuccess)
        return true;
    error = QObject::tr("系统钥匙串不可用（%1）。").arg(status);
    return false;
}
} // namespace
VaultIO systemVault(const QString &, const QString &account)
{
    return {[account](QByteArray &out, QString &error) {
                auto query = item(account);
                CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
                CFTypeRef value = nullptr;
                const auto status = SecItemCopyMatching(query, &value);
                CFRelease(query);
                if (status == errSecItemNotFound) {
                    out.clear();
                    return true;
                }
                if (!check(status, error))
                    return false;
                auto data = static_cast<CFDataRef>(value);
                out =
                    QByteArray(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), CFDataGetLength(data));
                CFRelease(value);
                if (out.isEmpty()) {
                    error = QObject::tr("钥匙串设备记录为空。");
                    return false;
                }
                return true;
            },
            [account](const QByteArray &in, QString &error) {
                auto query = item(account);
                auto data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(in.constData()), in.size());
                const void *keys[]{kSecValueData};
                const void *values[]{data};
                auto changes = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
                auto status = SecItemUpdate(query, changes);
                if (status == errSecItemNotFound) {
                    CFDictionarySetValue(query, kSecValueData, data);
                    CFDictionarySetValue(query, kSecAttrLabel, CFSTR("WWMM Remote"));
                    status = SecItemAdd(query, nullptr);
                }
                CFRelease(changes);
                CFRelease(data);
                CFRelease(query);
                return check(status, error);
            }};
}
} // namespace wwmm
