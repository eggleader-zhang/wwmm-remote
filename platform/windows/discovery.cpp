#include "discovery.h"
#include <QSysInfo>
#include <winsock2.h>
#include <windns.h>
#include <cstring>
#include <memory>
#include <mutex>

namespace wwmm
{
namespace
{
const QString service = QStringLiteral("_wwmm-remote._tcp.local");
class WinDnsSd;
struct Call {
    enum Kind { Browse, Resolve, Register } kind;
    quintptr id;
    WinDnsSd *owner;
    unsigned index;
    QString name;
    std::wstring query;
    DNS_SERVICE_CANCEL cancel{};
    DNS_SERVICE_BROWSE_REQUEST browse{};
    DNS_SERVICE_RESOLVE_REQUEST resolve{};
    DNS_SERVICE_REGISTER_REQUEST registration{};
    bool published = false;
    bool withdrawing = false;
    bool completed = false;
    ~Call()
    {
        if (registration.pServiceInstance)
            DnsServiceFreeInstance(registration.pServiceInstance);
    }
    void *context() const
    {
        return reinterpret_cast<void *>(id);
    }
};
using Ref = std::shared_ptr<Call>;
// dnsapi calls back on system threads. A token can outlive its Qt receiver.
// One-shot request buffers stay alive until native completion, including deregistration.
struct Calls {
    std::mutex mutex;
    QHash<quintptr, Ref> active;
    quintptr next = 1;
};
Calls &calls()
{
    static auto *registry = new Calls;
    return *registry;
}
Ref lookup(void *token)
{
    std::lock_guard lock(calls().mutex);
    return calls().active.value(reinterpret_cast<quintptr>(token));
}
void release(const Ref &op)
{
    std::lock_guard lock(calls().mutex);
    calls().active.remove(op->id);
}
void withdraw(const Ref &op)
{
    if (DnsServiceDeRegister(&op->registration, nullptr) != DNS_REQUEST_PENDING)
        release(op);
}
void detach(const Ref &op)
{
    bool deregister = false;
    {
        std::lock_guard lock(calls().mutex);
        op->owner = nullptr;
        if (op->completed || !calls().active.contains(op->id))
            return;
        if (op->kind == Call::Register && op->published && !op->withdrawing)
            deregister = op->withdrawing = true;
    }
    if (deregister)
        withdraw(op);
    else if (op->kind == Call::Browse) {
        // Successful cancellation delivers a final ERROR_CANCELLED callback.
        if (DnsServiceBrowseCancel(&op->cancel) != ERROR_SUCCESS)
            release(op);
    } else if (op->kind == Call::Resolve) {
        DnsServiceResolveCancel(&op->cancel);
    }
}

class WinDnsSd final : public DnsSdBackend
{
public:
    using DnsSdBackend::DnsSdBackend;
    ~WinDnsSd() override
    {
        for (const auto &op : lasting_)
            detach(op);
        for (const auto &op : resolving_)
            detach(op);
    }
    void start(const QList<unsigned> &interfaces, const QList<Advertisement> &ads) override
    {
        for (auto index : interfaces) {
            auto op = make(Call::Browse, index, service);
            op->browse.Version = DNS_QUERY_REQUEST_VERSION1;
            op->browse.InterfaceIndex = index;
            op->browse.QueryName = op->query.c_str();
            op->browse.pBrowseCallback = browsed;
            op->browse.pQueryContext = op->context();
            const auto status = DnsServiceBrowse(&op->browse, &op->cancel);
            if (started(op, status))
                lasting_.append(op);
        }
        for (const auto &ad : ads)
            advertise(ad);
    }

private:
    QList<Ref> lasting_;
    QMap<QPair<unsigned, QString>, Ref> resolving_;
    Ref make(Call::Kind kind, unsigned index, const QString &name)
    {
        auto op = std::make_shared<Call>();
        op->kind = kind;
        op->owner = this;
        op->index = index;
        op->name = name;
        op->query = name.toStdWString();
        std::lock_guard lock(calls().mutex);
        op->id = calls().next++;
        calls().active.insert(op->id, op);
        return op;
    }
    bool started(const Ref &op, DWORD status)
    {
        if (status == DNS_REQUEST_PENDING)
            return true;
        release(op);
        emit problem(tr("Windows 局域网发现不可用（%1）。").arg(status));
        return false;
    }
    template <typename Fn> static void post(const Ref &op, Fn fn)
    {
        std::lock_guard lock(calls().mutex);
        if (auto *owner = op->owner)
            QMetaObject::invokeMethod(
                owner, [owner, fn = std::move(fn)] { fn(owner); }, Qt::QueuedConnection);
    }
    static void WINAPI browsed(DWORD status, void *token, PDNS_RECORD records)
    {
        const auto op = lookup(token);
        if (op && status == ERROR_SUCCESS) {
            for (auto *r = records; r; r = r->pNext) {
                if (r->wType != DNS_TYPE_PTR || !r->Data.PTR.pNameHost)
                    continue;
                QString name;
                if (r->Flags.S.CharSet == DnsCharSetUtf8 || r->Flags.S.CharSet == DnsCharSetAnsi)
                    name = QString::fromUtf8(reinterpret_cast<const char *>(r->Data.PTR.pNameHost));
                else
                    name = QString::fromWCharArray(r->Data.PTR.pNameHost);
                name = name.toLower();
                if (name.endsWith('.'))
                    name.chop(1);
                const auto labelSize = name.section('.', 0, 0).size();
                if (!name.endsWith("." + service) || labelSize < 34 || labelSize > 63 ||
                    !name.startsWith("w-") || name.count('.') != 3)
                    continue;
                const bool gone = r->dwTtl == 0 || r->Flags.S.Delete;
                post(op,
                     [index = op->index, name, gone](WinDnsSd *self) { self->instance(index, name, gone); });
            }
        } else if (op && status == ERROR_CANCELLED) {
            release(op);
        } else if (op) {
            post(op, [status](WinDnsSd *self) {
                emit self->problem(tr("Windows 浏览服务中断（%1）。").arg(status));
            });
        }
        if (records)
            DnsRecordListFree(records, DnsFreeRecordList);
    }
    void instance(unsigned index, const QString &name, bool gone)
    {
        const auto key = qMakePair(index, name);
        if (gone) {
            if (auto op = resolving_.take(key))
                detach(op);
            emit removed(index, name);
            return;
        }
        if (resolving_.contains(key) || resolving_.size() >= 128)
            return;
        auto op = make(Call::Resolve, index, name);
        op->resolve.Version = DNS_QUERY_REQUEST_VERSION1;
        op->resolve.InterfaceIndex = index;
        op->resolve.QueryName = op->query.data();
        op->resolve.pResolveCompletionCallback = resolvedCallback;
        op->resolve.pQueryContext = op->context();
        if (started(op, DnsServiceResolve(&op->resolve, &op->cancel)))
            resolving_.insert(key, op);
    }
    static void WINAPI resolvedCallback(DWORD status, void *token, PDNS_SERVICE_INSTANCE instance)
    {
        const auto op = lookup(token);
        if (op) {
            {
                std::lock_guard lock(calls().mutex);
                op->completed = true;
            }
            ServiceFields fields;
            quint16 port = 0;
            if (status == ERROR_SUCCESS && instance && instance->dwPropertyCount <= 16 && instance->keys &&
                instance->values) {
                port = instance->wPort;
                for (DWORD i = 0; i < instance->dwPropertyCount; ++i)
                    if (instance->keys[i] && instance->values[i])
                        fields.insert(QString::fromWCharArray(instance->keys[i]).toLower(),
                                      QString::fromWCharArray(instance->values[i]));
            }
            post(op, [op, fields, port](WinDnsSd *self) {
                auto key = qMakePair(op->index, op->name);
                if (self->resolving_.value(key) != op)
                    return;
                self->resolving_.remove(key);
                if (port)
                    emit self->resolved(op->index, op->name, port, fields);
            });
            release(op);
        }
        if (instance)
            DnsServiceFreeInstance(instance);
    }
    void advertise(const Advertisement &ad)
    {
        const auto &link = ad.local.link;
        auto op = make(Call::Register, link.interfaceIndex, ad.instance + "." + service);
        QList<std::wstring> keys, values;
        for (auto it = ad.fields.begin(); it != ad.fields.end(); ++it) {
            keys.append(it.key().toStdWString());
            values.append(it.value().toStdWString());
        }
        QList<PCWSTR> keyPtrs, valuePtrs;
        for (const auto &k : std::as_const(keys))
            keyPtrs.append(k.c_str());
        for (const auto &v : std::as_const(values))
            valuePtrs.append(v.c_str());
        IP4_ADDRESS v4 = htonl(link.address.toIPv4Address());
        IP6_ADDRESS v6{};
        auto qt6 = link.address.toIPv6Address();
        std::memcpy(&v6, qt6.c, 16);
        const bool ipv4 = link.address.protocol() == QAbstractSocket::IPv4Protocol;
        const auto host = (QSysInfo::machineHostName().section('.', 0, 0) + ".local").toStdWString();
        auto &request = op->registration;
        request.Version = DNS_QUERY_REQUEST_VERSION1;
        request.InterfaceIndex = link.interfaceIndex;
        request.pServiceInstance = DnsServiceConstructInstance(
            op->query.c_str(), host.c_str(), ipv4 ? &v4 : nullptr, ipv4 ? nullptr : &v6, ad.local.port, 0, 0,
            DWORD(keys.size()), keyPtrs.data(), valuePtrs.data());
        request.pQueryContext = op->context();
        request.pRegisterCompletionCallback = registered;
        if (!request.pServiceInstance) {
            started(op, ERROR_INVALID_DATA);
            return;
        }
        if (started(op, DnsServiceRegister(&request, nullptr)))
            lasting_.append(op);
    }
    static void WINAPI registered(DWORD status, void *token, PDNS_SERVICE_INSTANCE instance)
    {
        if (instance)
            DnsServiceFreeInstance(instance);
        auto op = lookup(token);
        if (!op)
            return;
        bool deregister = false;
        {
            std::lock_guard lock(calls().mutex);
            if (op->withdrawing) {
                calls().active.remove(op->id);
                return;
            }
            op->published = status == ERROR_SUCCESS;
            if (op->published && !op->owner)
                deregister = op->withdrawing = true;
        }
        if (deregister)
            withdraw(op);
        if (status != ERROR_SUCCESS) {
            post(op, [status](WinDnsSd *self) {
                emit self->problem(tr("无法发布本机设备（%1）。").arg(status));
            });
            release(op);
        }
    }
};
} // namespace
DnsSdBackend *nativeDnsSd(QObject *parent)
{
    return new WinDnsSd(parent);
}
} // namespace wwmm
