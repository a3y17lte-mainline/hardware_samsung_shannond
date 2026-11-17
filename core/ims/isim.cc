// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ims/isim.h"

#include <algorithm>
#include <memory>

extern "C" {
#include <samsung-ipc.h>
}

namespace shannond::ims {

namespace {

constexpr uint16_t kEfImpi = 0x6f02;
constexpr uint16_t kEfDomain = 0x6f03;
constexpr uint16_t kEfImpu = 0x6f04;
constexpr uint16_t kEfAd = 0x6fad;
const std::vector<uint16_t> kAdfPath{0x3f00, 0x7fff};
constexpr size_t kMaxImpus = 4;

bool ok(const SimIoResult &r)
{
    return r.sw1 == 0x90 || r.sw1 == 0x91;
}

const SimApp *findApp(const Sim &sim, uint8_t type)
{
    for (const auto &a : sim.apps())
        if (a.type == type)
            return &a;
    return nullptr;
}

// Reads a whole transparent file, or the first records of a linear fixed one
void readFile(Sim &sim, const SimApp *app, uint16_t fileId,
              std::function<void(std::optional<std::vector<Bytes>>)> done)
{
    sim.io(IPC_SEC_RSIM_COMMAND_GET_RESPONSE, fileId, kAdfPath, 0, 0, 0, {}, app,
           [&sim, app, fileId, done](std::optional<SimIoResult> r) {
               auto info = r && ok(*r) ? parseFcp(r->data) : std::nullopt;
               if (!info) {
                   done(std::nullopt);
                   return;
               }
               if (!info->recordLength) {
                   if (!info->size || info->size > 255) {
                       done(std::nullopt);
                       return;
                   }
                   sim.io(IPC_SEC_RSIM_COMMAND_READ_BINARY, fileId, kAdfPath, 0, 0,
                          static_cast<uint8_t>(info->size), {}, app, [done](std::optional<SimIoResult> r) {
                              if (!r || !ok(*r))
                                  done(std::nullopt);
                              else
                                  done(std::vector<Bytes>{r->data});
                          });
                   return;
               }
               if (info->recordLength > 255) {
                   done(std::nullopt);
                   return;
               }
               auto records = std::make_shared<std::vector<Bytes>>();
               auto next = std::make_shared<std::function<void(size_t)>>();
               const size_t count = std::min(info->records, kMaxImpus);
               const auto length = static_cast<uint8_t>(info->recordLength);
               *next = [&sim, app, fileId, done, records, next, count, length](size_t n) {
                   if (n > count) {
                       done(*records);
                       *next = nullptr;  // break the cycle
                       return;
                   }
                   sim.io(IPC_SEC_RSIM_COMMAND_READ_RECORD, fileId, kAdfPath, static_cast<uint8_t>(n), 0x04,
                          length, {}, app, [done, records, next, n](std::optional<SimIoResult> r) {
                              if (!r || !ok(*r)) {
                                  done(records->empty() ? std::nullopt : std::optional(*records));
                                  *next = nullptr;
                                  return;
                              }
                              records->push_back(r->data);
                              (*next)(n + 1);
                          });
               };
               (*next)(1);
           });
}

void fromImsi(Sim &sim, const std::string &plmn, std::function<void(std::optional<ImsIdentity>)> done)
{
    const SimApp *usim = findApp(sim, kSimAppUsim);
    sim.io(IPC_SEC_RSIM_COMMAND_READ_BINARY, kEfAd, kAdfPath, 0, 0, 4, {}, usim,
           [&sim, plmn, done](std::optional<SimIoResult> r) {
               unsigned mnc = r && ok(*r) ? mncLengthFromAd(r->data) : 0;
               const std::string &imsi = sim.imsi();
               if (!mnc && (plmn.size() == 5 || plmn.size() == 6) && imsi.rfind(plmn, 0) == 0)
                   mnc = plmn.size() - 3;
               done(identityFromImsi(imsi, mnc ? mnc : 2));
           });
}

}  // namespace

std::optional<FileInfo> parseFcp(const Bytes &fcp)
{
    if (fcp.size() < 2 || fcp[0] != 0x62)
        return std::nullopt;
    size_t end = std::min<size_t>(fcp.size(), 2 + fcp[1]);
    FileInfo info;
    bool any = false;
    for (size_t i = 2; i + 2 <= end;) {
        uint8_t tag = fcp[i], length = fcp[i + 1];
        if (i + 2 + length > end)
            break;
        const uint8_t *v = &fcp[i + 2];
        if (tag == 0x80 && length >= 2) {
            info.size = v[0] << 8 | v[1];
            any = true;
        } else if (tag == 0x82 && length >= 5 && (v[0] & 0x07) == 0x02) {  // linear fixed
            info.recordLength = v[2] << 8 | v[3];
            info.records = v[4];
            any = true;
        }
        i += 2 + length;
    }
    if (!any)
        return std::nullopt;
    return info;
}

void readImsIdentity(Sim &sim, const std::string &plmn, std::function<void(std::optional<ImsIdentity>)> done)
{
    const SimApp *isim = findApp(sim, kSimAppIsim);
    if (!isim) {
        fromImsi(sim, plmn, done);
        return;
    }
    auto id = std::make_shared<ImsIdentity>();
    id->fromIsim = true;
    auto fallBack = [&sim, plmn, done]() { fromImsi(sim, plmn, done); };
    readFile(sim, isim, kEfImpi, [&sim, isim, id, done, fallBack](std::optional<std::vector<Bytes>> impi) {
        auto value = impi && !impi->empty() ? isimTlv(impi->front()) : std::nullopt;
        if (!value) {
            fallBack();
            return;
        }
        id->impi = *value;
        readFile(sim, isim, kEfDomain, [&sim, isim, id, done, fallBack](std::optional<std::vector<Bytes>> domain) {
            auto value = domain && !domain->empty() ? isimTlv(domain->front()) : std::nullopt;
            if (!value) {
                fallBack();
                return;
            }
            id->domain = *value;
            readFile(sim, isim, kEfImpu, [id, done, fallBack](std::optional<std::vector<Bytes>> impus) {
                if (impus)
                    for (const Bytes &record : *impus)
                        if (auto value = isimTlv(record))
                            id->impus.push_back(*value);
                if (!id->valid()) {
                    fallBack();
                    return;
                }
                // The SIP URI registers; a tel: URI is an alias
                auto sip = std::find_if(id->impus.begin(), id->impus.end(),
                                        [](const std::string &u) { return u.rfind("sip:", 0) == 0; });
                if (sip == id->impus.end()) {
                    fallBack();
                    return;
                }
                std::rotate(id->impus.begin(), sip, sip + 1);
                done(*id);
            });
        });
    });
}

}  // namespace shannond::ims
