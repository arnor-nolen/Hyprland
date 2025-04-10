#include "IdleNotify.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"

static int onTimer(SP<CEventLoopTimer> self, void* data) {

    const auto NOTIF = (CExtIdleNotification*)data;

    NOTIF->onTimerFired();

    return 0;
}

CExtIdleNotification::CExtIdleNotification(SP<CExtIdleNotificationV1> resource_, uint32_t timeoutMs_, bool obeyInhibitors_) :
    resource(resource_), timeoutMs(timeoutMs_), obeyInhibitors(obeyInhibitors_) {
    if UNLIKELY (!resource_->resource())
        return;

    resource->setDestroy([this](CExtIdleNotificationV1* r) { PROTO::idle->destroyNotification(this); });
    resource->setOnDestroy([this](CExtIdleNotificationV1* r) { PROTO::idle->destroyNotification(this); });

    timer = makeShared<CEventLoopTimer>(std::nullopt, onTimer, this);
    g_pEventLoopManager->addTimer(timer);

    updateTimer();

    LOGM(LOG, "Registered idle-notification for {}ms", timeoutMs_);
}

CExtIdleNotification::~CExtIdleNotification() {
    g_pEventLoopManager->removeTimer(timer);
    timer.reset();
}

bool CExtIdleNotification::good() {
    return resource->resource();
}

void CExtIdleNotification::updateTimer() {
    if (PROTO::idle->isInhibited && obeyInhibitors)
        timer->updateTimeout(std::nullopt);
    else
        timer->updateTimeout(std::chrono::milliseconds(timeoutMs));
}

void CExtIdleNotification::onTimerFired() {
    resource->sendIdled();
    idled = true;
}

void CExtIdleNotification::onActivity() {
    if (idled)
        resource->sendResumed();

    idled = false;
    updateTimer();
}

bool CExtIdleNotification::inhibitorsAreObeyed() const {
    return obeyInhibitors;
}

CIdleNotifyProtocol::CIdleNotifyProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CIdleNotifyProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_vManagers.emplace_back(makeUnique<CExtIdleNotifierV1>(client, ver, id)).get();
    RESOURCE->setOnDestroy([this](CExtIdleNotifierV1* p) { this->onManagerResourceDestroy(p->resource()); });

    RESOURCE->setDestroy([this](CExtIdleNotifierV1* pMgr) { this->onManagerResourceDestroy(pMgr->resource()); });
    RESOURCE->setGetIdleNotification(
        [this](CExtIdleNotifierV1* pMgr, uint32_t id, uint32_t timeout, wl_resource* seat) { this->onGetNotification(pMgr, id, timeout, seat, true); });
    RESOURCE->setGetInputIdleNotification(
        [this](CExtIdleNotifierV1* pMgr, uint32_t id, uint32_t timeout, wl_resource* seat) { this->onGetNotification(pMgr, id, timeout, seat, false); });
}

void CIdleNotifyProtocol::onManagerResourceDestroy(wl_resource* res) {
    std::erase_if(m_vManagers, [&](const auto& other) { return other->resource() == res; });
}

void CIdleNotifyProtocol::destroyNotification(CExtIdleNotification* notif) {
    std::erase_if(m_vNotifications, [&](const auto& other) { return other.get() == notif; });
}

void CIdleNotifyProtocol::onGetNotification(CExtIdleNotifierV1* pMgr, uint32_t id, uint32_t timeout, wl_resource* seat, bool obeyInhibitors) {
    const auto CLIENT = pMgr->client();
    const auto RESOURCE =
        m_vNotifications.emplace_back(makeShared<CExtIdleNotification>(makeShared<CExtIdleNotificationV1>(CLIENT, pMgr->version(), id), timeout, obeyInhibitors)).get();

    if UNLIKELY (!RESOURCE->good()) {
        pMgr->noMemory();
        m_vNotifications.pop_back();
        return;
    }
}

void CIdleNotifyProtocol::onActivity() {
    for (auto const& n : m_vNotifications) {
        n->onActivity();
    }
}

void CIdleNotifyProtocol::setInhibit(bool inhibited) {
    isInhibited = inhibited;
    for (auto const& n : m_vNotifications) {
        if (n->inhibitorsAreObeyed())
            n->onActivity();
    }
}
