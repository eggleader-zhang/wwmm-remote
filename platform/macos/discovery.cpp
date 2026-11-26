#include "discovery.h"
#include <QSocketNotifier>
#include <arpa/inet.h>
#include <dns_sd.h>

namespace wwmm
{
namespace
{
constexpr char type[] = "_wwmm-remote._tcp";

QString bonjourError(DNSServiceErrorType status)
{
    if (status == kDNSServiceErr_PolicyDenied || status == kDNSServiceErr_NoAuth)
        return QObject::tr("请在系统设置的「隐私与安全性 → 本地网络」中允许 WWMM Remote。");
    return QObject::tr("Bonjour 暂时不可用（%1）。").arg(status);
}

class Operation final : public QObject
{
public:
    Operation(DnsSdBackend *owner, QString instance = {})
        : QObject(owner), owner(owner), instance(std::move(instance))
    {
    }
    ~Operation() override
    {
        delete notifier;
        if (ref)
            DNSServiceRefDeallocate(ref);
    }
    bool watch(DNSServiceErrorType error)
    {
        if (error || !ref) {
            emit owner->problem(bonjourError(error));
            deleteLater();
            return false;
        }
        notifier = new QSocketNotifier(DNSServiceRefSockFD(ref), QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this] {
            if (const auto err = DNSServiceProcessResult(ref)) {
                notifier->setEnabled(false);
                emit owner->problem(bonjourError(err));
            }
        });
        return true;
    }
    DnsSdBackend *owner;
    QString instance;
    DNSServiceRef ref = nullptr;
    QSocketNotifier *notifier = nullptr;
};

class MacDnsSd final : public DnsSdBackend
{
public:
    using DnsSdBackend::DnsSdBackend;
    void start(const QList<unsigned> &interfaces, const QList<Advertisement> &ads) override
    {
        for (auto i : interfaces) {
            auto *op = new Operation(this);
            op->watch(DNSServiceBrowse(&op->ref, 0, i, type, "local.", browse, this));
        }
        for (const auto &ad : ads) {
            TXTRecordRef txt;
            TXTRecordCreate(&txt, 0, nullptr);
            DNSServiceErrorType error = kDNSServiceErr_NoError;
            for (auto it = ad.fields.begin(); it != ad.fields.end() && !error; ++it) {
                const auto value = it.value().toUtf8();
                error = TXTRecordSetValue(&txt, it.key().toLatin1().constData(),
                                          static_cast<uint8_t>(value.size()), value.constData());
            }
            auto *op = new Operation(this);
            if (!error)
                error = DNSServiceRegister(&op->ref, 0, ad.local.link.interfaceIndex,
                                           ad.instance.toLatin1().constData(), type, "local.", nullptr,
                                           htons(ad.local.port), TXTRecordGetLength(&txt),
                                           TXTRecordGetBytesPtr(&txt), registered, this);
            TXTRecordDeallocate(&txt);
            op->watch(error);
        }
    }

private:
    QMap<QPair<unsigned, QString>, Operation *> resolving_;
    static void registered(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType error, const char *,
                           const char *, const char *, void *context)
    {
        if (error)
            emit static_cast<MacDnsSd *>(context)->problem(bonjourError(error));
    }
    static void browse(DNSServiceRef, DNSServiceFlags flags, uint32_t index, DNSServiceErrorType error,
                       const char *name, const char *service, const char *domain, void *context)
    {
        auto *self = static_cast<MacDnsSd *>(context);
        if (error) {
            emit self->problem(bonjourError(error));
            return;
        }
        const auto label = QString::fromUtf8(name).toLower();
        const auto key = qMakePair(index, label);
        if (!(flags & kDNSServiceFlagsAdd)) {
            if (auto *op = self->resolving_.take(key)) {
                op->notifier->setEnabled(false);
                op->deleteLater();
            }
            emit self->removed(index, label);
            return;
        }
        if (self->resolving_.contains(key) || self->resolving_.size() >= 256)
            return;
        auto *op = new Operation(self, label);
        if (op->watch(DNSServiceResolve(&op->ref, 0, index, name, service, domain, resolve, op)))
            self->resolving_.insert(key, op);
    }
    static void resolve(DNSServiceRef, DNSServiceFlags, uint32_t index, DNSServiceErrorType error,
                        const char *, const char *, uint16_t port, uint16_t length, const unsigned char *txt,
                        void *context)
    {
        auto *op = static_cast<Operation *>(context);
        auto *owner = static_cast<MacDnsSd *>(op->owner);
        const auto key = qMakePair(index, op->instance);
        owner->resolving_.remove(key);
        op->notifier->setEnabled(false);
        op->deleteLater();
        if (error) {
            emit owner->removed(index, op->instance);
            return;
        }
        ServiceFields fields;
        for (auto key : {"v", "pk", "name", "addr", "os", "desk", "input", "office"}) {
            uint8_t size = 0;
            auto data = static_cast<const char *>(TXTRecordGetValuePtr(length, txt, key, &size));
            if (data)
                fields.insert(QString::fromLatin1(key), QString::fromUtf8(data, size));
        }
        emit op->owner->resolved(index, op->instance, ntohs(port), fields);
    }
};
} // namespace
DnsSdBackend *nativeDnsSd(QObject *parent)
{
    return new MacDnsSd(parent);
}
} // namespace wwmm
